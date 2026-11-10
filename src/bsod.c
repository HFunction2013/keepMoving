/*
 * The crash call itself.
 *
 * NtRaiseHardError() is the documented-by-usage path that lets a user mode
 * process ask the executive to stop the machine: with
 * HARDERROR_RESPONSE_OPTION = OptionShutdownSystem (6) the kernel raises a
 * bugcheck whose code is the NTSTATUS that was passed in, and whose four
 * parameters are taken from the Parameters array.  That gives us exactly the
 * stop code 0xABCDEF12 that was requested.
 *
 * The shutdown response is privileged: the caller's token must be able to
 * enable SeShutdownPrivilege (and, on some configurations, SeTcbPrivilege).
 * Both are attempted, and every outcome is written to the log so a failure is
 * diagnosable instead of silent.
 */

#include "km.h"

typedef LONG(NTAPI *PFN_NtRaiseHardError)(LONG, ULONG, ULONG,
                                          PULONG_PTR, ULONG, PULONG);

BOOL km_enable_privilege(const wchar_t *name, BOOL *held, char *err, size_t cap)
{
    HANDLE           token = NULL;
    TOKEN_PRIVILEGES tp;
    LUID             luid;
    DWORD            last_error = 0;

    if (held != NULL)
        *held = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        if (err)
            km_win32_err(GetLastError(), err, cap);
        return FALSE;
    }

    if (!LookupPrivilegeValueW(NULL, name, &luid)) {
        if (err)
            km_win32_err(GetLastError(), err, cap);
        CloseHandle(token);
        return FALSE;
    }

    ZeroMemory(&tp, sizeof(tp));
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    SetLastError(ERROR_SUCCESS);
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), NULL, NULL)) {
        last_error = GetLastError();
        if (err)
            km_win32_err(last_error, err, cap);
        CloseHandle(token);
        return FALSE;
    }

    last_error = GetLastError();
    CloseHandle(token);

    if (last_error == ERROR_NOT_ALL_ASSIGNED) {
        if (err)
            StringCchPrintfA(err, cap, "privilege %ls is not held by this token",
                             name);
        return FALSE;
    }

    if (held != NULL)
        *held = TRUE;

    return TRUE;
}

int km_raise_hard_error(ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                        ULONG_PTR a4, char *err, size_t cap)
{
    HMODULE               ntdll;
    PFN_NtRaiseHardError  fn;
    ULONG_PTR             params[4];
    ULONG                 response = 0;
    LONG                  status;

    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll == NULL) {
        if (err)
            StringCchCopyA(err, cap, "ntdll.dll is not loaded");
        return -1;
    }

    fn = (PFN_NtRaiseHardError)(void *)GetProcAddress(ntdll, "NtRaiseHardError");
    if (fn == NULL) {
        if (err)
            StringCchCopyA(err, cap, "NtRaiseHardError not exported by ntdll.dll");
        return -1;
    }

    params[0] = a1;
    params[1] = a2;
    params[2] = a3;
    params[3] = a4;

    status = fn((LONG)KM_BUGCHECK_CODE, 4, 0, params,
                (ULONG)KM_OPTION_SHUTDOWN_SYSTEM, &response);

    if (err) {
        if (status == 0) {
            StringCchPrintfA(err, cap,
                             "NtRaiseHardError returned STATUS_SUCCESS "
                             "(response=%lu) without stopping the machine",
                             (unsigned long)response);
        } else {
            StringCchPrintfA(err, cap,
                             "NtRaiseHardError returned NTSTATUS 0x%08lX",
                             (unsigned long)(ULONG)status);
        }
    }

    return (int)status;
}
