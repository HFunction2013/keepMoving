/*
 * keepMoving v2.0.0 kernel driver.
 *
 * Minimal legacy NT driver: the moment DriverEntry runs, it raises bugcheck
 * 0xABCDEF12 directly in kernel mode.  The user-mode NtRaiseHardError path is
 * kept as a fallback; on some machines it does not actually stop the machine,
 * which is why this driver exists.
 *
 * Params:
 *   BugCheckCode = 0xABCDEF12
 *   param 1..3  = 0 (idle/limit/interval are already recorded in the user-mode
 *                  log and last-crash marker)
 *   param 4     = 0x4B4D5645 ("KMVE") tag to identify this bugcheck in a dump.
 */

#include <ntddk.h>

#define KM_BUGCHECK_CODE 0xABCDEF12UL
#define KM_TAG           0x4B4D5645UL

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT   DriverObject,
    _In_ PUNICODE_STRING  RegistryPath
    )
{
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);

    KeBugCheckEx(KM_BUGCHECK_CODE, 0UL, 0UL, 0UL, KM_TAG);

    /* Unreachable: KeBugCheckEx does not return. */
    return STATUS_SUCCESS;
}
