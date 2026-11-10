/*
 * Input activity tracking.
 *
 * The only source used is GetLastInputInfo(), which reports the tick count of
 * the most recent keyboard or mouse event for the current session.  It is a
 * documented user mode API, it needs no hook, no service and no change to any
 * system setting, which is exactly what was asked for.  If it is unavailable
 * the watchdog refuses to start instead of silently doing nothing.
 */

#include "km.h"

BOOL km_input_probe(char *err, size_t cap)
{
    LASTINPUTINFO li;
    wchar_t       name[256];

    ZeroMemory(&li, sizeof(li));
    li.cbSize = sizeof(li);

    if (GetProcessWindowStation() == NULL) {
        if (err)
            StringCchCopyA(err, cap,
                           "process has no window station (non interactive session)");
        return FALSE;
    }

    name[0] = 0;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME,
                              name, sizeof(name) - sizeof(wchar_t), NULL);

    if (!GetLastInputInfo(&li)) {
        if (err)
            StringCchPrintfA(err, cap,
                             "GetLastInputInfo failed (windowing=%ls)",
                             name[0] ? name : L"?");
        return FALSE;
    }

    return TRUE;
}

BOOL km_input_last_tick(DWORD *tick)
{
    LASTINPUTINFO li;

    if (tick == NULL)
        return FALSE;

    ZeroMemory(&li, sizeof(li));
    li.cbSize = sizeof(li);

    if (!GetLastInputInfo(&li))
        return FALSE;

    *tick = li.dwTime;
    return TRUE;
}
