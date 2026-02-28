/**
 * @file CrashHandler.h
 * @brief Internal crash handler installation and teardown.
 *
 * Provides process-wide (reference-counted) and per-thread crash handling
 * for field debugging. Not part of the public API.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

#pragma once

#include "HydraHook/Engine/HydraHookCore.h"

/**
 * @brief Installs global crash handlers (ref-counted) for the given engine.
 *
 * On first call: registers SetUnhandledExceptionFilter, set_terminate,
 * _set_invalid_parameter_handler, and _set_purecall_handler.
 * Subsequent calls increment the reference count without re-installing.
 *
 * @param engine Engine whose CrashHandler config controls dump output.
 */
void HydraHookCrashHandlerInstall(PHYDRAHOOK_ENGINE engine);

/**
 * @brief Decrements the crash handler reference count.
 *
 * If the uninstalling engine owns the active crash config snapshot, the
 * snapshot is cleared immediately so the crash path never sees a stale
 * pointer. When the last engine uninstalls, restores all previous handlers.
 *
 * @param engine The engine that is being destroyed.
 */
void HydraHookCrashHandlerUninstall(PHYDRAHOOK_ENGINE engine);

/**
 * @brief Installs the per-thread SEH-to-C++ translator on the calling thread.
 *
 * Must be called from the engine worker thread. Bridges Win32 structured
 * exceptions into HydraHook::Core::Exceptions::SehException so that
 * existing try/catch blocks can catch hardware faults.
 */
void HydraHookCrashHandlerInstallThreadSEH();
