#include "LdrLock.h"
#include <Windows.h>

using LdrLockLoaderLock_t = NTSTATUS(NTAPI*)(
	ULONG Flags,
	ULONG* Disposition,
	PULONG_PTR Cookie
);

using LdrUnlockLoaderLock_t = NTSTATUS(NTAPI*)(
	ULONG Flags,
	ULONG_PTR Cookie
);

bool HydraHook::Core::Util::IsLoaderLockHeld()
{
	auto ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll)
		return false;

	auto LdrLockLoaderLock =
		(LdrLockLoaderLock_t)GetProcAddress(ntdll, "LdrLockLoaderLock");

	auto LdrUnlockLoaderLock =
		(LdrUnlockLoaderLock_t)GetProcAddress(ntdll, "LdrUnlockLoaderLock");

	if (!LdrLockLoaderLock || !LdrUnlockLoaderLock)
		return false;

	ULONG disposition = 0;
	ULONG_PTR cookie = 0;

	// TRY_ONLY = 0x2. Disposition 1 means this call acquired the lock.
	// Disposition 2 means the lock is held by another thread.
	constexpr ULONG kTryOnly = 0x2;
	constexpr ULONG kLockAcquired = 1;
	constexpr ULONG kLockNotAcquired = 2;

	NTSTATUS status = LdrLockLoaderLock(kTryOnly, &disposition, &cookie);

	if (status < 0)
		return false;

	if (disposition == kLockNotAcquired)
		return true;

	if (disposition == kLockAcquired)
	{
		// Drop the reference this probe took. The loader lock is recursive,
		// so a try on the thread that already owns it also returns acquired
		// and this function reports "not held" in that case (including DllMain).
		LdrUnlockLoaderLock(0, cookie);
	}

	return false;
}
