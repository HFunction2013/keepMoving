/* File logger.  One line per event, always flushed to disk. */

#include "km.h"
#include <stdarg.h>

static FILE     *g_file = NULL;
static SRWLOCK   g_lock = SRWLOCK_INIT;
static BOOL      g_console = FALSE;

static void km_log_rotate(const wchar_t *path, const wchar_t *old_path)
{
    WIN32_FILE_ATTRIBUTE_DATA fad;
    ULONGLONG                 size;

    if (GetFileAttributesExW(path, GetFileExInfoStandard, &fad) == 0)
        return;

    size = ((ULONGLONG)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    if (size < (2ULL << 20))      /* rotate above 2 MiB */
        return;

    DeleteFileW(old_path);
    MoveFileExW(path, old_path, MOVEFILE_REPLACE_EXISTING);
}

void km_log_open(const wchar_t *path, const wchar_t *old_path)
{
    AcquireSRWLockExclusive(&g_lock);

    if (g_file != NULL) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }

    if (path != NULL)
        km_log_rotate(path, old_path);

    if (path != NULL)
        g_file = _wfopen(path, L"ab");

    ReleaseSRWLockExclusive(&g_lock);
}

void km_log_close(void)
{
    AcquireSRWLockExclusive(&g_lock);

    if (g_file != NULL) {
        fflush(g_file);
        fclose(g_file);
        g_file = NULL;
    }

    ReleaseSRWLockExclusive(&g_lock);
}

void km_log_console(BOOL on)
{
    AcquireSRWLockExclusive(&g_lock);
    g_console = on;
    ReleaseSRWLockExclusive(&g_lock);
}

void km_log_flush(void)
{
    AcquireSRWLockExclusive(&g_lock);
    if (g_file != NULL)
        fflush(g_file);
    ReleaseSRWLockExclusive(&g_lock);
}

void km_logf(const char *level, const char *fmt, ...)
{
    char       msg[1024];
    char       line[1280];
    SYSTEMTIME st;
    va_list    ap;
    int        truncated = 0;

    msg[0] = 0;

    va_start(ap, fmt);
    truncated = _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    if (truncated < 0)
        StringCchCatA(msg, sizeof(msg), "...(truncated)");

    GetLocalTime(&st);

    _snprintf_s(line, sizeof(line), _TRUNCATE,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u pid=%-5lu tid=%-5lu %-5s %s",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
                st.wMilliseconds,
                (unsigned long)GetCurrentProcessId(),
                (unsigned long)GetCurrentThreadId(),
                level, msg);

    AcquireSRWLockExclusive(&g_lock);

    if (g_file != NULL) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);            /* the log is the only crash evidence */
    }

    if (g_console)
        printf("%s\n", line);

    ReleaseSRWLockExclusive(&g_lock);
}
