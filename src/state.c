/* Persisted state: who the watchdog is, and why the last crash happened. */

#include "km.h"

static BOOL kv_u64(const char *line, const char *key, ULONGLONG *out)
{
    size_t             klen = strlen(key);
    const char        *v;
    char              *end = NULL;
    unsigned long long parsed;

    if (strncmp(line, key, klen) != 0 || line[klen] != '=')
        return FALSE;

    v = line + klen + 1;
    parsed = _strtoui64(v, &end, 10);
    if (end == v)
        return FALSE;

    *out = (ULONGLONG)parsed;
    return TRUE;
}

static void trim_eol(char *line)
{
    size_t len = strlen(line);

    while (len > 0) {
        char c = line[len - 1];
        if (c == '\r' || c == '\n') {
            line[len - 1] = 0;
            --len;
        } else {
            break;
        }
    }
}

BOOL km_state_write(const km_paths *p, const km_state *st)
{
    FILE *f;
    char  now[64];

    if (p == NULL || st == NULL)
        return FALSE;

    km_now_str(now, sizeof(now));

    f = _wfopen(p->state_tmp, L"wb");
    if (f == NULL)
        return FALSE;

    fprintf(f, "format=%s\n", KM_STATE_FORMAT);
    fprintf(f, "version=%s\n", KM_VERSION_STR);
    fprintf(f, "pid=%lu\n", (unsigned long)st->pid);
    fprintf(f, "interval=%lu\n", (unsigned long)st->interval);
    fprintf(f, "dry_run=%d\n", st->dry_run);
    fprintf(f, "warn_run=%d\n", st->warn_run);
    fprintf(f, "proc_create=%llu\n", (unsigned long long)st->proc_create);
    fprintf(f, "written=%s\n", now);

    fflush(f);
    fclose(f);

    if (!MoveFileExW(p->state_tmp, p->state,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(p->state_tmp);
        return FALSE;
    }

    return TRUE;
}

BOOL km_state_read(const km_paths *p, km_state *st)
{
    FILE *f;
    char  line[512];
    BOOL  have_format = FALSE;

    if (p == NULL || st == NULL)
        return FALSE;

    ZeroMemory(st, sizeof(*st));

    f = _wfopen(p->state, L"rb");
    if (f == NULL)
        return FALSE;

    while (fgets(line, sizeof(line), f) != NULL) {
        ULONGLONG v = 0;

        trim_eol(line);

        if (strncmp(line, "format=", 7) == 0) {
            have_format = (strcmp(line + 7, KM_STATE_FORMAT) == 0);
        } else if (kv_u64(line, "pid", &v)) {
            st->pid = (DWORD)v;
        } else if (kv_u64(line, "interval", &v)) {
            st->interval = (DWORD)v;
        } else if (kv_u64(line, "dry_run", &v)) {
            st->dry_run = (int)(v != 0);
        } else if (kv_u64(line, "warn_run", &v)) {
            st->warn_run = (int)(v != 0);
        } else if (kv_u64(line, "proc_create", &v)) {
            st->proc_create = v;
        }
    }

    fclose(f);

    st->valid = have_format && st->pid != 0;
    return st->valid;
}

BOOL km_state_remove(const km_paths *p)
{
    if (p == NULL)
        return FALSE;

    DeleteFileW(p->state_tmp);
    return DeleteFileW(p->state);
}

BOOL km_crash_marker_write(const km_paths *p, const char *text)
{
    FILE *f;

    if (p == NULL || text == NULL)
        return FALSE;

    f = _wfopen(p->crash, L"wab");
    if (f == NULL)
        return FALSE;

    fputs(text, f);
    fflush(f);
    fclose(f);
    return TRUE;
}

BOOL km_crash_marker_read(const km_paths *p, char *buf, size_t cap)
{
    FILE *f;
    size_t got;

    if (p == NULL || buf == NULL || cap == 0)
        return FALSE;

    buf[0] = 0;

    f = _wfopen(p->crash, L"rb");
    if (f == NULL)
        return FALSE;

    got = fread(buf, 1, cap - 1, f);
    buf[got] = 0;
    fclose(f);

    /* Keep only the first line for display purposes. */
    {
        char *nl = strchr(buf, '\n');
        if (nl != NULL)
            *nl = 0;
    }

    return (got != 0);
}

BOOL km_crash_marker_clear(const km_paths *p)
{
    if (p == NULL)
        return FALSE;

    return DeleteFileW(p->crash);
}
