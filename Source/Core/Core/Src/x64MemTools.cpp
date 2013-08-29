// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2
// Refer to the license.txt file included.

#ifdef _WIN32
#include <windows.h>
#else
#include <stdio.h>
#include <signal.h>
#ifndef ANDROID
#include <sys/ucontext.h>   // Look in here for the context definition.
#endif
#endif

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/message.h>
#include "Thread.h"
#endif

#ifdef __APPLE__
#define CREG_RAX(ctx) (*(ctx))->__ss.__rax
#define CREG_RIP(ctx) (*(ctx))->__ss.__rip
#define CREG_EAX(ctx) (*(ctx))->__ss.__eax
#define CREG_EIP(ctx) (*(ctx))->__ss.__eip
#elif defined __FreeBSD__
#define CREG_RAX(ctx) (ctx)->mc_rax
#define CREG_RIP(ctx) (ctx)->mc_rip
#define CREG_EAX(ctx) (ctx)->mc_eax
#define CREG_EIP(ctx) (ctx)->mc_eip
#elif defined __linux__
#define CREG_RAX(ctx) (ctx)->gregs[REG_RAX]
#define CREG_RIP(ctx) (ctx)->gregs[REG_RIP]
#define CREG_EAX(ctx) (ctx)->gregs[REG_EAX]
#define CREG_EIP(ctx) (ctx)->gregs[REG_EIP]
#elif defined __NetBSD__
#define CREG_RAX(ctx) (ctx)->__gregs[_REG_RAX]
#define CREG_RIP(ctx) (ctx)->__gregs[_REG_RIP]
#define CREG_EAX(ctx) (ctx)->__gregs[_REG_EAX]
#define CREG_EIP(ctx) (ctx)->__gregs[_REG_EIP]
#endif

#include <vector>

#include "Common.h"
#include "MemTools.h"
#include "HW/Memmap.h"
#include "PowerPC/PowerPC.h"
#include "PowerPC/JitInterface.h"
#ifndef _M_GENERIC
#include "PowerPC/JitCommon/JitBase.h"
#endif
#include "x64Analyzer.h"

