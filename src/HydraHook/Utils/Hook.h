/**
 * @file Hook.h
 * @brief C++ wrapper for Microsoft Detours function hooking.
 *
 * Provides a type-safe Hook template for attaching/detaching detours with
 * support for __stdcall and __cdecl conventions. Uses RAII for transaction
 * management.
 *
 * @par Dependencies
 * Requires detours library and HydraHook::Core::Exceptions::DetourException.
 *
 * @internal
 * @copyright MIT License (c) 2018-2026 Benjamin Höglinger-Stelzer
 */

/*
MIT License

Copyright (c) 2018-2026 Benjamin Höglinger-Stelzer

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#pragma once

#include "Windows.h"
#include <detours/detours.h>
#include "Exceptions.hpp"

/** @brief Supported calling conventions for hook targets. */
enum class CallConvention
{
    stdcall_t,  /**< __stdcall convention. */
    cdecl_t     /**< __cdecl convention. */
};

/** @brief Maps CallConvention to function pointer type. */
template <CallConvention cc, typename retn, typename convention, typename ...args>
struct convention;

template <typename retn, typename ...args>
struct convention<CallConvention::stdcall_t, retn, args...>
{
    typedef retn (__stdcall *type)(args ...);
};

template <typename retn, typename ...args>
struct convention<CallConvention::cdecl_t, retn, args...>
{
    typedef retn (__cdecl *type)(args ...);
};

/**
 * @brief RAII Detours hook wrapper for function interception.
 * @tparam cc Calling convention (stdcall_t or cdecl_t).
 * @tparam retn Return type.
 * @tparam args Argument types.
 */
template <CallConvention cc, typename retn, typename ...args>
class Hook
{
    typedef typename convention<cc, retn, args...>::type type;

    size_t orig_;
    type detour_;

    bool is_applied_;
    bool has_open_transaction_;

    void transaction_begin()
    {
        const auto result = DetourTransactionBegin();

        if (result != NO_ERROR)
        {
            if (result == ERROR_INVALID_OPERATION)
            {
                throw HydraHook::Core::Exceptions::DetourException(
                    "A pending transaction already exists"
                );
            }

            throw HydraHook::Core::Exceptions::DetourException("Unknown error");
        }

        has_open_transaction_ = true;
    }

    void transaction_commit()
    {
        const auto result = DetourTransactionCommit();

        if (result != NO_ERROR)
        {
            switch (result)
            {
            case ERROR_INVALID_DATA:
                throw HydraHook::Core::Exceptions::DetourException(
                    "Target function was changed by third party between steps of the transaction"
                );

            case ERROR_INVALID_OPERATION:
                throw HydraHook::Core::Exceptions::DetourException(
                    "No pending transaction exists"
                );

            case ERROR_INVALID_BLOCK:
                throw HydraHook::Core::Exceptions::DetourException(
                    "The function referenced is too small to be detoured"
                );

            case ERROR_INVALID_HANDLE:
                throw HydraHook::Core::Exceptions::DetourException(
                    "The ppPointer parameter is null or points to a null pointer"
                );

            case ERROR_NOT_ENOUGH_MEMORY:
                throw HydraHook::Core::Exceptions::DetourException(
                    "Not enough memory exists to complete the operation"
                );

            default:
                throw HydraHook::Core::Exceptions::DetourException(
                    "Unknown error"
                );
            }
        }

        has_open_transaction_ = false;
    }

    static void update_thread(HANDLE hThread)
    {
        const auto result = DetourUpdateThread(hThread);

        if (result != NO_ERROR)
        {
            if (result == ERROR_NOT_ENOUGH_MEMORY)
            {
                throw HydraHook::Core::Exceptions::DetourException(
                    "Not enough memory to record identity of thread"
                );
            }

            throw HydraHook::Core::Exceptions::DetourException("Unknown error");
        }
    }

public:
    /** @brief Constructs an unapplied hook. */
    Hook() : orig_(0), detour_(0), is_applied_(false), has_open_transaction_(false)
    {
    }

