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

	// TRY_ONLY = 0x2
	NTSTATUS status = LdrLockLoaderLock(0x2, &disposition, &cookie);

	if (status < 0)
		return false;

	if (disposition == 0)
	{
		// Lock was acquired -> loader lock was NOT held
		LdrUnlockLoaderLock(0, cookie);
		return false;
	}

	if (disposition == 1)
	{
		// Lock was already held
		return true;
	}

	return false;
}