namespace EMM
{

#if defined __APPLE__ || defined __linux__ || defined __FreeBSD__
#include <execinfo.h>
void print_trace(const char * msg)
{
	void *array[100];
	size_t size;
	char **strings;
	size_t i;

	size = backtrace(array, 100);
	strings = backtrace_symbols(array, size);
	printf("%s Obtained %u stack frames.\n", msg, (unsigned int)size);
	for (i = 0; i < size; i++)
		printf("--> %s\n", strings[i]);
	free(strings);
}
#endif

bool DoFault(u64 bad_address, bool is_write, CONTEXT *ctx)
{
	if (!JitInterface::IsInCodeSpace((u8*) CONTEXT_PC(ctx)))
	{
		// Let's not prevent debugging.
		return false;
	}

	u64 memspace_bottom = (u64)Memory::base;
	u64 memspace_top = memspace_bottom +
#ifdef _M_X64
		0x100000000ULL;
#else
		0x40000000;
#endif

	if (bad_address < memspace_bottom || bad_address >= memspace_top) {
		return false;
	}
	u32 em_address = (u32)(bad_address - memspace_bottom);
	const u8 *new_pc = jit->BackPatch((u8*) CONTEXT_PC(ctx), is_write, em_address, ctx);
	if (new_pc)
	{
		CONTEXT_PC(ctx) = (u64) new_pc;
	}

	return true;
}

#ifdef _WIN32

LONG NTAPI Handler(PEXCEPTION_POINTERS pPtrs)
{
	switch (pPtrs->ExceptionRecord->ExceptionCode)
	{
	case EXCEPTION_ACCESS_VIOLATION:
		{
			int accessType = (int)pPtrs->ExceptionRecord->ExceptionInformation[0];
			if (accessType == 8) //Rule out DEP
			{
				return (DWORD)EXCEPTION_CONTINUE_SEARCH;
			}

			//Where in the x86 code are we?
			PVOID codeAddr = pPtrs->ExceptionRecord->ExceptionAddress;
			unsigned char *codePtr = (unsigned char*)codeAddr;
			u64 badAddress = (u64)pPtrs->ExceptionRecord->ExceptionInformation[1];
			CONTEXT *ctx = pPtrs->ContextRecord;

			if (DoFault(badAddress, accessType == 1, ctx))
			{
				return (DWORD)EXCEPTION_CONTINUE_EXECUTION;
			}
			else
			{
				// Let's not prevent debugging.
				return (DWORD)EXCEPTION_CONTINUE_SEARCH;
			}

		}

	case EXCEPTION_STACK_OVERFLOW:
		MessageBox(0, _T("Stack overflow!"), 0,0);
		return EXCEPTION_CONTINUE_SEARCH;

	case EXCEPTION_ILLEGAL_INSTRUCTION:
		//No SSE support? Or simply bad codegen?
		return EXCEPTION_CONTINUE_SEARCH;
		
	case EXCEPTION_PRIV_INSTRUCTION:
		//okay, dynarec codegen is obviously broken.
		return EXCEPTION_CONTINUE_SEARCH;

	case EXCEPTION_IN_PAGE_ERROR:
		//okay, something went seriously wrong, out of memory?
		return EXCEPTION_CONTINUE_SEARCH;

	case EXCEPTION_BREAKPOINT:
		//might want to do something fun with this one day?
		return EXCEPTION_CONTINUE_SEARCH;

	default:
		return EXCEPTION_CONTINUE_SEARCH;
	}
}

void InstallExceptionHandler()
{
#ifdef _M_X64
	// Make sure this is only called once per process execution
	// Instead, could make a Uninstall function, but whatever..
	static bool handlerInstalled = false;
	if (handlerInstalled)
		return;

	AddVectoredExceptionHandler(TRUE, Handler);
	handlerInstalled = true;
#endif
}

#elif defined(__APPLE__)

void CheckKR(const char* name, kern_return_t kr)
{
	if (kr)
	{
		PanicAlertT("%s failed: kr=%x", name, kr);
	}
}

#ifdef _M_X64
void ExceptionThread(mach_port_t port)
{
	Common::SetCurrentThreadName("Mach exception thread");
	#pragma pack(4)
	struct
	{
		mach_msg_header_t Head;
		NDR_record_t NDR;
		exception_type_t exception;
		mach_msg_type_number_t codeCnt;
		int64_t code[2];
		int flavor;
		mach_msg_type_number_t old_stateCnt;
		natural_t old_state[224];
	} msg_in;

	struct
	{
		mach_msg_header_t Head;
		NDR_record_t NDR;
		kern_return_t RetCode;
		int flavor;
		mach_msg_type_number_t new_stateCnt;
		natural_t new_state[224];
	} msg_out;
	#pragma pack()
	memset(&msg_in, 0xee, sizeof(msg_in));
	memset(&msg_out, 0xee, sizeof(msg_out));
	mach_msg_header_t *send_msg = NULL;
	mach_msg_size_t send_size = 0;
	mach_msg_option_t option = MACH_RCV_MSG;
	while (1)
	{

		CheckKR("mach_msg_overwrite recv", mach_msg_overwrite(send_msg, option, send_size, sizeof(msg_in), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL, &msg_in.Head, 0));

		if (msg_in.Head.msgh_id == MACH_NOTIFY_NO_SENDERS)
		{
			// the other thread exited
			mach_port_destroy(mach_task_self(), port);
			return;
		}

		if (msg_in.Head.msgh_id != 2406)
		{
			PanicAlertT("unknown message received");
			return;
		}

		if (msg_in.flavor != x86_THREAD_STATE64)
		{
			PanicAlertT("unknown flavor %d (expected %d)", msg_in.flavor, x86_THREAD_STATE64);
			return;
		}

		x86_thread_state64_t *state = (x86_thread_state64_t *) msg_in.old_state;
		CONTEXT fake_ctx;
		fake_ctx.Rax = state->__rax;
		fake_ctx.Rip = state->__rip;

		bool ok = DoFault(msg_in.code[1], 0, &fake_ctx);

		state->__rax = fake_ctx.Rax;
		state->__rip = fake_ctx.Rip;

		msg_out.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(msg_in.Head.msgh_bits), 0);
		msg_out.Head.msgh_remote_port = msg_in.Head.msgh_remote_port;
		msg_out.Head.msgh_local_port = MACH_PORT_NULL;
		msg_out.Head.msgh_id = msg_in.Head.msgh_id + 100;
		msg_out.NDR = msg_in.NDR;
		if (ok)
		{
			msg_out.RetCode = KERN_SUCCESS;
			msg_out.flavor = x86_THREAD_STATE64;
			msg_out.new_stateCnt = x86_THREAD_STATE64_COUNT;
			memcpy(msg_out.new_state, msg_in.old_state, x86_THREAD_STATE64_COUNT * sizeof(natural_t));
		}
		else
		{
			// pass it down
			msg_out.RetCode = KERN_FAILURE;
			msg_out.flavor = 0;
			msg_out.new_stateCnt = 0;
		}
		msg_out.Head.msgh_size = offsetof(typeof(msg_out), new_state) + msg_out.new_stateCnt * sizeof(natural_t);

		send_msg = &msg_out.Head;
		send_size = msg_out.Head.msgh_size;
		option |= MACH_SEND_MSG;
	}
}
#endif