    /** @brief Destroys hook; removes if applied. @throws DetourException on abort/remove failure. */
    ~Hook() noexcept(false)
    {
        if (has_open_transaction_)
        {
            const auto result = DetourTransactionAbort();

            if (result != NO_ERROR)
            {
                if (result == ERROR_INVALID_OPERATION)
                {
                    throw HydraHook::Core::Exceptions::DetourException(
                        "No pending transaction exists"
                    );
                }
                throw HydraHook::Core::Exceptions::DetourException("Unknown error");
            }
        }

        remove();
    }

    /**
     * @brief Attaches detour to target function.
     * @param pFunc Pointer to target function (address to patch).
     * @param detour Detour function (same signature as target).
     * @throws DetourException on Detours API failure.
     */
    template <typename T>
    void apply(T pFunc, type detour)
    {
        detour_ = detour;
        orig_ = static_cast<size_t>(pFunc);

        transaction_begin();
        update_thread(GetCurrentThread());
        const auto result = DetourAttach(reinterpret_cast<void **>(&orig_), reinterpret_cast<void *>(detour_));

        if (result != NO_ERROR)
        {
            switch (result)
            {
            case ERROR_INVALID_BLOCK:
                throw HydraHook::Core::Exceptions::DetourException(
                    "The function referenced is too small to be detoured"
                );

            case ERROR_INVALID_HANDLE:
                throw HydraHook::Core::Exceptions::DetourException(
                    "The ppPointer parameter is null or points to a null pointer"
                );

            case ERROR_INVALID_OPERATION:
                throw HydraHook::Core::Exceptions::DetourException(
                    "No pending transaction exists"
                );

            case ERROR_NOT_ENOUGH_MEMORY:
                throw HydraHook::Core::Exceptions::DetourException(
                    "Not enough memory exists to complete the operation"
                );

            default:
                throw HydraHook::Core::Exceptions::DetourException("Unknown error");
            }
        }

        transaction_commit();

        is_applied_ = true;
    }

    /** @brief Removes hook if applied. @throws DetourException on Detours API failure. */
    void remove()
    {
        if (!is_applied_)
            return;

        is_applied_ = false;

        transaction_begin();
        update_thread(GetCurrentThread());
        const auto result = DetourDetach(reinterpret_cast<void **>(&orig_), reinterpret_cast<void *>(detour_));

        if (result != NO_ERROR)
        {
            switch (result)
            {
            case ERROR_INVALID_BLOCK:
                throw HydraHook::Core::Exceptions::DetourException(
                    "The function to be detached was too small to be detoured"
                );

            case ERROR_INVALID_HANDLE:
                throw HydraHook::Core::Exceptions::DetourException(
                    "The ppPointer parameter is null or points to a null pointer"
                );

            case ERROR_INVALID_OPERATION:
                throw HydraHook::Core::Exceptions::DetourException(
                    "No pending transaction exists"
                );

            case ERROR_NOT_ENOUGH_MEMORY:
                throw HydraHook::Core::Exceptions::DetourException(
                    "Not enough memory exists to complete the operation"
                );

            default:
                throw HydraHook::Core::Exceptions::DetourException(
                    "Unknown error"
                );
            }
        }

        transaction_commit();
    }

    /**
     * @brief Removes hook if applied. Never throws; safe to call under loader lock.
     * @return true if successfully removed, false on error or if not applied.
     */
    bool remove_nothrow() noexcept
    {
        if (!is_applied_)
            return true;

        if (DetourTransactionBegin() != NO_ERROR)
            return false;

        if (DetourUpdateThread(GetCurrentThread()) != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }

        const auto result = DetourDetach(reinterpret_cast<void **>(&orig_), reinterpret_cast<void *>(detour_));

        if (result != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }

        if (DetourTransactionCommit() != NO_ERROR)
        {
            DetourTransactionAbort();
            return false;
        }

        is_applied_ = false;
        return true;
    }

    /** @brief Calls the original (unhooked) function. */
    retn call_orig(args ... p)
    {
        return type(orig_)(p...);
    }

    /** @brief Returns true if hook is currently applied. */
    bool is_applied() const
    {
        return is_applied_;
    }
};
