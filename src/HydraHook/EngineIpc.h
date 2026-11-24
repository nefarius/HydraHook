/**
 * @file EngineIpc.h
 * @brief Session-local shared-memory engine status and rehook control.
 *
 * Each engine publishes a 64-byte status block named
 * Local\\HydraHook.State.\<pid\>.\<hostInstanceHex16\> and a companion auto-reset
 * event Local\\HydraHook.Rehook.\<pid\>.\<hostInstanceHex16\>. Readers (HydraHook
 * Manager) open the section read-only. The rehook event is waited on with the
 * process thread pool only while the engine is stopped for a recoverable
 * reason, so a pinned DLL can be restarted without LoadLibrary.
 *
 * Layout (little-endian, natural alignment, 64 bytes):
 *   0  uint32 Magic              0x534B4848 ('HHKS')
 *   4  uint32 Version            1
 *   8  uint32 Size               64
 *  12  uint32 State              HydraHookIpcState
 *  16  uint32 ShutdownOrigin     HydraHookIpcShutdownOrigin
 *  20  uint32 GameVersion        HYDRAHOOK_D3D_VERSION
 *  24  uint32 Generation         starts at 1, increments on each rehook
 *  28  uint32 Flags              HydraHookIpcFlags
 *  32  uint64 LastTransitionTick64
 *  40  uint32 Reserved[6]
 *
 * State is published last with an interlocked store. Readers that sample State
 * twice around the other fields observe one transition.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

#pragma once

#include <Windows.h>

#include "HydraHook/Engine/HydraHookCore.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

constexpr std::uint32_t HydraHookIpcMagic = 0x534B4848u;
constexpr std::uint32_t HydraHookIpcVersion = 1u;

constexpr std::uint32_t HydraHookIpcStateNone = 0u;
constexpr std::uint32_t HydraHookIpcStateInitializing = 1u;
constexpr std::uint32_t HydraHookIpcStateRunning = 2u;
constexpr std::uint32_t HydraHookIpcStateHooked = 3u;
constexpr std::uint32_t HydraHookIpcStateShuttingDown = 4u;
constexpr std::uint32_t HydraHookIpcStateStopped = 5u;

constexpr std::uint32_t HydraHookIpcShutdownOriginNone = 0u;
constexpr std::uint32_t HydraHookIpcShutdownOriginExitProcess = 1u;
constexpr std::uint32_t HydraHookIpcShutdownOriginPostQuitMessage = 2u;
constexpr std::uint32_t HydraHookIpcShutdownOriginFreeLibrary = 3u;
constexpr std::uint32_t HydraHookIpcShutdownOriginDllMainDetach = 4u;
constexpr std::uint32_t HydraHookIpcShutdownOriginManualUnhook = 5u;

constexpr std::uint32_t HydraHookIpcFlagRehookSupported = 1u << 0;
constexpr std::uint32_t HydraHookIpcFlagDrainIncomplete = 1u << 1;

constexpr std::int32_t HydraHookIpcPhaseIdle = 0;
constexpr std::int32_t HydraHookIpcPhaseArmed = 1;
constexpr std::int32_t HydraHookIpcPhaseInCallback = 2;
constexpr std::int32_t HydraHookIpcPhaseInCallbackExternal = 3;

#pragma pack(push, 4)
struct HydraHookIpcStatus
{
	std::uint32_t Magic;
	std::uint32_t Version;
	std::uint32_t Size;
	std::uint32_t State;
	std::uint32_t ShutdownOrigin;
	std::uint32_t GameVersion;
	std::uint32_t Generation;
	std::uint32_t Flags;
	std::uint64_t LastTransitionTick64;
	std::uint32_t Reserved[6];
};
#pragma pack(pop)

static_assert(sizeof(HydraHookIpcStatus) == 64, "HydraHookIpcStatus must stay 64 bytes");
static_assert(offsetof(HydraHookIpcStatus, LastTransitionTick64) == 32, "tick must stay at offset 32");
static_assert(offsetof(HydraHookIpcStatus, State) == 12, "state must stay at offset 12");

inline void HydraHookIpcFormatNames(DWORD pid, std::uintptr_t host, wchar_t* stateName, std::size_t stateCch,
                                    wchar_t* rehookName, std::size_t rehookCch)
{
	swprintf_s(stateName, stateCch, L"Local\\HydraHook.State.%lu.%016llX", pid,
	           static_cast<unsigned long long>(host));
	swprintf_s(rehookName, rehookCch, L"Local\\HydraHook.Rehook.%lu.%016llX", pid,
	           static_cast<unsigned long long>(host));
}

using HydraHookIpcRehookHold = void (*)(PHYDRAHOOK_ENGINE engine);

/** Test seam. Invoked after a successful rehook callback starts the worker and before it releases the phase. */
extern std::atomic<HydraHookIpcRehookHold> HydraHookIpcRehookHoldForTest;

void HydraHookIpcCreate(PHYDRAHOOK_ENGINE engine);
void HydraHookIpcPublish(PHYDRAHOOK_ENGINE engine, std::uint32_t state, std::uint32_t origin, std::uint32_t gameVersion,
                         std::uint32_t flags);

/** @brief Moves Initializing to Running. Does not write the graphics API a Hooked publish owns. */
void HydraHookIpcPublishRunning(PHYDRAHOOK_ENGINE engine);
void HydraHookIpcArmRehookWait(PHYDRAHOOK_ENGINE engine);
void HydraHookIpcDisarmRehookWait(PHYDRAHOOK_ENGINE engine);

/**
 * @brief Cancels a pending rehook wait so the engine can be freed.
 * @return false when a rehook callback is queued or running. The caller must keep the engine.
 */
bool HydraHookIpcAbandonRehookWait(PHYDRAHOOK_ENGINE engine);

/** @brief Closes IPC objects. The caller guarantees no rehook callback is running. */
void HydraHookIpcClose(PHYDRAHOOK_ENGINE engine);