void InstallExceptionHandler()
{
#ifdef _M_IX86
	PanicAlertT("InstallExceptionHandler called, but this platform does not yet support it.");
#else
	mach_port_t port;
	CheckKR("mach_port_allocate", mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port));
	std::thread exc_thread(ExceptionThread, port);
	exc_thread.detach();
	CheckKR("mach_port_insert_right", mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND));
	CheckKR("thread_set_exception_ports", thread_set_exception_ports(mach_thread_self(), EXC_MASK_BAD_ACCESS, port, EXCEPTION_STATE | MACH_EXCEPTION_CODES, x86_THREAD_STATE64));
	mach_port_t previous;
	CheckKR("Mach_port_request_notification", mach_port_request_notification(mach_task_self(), port, MACH_NOTIFY_NO_SENDERS, 0, port, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous));
#endif
}

#elif !defined(ANDROID)

void sigsegv_handler(int sig, siginfo_t *info, void *raw_context)
{
#ifndef _M_GENERIC
	if (sig != SIGSEGV)
	{
		// We are not interested in other signals - handle it as usual.
		return;
	}
	ucontext_t *context = (ucontext_t *)raw_context;
	int sicode = info->si_code;
	if (sicode != SEGV_MAPERR && sicode != SEGV_ACCERR)
	{
		// Huh? Return.
		return;
	}
	u64 bad_address = (u64)info->si_addr;

	// Get all the information we can out of the context.
	mcontext_t *ctx = &context->uc_mcontext;
	CONTEXT fake_ctx;
#ifdef _M_X64
	fake_ctx.Rax = CREG_RAX(ctx);
	fake_ctx.Rip = CREG_RIP(ctx);
#else
	fake_ctx.Eax = CREG_EAX(ctx);
	fake_ctx.Eip = CREG_EIP(ctx);
#endif
	// assume it's not a write
	if (DoFault(bad_address, false, &fake_ctx))
	{
#ifdef _M_X64
		CREG_RAX(ctx) = fake_ctx.Rax;
		CREG_RIP(ctx) = fake_ctx.Rip;
#else
		CREG_EAX(ctx) = fake_ctx.Eax;
		CREG_EIP(ctx) = fake_ctx.Eip;
#endif
	}
	else
	{
		// retry and crash
		signal(SIGSEGV, SIG_DFL);
	}
#endif
}

void InstallExceptionHandler()
{
#ifdef _M_IX86
	PanicAlertT("InstallExceptionHandler called, but this platform does not yet support it.");
#else
	struct sigaction sa;
	sa.sa_handler = 0;
	sa.sa_sigaction = &sigsegv_handler;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
#endif
}

#endif

}  // namespace
