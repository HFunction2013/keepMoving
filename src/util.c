/* Small helpers: error formatting, path discovery, process probes. */

#include "km.h"

void km_win32_err(DWORD code, char *buf, size_t cap)
{
    char  tmp[512];
    char *p;

    if (buf == NULL || cap == 0)
        return;

    buf[0] = 0;
    tmp[0] = 0;

    if (code != 0) {
        if (FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                           NULL, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                           tmp, (DWORD)sizeof(tmp), NULL) != 0) {
            /* Drop the trailing CR/LF that the message table carries. */
            for (p = tmp; *p; ++p) {
                if (*p == '\r' || *p == '\n') {
                    *p = 0;
                    break;
                }
            }
        } else {
            StringCchCopyA(tmp, sizeof(tmp), "no message text available");
        }

        _snprintf_s(buf, cap, _TRUNCATE, "Win32 error %lu (0x%08lX): %s",
                    (unsigned long)code, (unsigned long)code, tmp);
    }
}

ULONGLONG km_ft_to_u64(FILETIME ft)
{
    return ((ULONGLONG)ft.dwHighDateTime << 32) | (ULONGLONG)ft.dwLowDateTime;
}

ULONGLONG km_self_create_time(void)
{
    FILETIME  c, e, k, u;
    ULONGLONG v = 0;

    if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        v = km_ft_to_u64(c);

    return v;
}

BOOL km_pid_create_time(DWORD pid, ULONGLONG *out)
{
    HANDLE    h;
    FILETIME  c, e, k, u;
    BOOL      ok = FALSE;

    if (out)
        *out = 0;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL)
        return FALSE;

    if (GetProcessTimes(h, &c, &e, &k, &u)) {
        if (out)
            *out = km_ft_to_u64(c);
        ok = TRUE;
    }

    CloseHandle(h);
    return ok;
}

BOOL km_pid_alive(DWORD pid, ULONGLONG expected_create)
{
    HANDLE    h;
    DWORD     code = 0;
    BOOL      alive = FALSE;
    ULONGLONG created = 0;

    if (pid == 0 || pid == (DWORD)-1)
        return FALSE;

    h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == NULL)
        return FALSE;

    if (GetExitCodeProcess(h, &code) && code == STILL_ACTIVE) {
        if (km_pid_create_time(pid, &created) && expected_create != 0)
            alive = (created == expected_create);
        else
            alive = TRUE;   /* no timestamp recorded: trust the pid */
    }

    CloseHandle(h);
    return alive;
}

void km_now_str(char *buf, size_t cap)
{
    SYSTEMTIME st;

    if (buf == NULL || cap == 0)
        return;

    GetLocalTime(&st);
    _snprintf_s(buf, cap, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u",
                st.wYear, st.wMonth, st.wDay,
                st.wHour, st.wMinute, st.wSecond);
}

BOOL km_mkdir_p(const wchar_t *dir)
{
    wchar_t tmp[MAX_PATH];
    size_t  i;
    size_t  len;

    if (dir == NULL || dir[0] == 0)
        return FALSE;

    len = wcslen(dir);
    if (len >= MAX_PATH)
        return FALSE;

    StringCchCopyW(tmp, MAX_PATH, dir);

    /* Walk over every separator after the drive root, creating each level. */
    for (i = 3; i < len; ++i) {
        if (tmp[i] == L'\\' || tmp[i] == L'/') {
            wchar_t saved = tmp[i];

            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = saved;
        }
    }

    if (CreateDirectoryW(tmp, NULL))
        return TRUE;

    return (GetLastError() == ERROR_ALREADY_EXISTS);
}

void km_json_str(const wchar_t *src, char *dst, size_t cap)
{
    char   narrow[1024];
    size_t i;
    size_t o = 0;

    if (dst == NULL || cap == 0)
        return;

    dst[0] = 0;

    if (src == NULL)
        return;

    if (!WideCharToMultiByte(CP_UTF8, 0, src, -1, narrow, (int)sizeof(narrow),
                             NULL, NULL))
        return;

    for (i = 0; narrow[i] != 0 && (o + 2) < cap; ++i) {
        unsigned char c = (unsigned char)narrow[i];

        if (c == '\\' || c == '"') {
            dst[o++] = '\\';
            dst[o++] = (char)c;
        } else if (c < 0x20) {
            dst[o++] = ' ';
        } else {
            dst[o++] = (char)c;
        }
    }

    dst[o] = 0;
}

BOOL km_paths_init(km_paths *p, char *err, size_t cap)
{
    const wchar_t *base = NULL;
    wchar_t        root[MAX_PATH];

    if (p == NULL)
        return FALSE;

    ZeroMemory(p, sizeof(*p));

    base = _wgetenv(L"LOCALAPPDATA");
    if (base != NULL && base[0] != 0) {
        StringCchCopyW(root, MAX_PATH, base);
    } else {
        base = _wgetenv(L"TEMP");
        if (base != NULL && base[0] != 0) {
            StringCchCopyW(root, MAX_PATH, base);
        } else {
            base = _wgetenv(L"USERPROFILE");
            if (base != NULL && base[0] != 0)
                StringCchCopyW(root, MAX_PATH, base);
            else
                GetCurrentDirectoryW(MAX_PATH, root);
        }
    }

    StringCchPrintfW(p->dir, MAX_PATH, L"%s\\keepMoving", root);

    if (!km_mkdir_p(p->dir)) {
        DWORD code = GetLastError();

        if (err)
            km_win32_err(code, err, cap);
        return FALSE;
    }

    StringCchPrintfW(p->log, MAX_PATH, L"%s\\keepMoving.log", p->dir);
    StringCchPrintfW(p->log_old, MAX_PATH, L"%s\\keepMoving.log.1", p->dir);
    StringCchPrintfW(p->state, MAX_PATH, L"%s\\state.ini", p->dir);
    StringCchPrintfW(p->state_tmp, MAX_PATH, L"%s\\state.ini.tmp", p->dir);
    StringCchPrintfW(p->crash, MAX_PATH, L"%s\\last-crash.txt", p->dir);

    return TRUE;
}
