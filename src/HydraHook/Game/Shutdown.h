/**
 * @file Shutdown.h
 * @brief Internal shutdown origin and cleanup for HydraHook engine.
 *
 * Defines ShutdownOrigin enum and PerformShutdownCleanup for consolidated
 * pre-exit handling. Internal use only; not part of public API.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

#pragma once

#include "HydraHook/Engine/HydraHookCore.h"

/**
 * @brief Origin of the shutdown request; determines what is safe to call.
 */
enum class ShutdownOrigin
{
	ExitProcessHook,
	PostQuitMessageHook,
	DllMainProcessDetach
};

/**
 * @brief Performs pre-exit cleanup based on shutdown origin.
 *
 * For ExitProcessHook and PostQuitMessageHook: removes the other hook,
 * invokes EvtHydraHookGamePreExit, signals the engine thread, and waits
 * for it to finish. For DllMainProcessDetach: skips user callbacks and
 * hook removal (loader lock); optionally signals and does a brief wait.
 *
 * @param engine Engine instance to clean up.
 * @param origin Shutdown origin for decision making.
 */
void PerformShutdownCleanup(PHYDRAHOOK_ENGINE engine, ShutdownOrigin origin);
