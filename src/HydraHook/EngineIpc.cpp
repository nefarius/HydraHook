/**
 * @file EngineIpc.cpp
 * @brief Shared-memory engine status block and thread-pool rehook wait.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "HydraHook/Engine/HydraHookCore.h"
#include "HydraHook/Engine/HydraHookDirect3D9.h"
#include "HydraHook/Engine/HydraHookDirect3D10.h"
#include "HydraHook/Engine/HydraHookDirect3D11.h"
#include "HydraHook/Engine/HydraHookDirect3D12.h"
#include "HydraHook/Engine/HydraHookCoreAudio.h"

#include "EngineIpc.h"
#include "Engine.h"

#include <spdlog/spdlog.h>

#include <cstring>

namespace
{
	void LogIpc(const char* message, DWORD error)
	{
		const auto logger = spdlog::get("HYDRAHOOK");
		if (logger)
			logger->warn("{} ({})", message, error);
	}

	// Relative 100ms. The callback uses this when the request is already accepted but cannot run yet.
	void QueueRehookRetry(PHYDRAHOOK_ENGINE engine)
	{
		if (!engine)
			return;

		if (!engine->RehookWait || !engine->RehookEvent || engine->RehookDisabled.load(std::memory_order_acquire))
		{
			std::int32_t inCallback = HydraHookIpcPhaseInCallback;
			engine->RehookWaitPhase.compare_exchange_strong(inCallback, HydraHookIpcPhaseIdle);
			return;
		}

		std::int32_t inCallback = HydraHookIpcPhaseInCallback;
		if (!engine->RehookWaitPhase.compare_exchange_strong(inCallback, HydraHookIpcPhaseArmed))
			return;

		if (engine->RehookDisabled.load(std::memory_order_acquire))
		{
			std::int32_t armed = HydraHookIpcPhaseArmed;
			engine->RehookWaitPhase.compare_exchange_strong(armed, HydraHookIpcPhaseIdle);
			return;
		}

		ULARGE_INTEGER due;
		due.QuadPart = static_cast<ULONGLONG>(static_cast<LONGLONG>(-100) * 10000);
		FILETIME timeout;
		timeout.dwLowDateTime = due.LowPart;
		timeout.dwHighDateTime = due.HighPart;
		SetThreadpoolWait(engine->RehookWait, engine->RehookEvent, &timeout);

		if (engine->RehookDisabled.load(std::memory_order_acquire)
			&& SetThreadpoolWaitEx(engine->RehookWait, nullptr, nullptr, nullptr))
		{
			std::int32_t armed = HydraHookIpcPhaseArmed;
			engine->RehookWaitPhase.compare_exchange_strong(armed, HydraHookIpcPhaseIdle);
		}
	}

	VOID CALLBACK HydraHookIpcOnRehook(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WAIT, TP_WAIT_RESULT)
	{
		auto* target = static_cast<PHYDRAHOOK_ENGINE>(context);
		if (!target)
			return;

		std::int32_t expected = HydraHookIpcPhaseArmed;
		if (!target->RehookWaitPhase.compare_exchange_strong(expected, HydraHookIpcPhaseInCallback))
			return;

		const auto leave = [&]()
		{
			target->RehookWaitPhase.store(HydraHookIpcPhaseIdle, std::memory_order_release);
		};

		if (target->RehookDisabled.load(std::memory_order_acquire))
		{
			leave();
			return;
		}

		bool retry = false;
		HANDLE thread = nullptr;
		if (TryAcquireSRWLockExclusive(&target->LifecycleLock))
		{
			if (target->EngineThread != nullptr
				&& !DuplicateHandle(GetCurrentProcess(), target->EngineThread, GetCurrentProcess(), &thread,
				                    SYNCHRONIZE, FALSE, 0))
			{
				thread = nullptr;
				retry = true;
			}
			ReleaseSRWLockExclusive(&target->LifecycleLock);
		}
		else
		{
			retry = true;
		}

		if (thread != nullptr)
		{
			const DWORD waited = WaitForSingleObject(thread, 1000);
			CloseHandle(thread);
			if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED)
				retry = true;
		}

		if (target->RehookDisabled.load(std::memory_order_acquire))
		{
			leave();
			return;
		}

		if (retry)
		{
			QueueRehookRetry(target);
			return;
		}

		const HYDRAHOOK_ERROR error = HydraHookEngineRehook(target);
		if (error != HYDRAHOOK_ERROR_NONE)
		{
			HydraHookIpcArmRehookWait(target);
			std::int32_t stuck = HydraHookIpcPhaseInCallback;
			target->RehookWaitPhase.compare_exchange_strong(stuck, HydraHookIpcPhaseIdle);
			return;
		}

		leave();
	}
}

void HydraHookIpcCreate(PHYDRAHOOK_ENGINE engine)
{
	if (!engine)
		return;

	wchar_t stateName[96] = {};
	wchar_t rehookName[96] = {};
	HydraHookIpcFormatNames(GetCurrentProcessId(), reinterpret_cast<std::uintptr_t>(engine->HostInstance), stateName,
	                        96, rehookName, 96);

	SetLastError(ERROR_SUCCESS);
	const HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
	                                          static_cast<DWORD>(sizeof(HydraHookIpcStatus)), stateName);
	if (!mapping)
	{
		LogIpc("HydraHook status section was not created", GetLastError());
		return;
	}
	if (GetLastError() == ERROR_ALREADY_EXISTS)
	{
		LogIpc("HydraHook status section already exists", ERROR_ALREADY_EXISTS);
		CloseHandle(mapping);
		return;
	}

	void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(HydraHookIpcStatus));
	if (!view)
	{
		LogIpc("HydraHook status section could not be mapped", GetLastError());
		CloseHandle(mapping);
		return;
	}

	std::memset(view, 0, sizeof(HydraHookIpcStatus));
	auto* status = static_cast<HydraHookIpcStatus*>(view);
	status->Version = HydraHookIpcVersion;
	status->Size = static_cast<std::uint32_t>(sizeof(HydraHookIpcStatus));
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->Magic), static_cast<LONG>(HydraHookIpcMagic));

	SetLastError(ERROR_SUCCESS);
	HANDLE rehookEvent = CreateEventW(nullptr, FALSE, FALSE, rehookName);
	const DWORD rehookError = GetLastError();
	if (!rehookEvent || rehookError == ERROR_ALREADY_EXISTS)
	{
		LogIpc("HydraHook rehook event was not created", rehookError);
		if (rehookEvent)
			CloseHandle(rehookEvent);
		rehookEvent = nullptr;
	}

	PTP_WAIT rehookWait = nullptr;
	if (rehookEvent)
	{
		rehookWait = CreateThreadpoolWait(&HydraHookIpcOnRehook, engine, nullptr);
		if (!rehookWait)
			LogIpc("HydraHook rehook wait was not created", GetLastError());
	}

	engine->IpcStateMap = mapping;
	engine->IpcStateView = view;
	engine->RehookEvent = rehookEvent;
	engine->RehookWait = rehookWait;
	engine->IpcGeneration.store(1, std::memory_order_release);
	HydraHookIpcPublish(engine, HydraHookIpcStateInitializing, HydraHookIpcShutdownOriginNone, 0, 0);
}

void HydraHookIpcPublish(PHYDRAHOOK_ENGINE engine, std::uint32_t state, std::uint32_t origin, std::uint32_t gameVersion,
                         std::uint32_t flags)
{
	if (!engine || !engine->IpcStateView)
		return;

	auto* status = static_cast<HydraHookIpcStatus*>(engine->IpcStateView);
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->ShutdownOrigin), static_cast<LONG>(origin));
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->GameVersion), static_cast<LONG>(gameVersion));
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->Generation),
	                    static_cast<LONG>(engine->IpcGeneration.load(std::memory_order_acquire)));
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->Flags), static_cast<LONG>(flags));
	InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&status->LastTransitionTick64),
	                      static_cast<LONG64>(GetTickCount64()));
	InterlockedExchange(reinterpret_cast<volatile LONG*>(&status->State), static_cast<LONG>(state));
}

void HydraHookIpcPublishRunning(PHYDRAHOOK_ENGINE engine)
{
	if (!engine || !engine->IpcStateView)
		return;

	auto* status = static_cast<HydraHookIpcStatus*>(engine->IpcStateView);
	// Leave GameVersion alone. A Hooked announcement publishes that field, and writing it here can
	// replace the announced API after State has already become Hooked.
	InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&status->State),
	                           static_cast<LONG>(HydraHookIpcStateRunning),
	                           static_cast<LONG>(HydraHookIpcStateInitializing));
}

void HydraHookIpcArmRehookWait(PHYDRAHOOK_ENGINE engine)
{
	if (!engine)
		return;

	const auto dropCallback = [&]()
	{
		std::int32_t inCallback = HydraHookIpcPhaseInCallback;
		engine->RehookWaitPhase.compare_exchange_strong(inCallback, HydraHookIpcPhaseIdle);
	};

	if (!engine->RehookWait || !engine->RehookEvent || engine->RehookDisabled.load(std::memory_order_acquire))
	{
		dropCallback();
		return;
	}

	std::int32_t phase = engine->RehookWaitPhase.load(std::memory_order_acquire);
	if (phase == HydraHookIpcPhaseInCallback)
	{
		if (!engine->RehookWaitPhase.compare_exchange_strong(phase, HydraHookIpcPhaseArmed))
			return;
	}
	else
	{
		std::int32_t expected = HydraHookIpcPhaseIdle;
		if (!engine->RehookWaitPhase.compare_exchange_strong(expected, HydraHookIpcPhaseArmed))
			return;
	}

	if (engine->RehookDisabled.load(std::memory_order_acquire))
	{
		std::int32_t armed = HydraHookIpcPhaseArmed;
		engine->RehookWaitPhase.compare_exchange_strong(armed, HydraHookIpcPhaseIdle);
		return;
	}

	ResetEvent(engine->RehookEvent);
	SetThreadpoolWait(engine->RehookWait, engine->RehookEvent, nullptr);

	if (engine->RehookDisabled.load(std::memory_order_acquire)
		&& SetThreadpoolWaitEx(engine->RehookWait, nullptr, nullptr, nullptr))
	{
		std::int32_t armed = HydraHookIpcPhaseArmed;
		engine->RehookWaitPhase.compare_exchange_strong(armed, HydraHookIpcPhaseIdle);
	}
}

void HydraHookIpcDisarmRehookWait(PHYDRAHOOK_ENGINE engine)
{
	if (!engine || !engine->RehookWait)
		return;
	if (engine->RehookWaitPhase.load(std::memory_order_acquire) != HydraHookIpcPhaseArmed)
		return;

	if (!SetThreadpoolWaitEx(engine->RehookWait, nullptr, nullptr, nullptr))
		return;

	std::int32_t armed = HydraHookIpcPhaseArmed;
	engine->RehookWaitPhase.compare_exchange_strong(armed, HydraHookIpcPhaseIdle);
}

bool HydraHookIpcAbandonRehookWait(PHYDRAHOOK_ENGINE engine)
{
	if (!engine)
		return true;

	engine->RehookDisabled.store(true, std::memory_order_release);
	if (!engine->RehookWait)
		return true;

	if (engine->RehookWaitPhase.load(std::memory_order_acquire) == HydraHookIpcPhaseInCallback)
		return false;

	if (engine->RehookWaitPhase.load(std::memory_order_acquire) == HydraHookIpcPhaseArmed)
	{
		if (!SetThreadpoolWaitEx(engine->RehookWait, nullptr, nullptr, nullptr))
			return false;

		std::int32_t armed = HydraHookIpcPhaseArmed;
		engine->RehookWaitPhase.compare_exchange_strong(armed, HydraHookIpcPhaseIdle);
		if (engine->RehookWaitPhase.load(std::memory_order_acquire) == HydraHookIpcPhaseInCallback)
			return false;
	}

	return true;
}

void HydraHookIpcClose(PHYDRAHOOK_ENGINE engine)
{
	if (!engine)
		return;

	if (engine->RehookWait)
	{
		SetThreadpoolWaitEx(engine->RehookWait, nullptr, nullptr, nullptr);
		CloseThreadpoolWait(engine->RehookWait);
		engine->RehookWait = nullptr;
	}
	if (engine->RehookEvent)
	{
		CloseHandle(engine->RehookEvent);
		engine->RehookEvent = nullptr;
	}
	if (engine->IpcStateView)
	{
		UnmapViewOfFile(engine->IpcStateView);
		engine->IpcStateView = nullptr;
	}
	if (engine->IpcStateMap)
	{
		CloseHandle(engine->IpcStateMap);
		engine->IpcStateMap = nullptr;
	}
}
