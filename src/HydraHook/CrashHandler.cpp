/**
 * @file CrashHandler.cpp
 * @brief Crash handler implementation -- SEH, minidump, CRT handlers.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <eh.h>
#include <signal.h>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <atomic>

#pragma comment(lib, "DbgHelp.lib")

#include "HydraHook/Engine/HydraHookCore.h"
#include "HydraHook/Engine/HydraHookDirect3D9.h"
#include "HydraHook/Engine/HydraHookDirect3D10.h"
#include "HydraHook/Engine/HydraHookDirect3D11.h"
#include "HydraHook/Engine/HydraHookDirect3D12.h"
#include "HydraHook/Engine/HydraHookCoreAudio.h"
#include "Engine.h"
#include "CrashHandler.h"
#include "Exceptions.hpp"
#include "Global.h"

#include <spdlog/spdlog.h>

// ---------------------------------------------------------------------------
// Saved previous handlers (restored on uninstall)
// ---------------------------------------------------------------------------
static LPTOP_LEVEL_EXCEPTION_FILTER  s_prevUnhandledFilter  = nullptr;
static std::terminate_handler        s_prevTerminateHandler  = nullptr;
static _invalid_parameter_handler    s_prevInvalidParamHandler = nullptr;
static _purecall_handler             s_prevPurecallHandler   = nullptr;

// Reference count for global handlers (multiple engine instances)
static std::atomic<int> s_refCount{ 0 };
static std::mutex       s_installMutex;

// Self-contained snapshot of crash config, independent of any engine's lifetime.
// The crash path reads this atomically without locks to avoid deadlock.
struct CrashConfigSnapshot
{
	std::string DumpDirectoryPath;
	HMODULE HostInstance;
	HYDRAHOOK_DUMP_TYPE DumpType;
	PFN_HYDRAHOOK_CRASH_HANDLER EvtCrashHandler;
	PHYDRAHOOK_ENGINE OwnerEngine;  // only valid while owner is alive; used for callback arg
};

static std::atomic<CrashConfigSnapshot*> s_snapshot{ nullptr };

// ---------------------------------------------------------------------------
// Exception code to symbolic name
// ---------------------------------------------------------------------------
static const char* ExceptionCodeToString(DWORD code)
{
	switch (code)
	{
	case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
	case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
	case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
	case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
	case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
	case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
	case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
	case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
	case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
	case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
	case EXCEPTION_GUARD_PAGE:               return "EXCEPTION_GUARD_PAGE";
	case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
	case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
	case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
	case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
	case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
	case EXCEPTION_INVALID_HANDLE:           return "EXCEPTION_INVALID_HANDLE";
	case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
	case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
	case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
	case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
	case STATUS_HEAP_CORRUPTION:             return "STATUS_HEAP_CORRUPTION";
	default:                                 return "UNKNOWN_EXCEPTION";
	}
}

// ---------------------------------------------------------------------------
// Resolve faulting module name + offset from an address
// ---------------------------------------------------------------------------
static void GetModuleFromAddress(PVOID address, char* moduleName, size_t nameLen, DWORD_PTR* offset)
{
	HMODULE hMod = nullptr;
	if (GetModuleHandleExA(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		static_cast<LPCSTR>(address),
		&hMod) && hMod)
	{
		GetModuleFileNameA(hMod, moduleName, static_cast<DWORD>(nameLen));
		*offset = reinterpret_cast<DWORD_PTR>(address) - reinterpret_cast<DWORD_PTR>(hMod);
	}
	else
	{
		strncpy_s(moduleName, nameLen, "<unknown>", _TRUNCATE);
		*offset = 0;
	}
}

// ---------------------------------------------------------------------------
// Map HYDRAHOOK_DUMP_TYPE to MINIDUMP_TYPE flags
// ---------------------------------------------------------------------------
static MINIDUMP_TYPE GetMiniDumpTypeFlags(HYDRAHOOK_DUMP_TYPE type)
{
	switch (type)
	{
	case HydraHookDumpTypeMinimal:
		return MiniDumpNormal;
	case HydraHookDumpTypeFull:
		return static_cast<MINIDUMP_TYPE>(
			MiniDumpWithFullMemory |
			MiniDumpWithHandleData |
			MiniDumpWithThreadInfo |
			MiniDumpWithUnloadedModules);
	case HydraHookDumpTypeNormal:
	default:
		return static_cast<MINIDUMP_TYPE>(
			MiniDumpNormal |
			MiniDumpWithDataSegs |
			MiniDumpWithHandleData |
			MiniDumpWithThreadInfo |
			MiniDumpWithUnloadedModules);
	}
}

// ---------------------------------------------------------------------------
// Build dump directory path, falling back through configured -> log dir -> %TEMP%
// ---------------------------------------------------------------------------
static std::string ResolveDumpDirectory(const CrashConfigSnapshot* snap)
{
	if (snap && !snap->DumpDirectoryPath.empty())
	{
		std::string path = HydraHook::Core::Util::expand_environment_variables(
			snap->DumpDirectoryPath);
		if (!path.empty())
		{
			if (path.back() != '\\' && path.back() != '/')
				path += '\\';
			return path;
		}
	}

	std::string dir = HydraHook::Core::Util::get_process_directory();
	if (!dir.empty())
		return dir;

	if (snap && snap->HostInstance)
	{
		dir = HydraHook::Core::Util::get_module_directory(snap->HostInstance);
		if (!dir.empty())
			return dir;
	}

	return HydraHook::Core::Util::expand_environment_variables("%TEMP%\\");
}

// ---------------------------------------------------------------------------
// Get a short process name without path or extension
// ---------------------------------------------------------------------------
static std::string GetProcessBaseName()
{
	char path[MAX_PATH]{};
	GetModuleFileNameA(nullptr, path, MAX_PATH);
	std::string s(path);
	auto pos = s.find_last_of("\\/");
	if (pos != std::string::npos)
		s = s.substr(pos + 1);
	auto dot = s.find_last_of('.');
	if (dot != std::string::npos)
		s = s.substr(0, dot);
	return s;
}

// ---------------------------------------------------------------------------
// Core crash output routine -- log + user callback + minidump
// ---------------------------------------------------------------------------
static void WriteCrashDump(EXCEPTION_POINTERS* exInfo, const char* trigger)
{
	// Atomic snapshot load -- no lock, safe even if crash fires during install/uninstall
	const CrashConfigSnapshot* snap = s_snapshot.load(std::memory_order_acquire);

	auto logger = spdlog::get("HYDRAHOOK");
	if (!logger)
		return;

	auto crashLog = logger->clone("crash");

	const DWORD exCode = exInfo && exInfo->ExceptionRecord
		? exInfo->ExceptionRecord->ExceptionCode : 0;
	const PVOID exAddr = exInfo && exInfo->ExceptionRecord
		? exInfo->ExceptionRecord->ExceptionAddress : nullptr;

	crashLog->critical("=== HydraHook Crash Handler ({}) ===", trigger);
	crashLog->critical("Exception code: 0x{:08X} ({})", exCode, ExceptionCodeToString(exCode));
	crashLog->critical("Faulting address: {}", exAddr);
	crashLog->critical("Thread ID: {}", GetCurrentThreadId());

	if (exAddr)
	{
		char modName[MAX_PATH]{};
		DWORD_PTR modOffset = 0;
		GetModuleFromAddress(exAddr, modName, sizeof(modName), &modOffset);
		crashLog->critical("Faulting module: {} + 0x{:X}", modName, modOffset);
	}

	if (exInfo && exInfo->ContextRecord)
	{
		const CONTEXT& ctx = *exInfo->ContextRecord;
#ifdef _WIN64
		crashLog->critical("Registers: RIP=0x{:016X} RSP=0x{:016X} RBP=0x{:016X}",
			ctx.Rip, ctx.Rsp, ctx.Rbp);
		crashLog->critical("           RAX=0x{:016X} RBX=0x{:016X} RCX=0x{:016X}",
			ctx.Rax, ctx.Rbx, ctx.Rcx);
		crashLog->critical("           RDX=0x{:016X} RSI=0x{:016X} RDI=0x{:016X}",
			ctx.Rdx, ctx.Rsi, ctx.Rdi);
		crashLog->critical("           R8 =0x{:016X} R9 =0x{:016X} R10=0x{:016X}",
			ctx.R8, ctx.R9, ctx.R10);
		crashLog->critical("           R11=0x{:016X} R12=0x{:016X} R13=0x{:016X}",
			ctx.R11, ctx.R12, ctx.R13);
		crashLog->critical("           R14=0x{:016X} R15=0x{:016X}",
			ctx.R14, ctx.R15);
#else
		crashLog->critical("Registers: EIP=0x{:08X} ESP=0x{:08X} EBP=0x{:08X}",
			ctx.Eip, ctx.Esp, ctx.Ebp);
		crashLog->critical("           EAX=0x{:08X} EBX=0x{:08X} ECX=0x{:08X}",
			ctx.Eax, ctx.Ebx, ctx.Ecx);
		crashLog->critical("           EDX=0x{:08X} ESI=0x{:08X} EDI=0x{:08X}",
			ctx.Edx, ctx.Esi, ctx.Edi);
#endif
	}

	crashLog->flush();

	// Invoke user callback if registered (uses snapshot-owned data only)
	if (snap && snap->EvtCrashHandler)
	{
		if (!snap->EvtCrashHandler(snap->OwnerEngine, exCode, exInfo))
		{
			crashLog->critical("User crash callback returned FALSE, skipping dump file");
			crashLog->flush();
			return;
		}
	}

	// Build dump file path
	SYSTEMTIME st;
	GetLocalTime(&st);

	std::string processName = GetProcessBaseName();
	std::string dumpDir = ResolveDumpDirectory(snap);

	char dumpPath[MAX_PATH]{};
	_snprintf_s(dumpPath, _TRUNCATE,
		"%sHydraHook-%s-%u-%04d%02d%02d-%02d%02d%02d-0x%08X.dmp",
		dumpDir.c_str(),
		processName.c_str(),
		GetCurrentProcessId(),
		st.wYear, st.wMonth, st.wDay,
		st.wHour, st.wMinute, st.wSecond,
		exCode);

	HANDLE hFile = CreateFileA(
		dumpPath,
		GENERIC_WRITE,
		0,
		nullptr,
		CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL,
		nullptr);

	if (hFile == INVALID_HANDLE_VALUE)
	{
		crashLog->critical("Failed to create dump file: {} (error {})", dumpPath, GetLastError());
		crashLog->flush();
		return;
	}

	MINIDUMP_EXCEPTION_INFORMATION mdei;
	mdei.ThreadId = GetCurrentThreadId();
	mdei.ExceptionPointers = exInfo;
	mdei.ClientPointers = FALSE;

	HYDRAHOOK_DUMP_TYPE dumpType = snap ? snap->DumpType : HydraHookDumpTypeNormal;

	const BOOL success = MiniDumpWriteDump(
		GetCurrentProcess(),
		GetCurrentProcessId(),
		hFile,
		GetMiniDumpTypeFlags(dumpType),
		exInfo ? &mdei : nullptr,
		nullptr,
		nullptr);

	CloseHandle(hFile);

	if (success)
		crashLog->critical("Minidump written to: {}", dumpPath);
	else
		crashLog->critical("MiniDumpWriteDump failed (error {})", GetLastError());

	crashLog->flush();
}

// ---------------------------------------------------------------------------
// Unhandled exception filter (last resort for SEH)
// ---------------------------------------------------------------------------
static LONG WINAPI HydraHookUnhandledExceptionFilter(EXCEPTION_POINTERS* exInfo)
{
	WriteCrashDump(exInfo, "UnhandledExceptionFilter");

	if (s_prevUnhandledFilter)
		return s_prevUnhandledFilter(exInfo);

	return EXCEPTION_EXECUTE_HANDLER;
}

// ---------------------------------------------------------------------------
// C++ terminate handler
// ---------------------------------------------------------------------------
static void HydraHookTerminateHandler()
{
	__try
	{
		RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}
	__except (WriteCrashDump(GetExceptionInformation(), "terminate"), EXCEPTION_EXECUTE_HANDLER)
	{
	}

	if (s_prevTerminateHandler)
		s_prevTerminateHandler();
	else
		abort();
}

// ---------------------------------------------------------------------------
// CRT invalid parameter handler
// ---------------------------------------------------------------------------
static void HydraHookInvalidParameterHandler(
	const wchar_t* /*expression*/,
	const wchar_t* /*function*/,
	const wchar_t* /*file*/,
	unsigned int   /*line*/,
	uintptr_t      /*pReserved*/)
{
	__try
	{
		RaiseException(0xE0000002, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}
	__except (WriteCrashDump(GetExceptionInformation(), "InvalidParameter"), EXCEPTION_EXECUTE_HANDLER)
	{
	}

	if (s_prevInvalidParamHandler)
		s_prevInvalidParamHandler(nullptr, nullptr, nullptr, 0, 0);
}

// ---------------------------------------------------------------------------
// Pure virtual call handler
// ---------------------------------------------------------------------------
static void HydraHookPurecallHandler()
{
	__try
	{
		RaiseException(0xE0000003, EXCEPTION_NONCONTINUABLE, 0, nullptr);
	}
	__except (WriteCrashDump(GetExceptionInformation(), "PureVirtualCall"), EXCEPTION_EXECUTE_HANDLER)
	{
	}

	if (s_prevPurecallHandler)
		s_prevPurecallHandler();
}

// ---------------------------------------------------------------------------
// SEH-to-C++ translator (per-thread)
// ---------------------------------------------------------------------------
static void HydraHookSehTranslator(unsigned int code, EXCEPTION_POINTERS* info)
{
	throw HydraHook::Core::Exceptions::SehException(static_cast<DWORD>(code), info);
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

static CrashConfigSnapshot* MakeSnapshot(PHYDRAHOOK_ENGINE engine)
{
	auto* snap = new (std::nothrow) CrashConfigSnapshot();
	if (!snap)
		return nullptr;

	const auto& cfg = engine->EngineConfig.CrashHandler;
	snap->DumpDirectoryPath = cfg.DumpDirectoryPath ? cfg.DumpDirectoryPath : "";
	snap->HostInstance = engine->HostInstance;
	snap->DumpType = cfg.DumpType;
	snap->EvtCrashHandler = cfg.EvtCrashHandler;
	snap->OwnerEngine = engine;
	return snap;
}

void HydraHookCrashHandlerInstall(PHYDRAHOOK_ENGINE engine)
{
	if (!engine)
		return;

	std::lock_guard<std::mutex> lock(s_installMutex);

	auto* newSnap = MakeSnapshot(engine);

	if (s_refCount.fetch_add(1) == 0)
	{
		// First installer: publish the snapshot and register global handlers
		auto* old = s_snapshot.exchange(newSnap, std::memory_order_release);
		delete old;

		s_prevUnhandledFilter = SetUnhandledExceptionFilter(HydraHookUnhandledExceptionFilter);
		s_prevTerminateHandler = std::set_terminate(HydraHookTerminateHandler);
		s_prevInvalidParamHandler = _set_invalid_parameter_handler(HydraHookInvalidParameterHandler);
		s_prevPurecallHandler = _set_purecall_handler(HydraHookPurecallHandler);

		auto logger = spdlog::get("HYDRAHOOK");
		if (logger)
			logger->clone("crash")->info("Crash handler installed (dump type: {})",
				static_cast<int>(engine->EngineConfig.CrashHandler.DumpType));
	}
	else
	{
		// Additional installer: keep as a potential replacement but don't publish yet.
		// The snapshot is ready if the current owner uninstalls first.
		delete newSnap;
	}
}

void HydraHookCrashHandlerUninstall(PHYDRAHOOK_ENGINE engine)
{
	std::lock_guard<std::mutex> lock(s_installMutex);

	int expected = s_refCount.load(std::memory_order_relaxed);
	do {
		if (expected <= 0)
			return;
	} while (!s_refCount.compare_exchange_weak(expected, expected - 1,
		std::memory_order_acq_rel, std::memory_order_relaxed));

	const int prev = expected;

	// If the uninstalling engine owns the current snapshot, clear it now --
	// before the engine struct can be freed -- so the crash path never sees a stale pointer.
	CrashConfigSnapshot* snap = s_snapshot.load(std::memory_order_acquire);
	if (snap && snap->OwnerEngine == engine)
	{
		auto* old = s_snapshot.exchange(nullptr, std::memory_order_release);
		delete old;
	}

	if (prev == 1)
	{
		// Last uninstaller: restore all previous handlers
		SetUnhandledExceptionFilter(s_prevUnhandledFilter);
		std::set_terminate(s_prevTerminateHandler);
		_set_invalid_parameter_handler(s_prevInvalidParamHandler);
		_set_purecall_handler(s_prevPurecallHandler);

		s_prevUnhandledFilter = nullptr;
		s_prevTerminateHandler = nullptr;
		s_prevInvalidParamHandler = nullptr;
		s_prevPurecallHandler = nullptr;

		auto logger = spdlog::get("HYDRAHOOK");
		if (logger)
			logger->clone("crash")->info("Crash handler uninstalled");
	}
}

void HydraHookCrashHandlerInstallThreadSEH()
{
	_set_se_translator(HydraHookSehTranslator);
}
