/*
 * keepMoving - idle watchdog that bugchecks the machine when the user
 * stops touching the keyboard or the mouse.
 *
 * Public interface of the project.  Everything here is user mode code.
 */

#ifndef KM_H
#define KM_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <strsafe.h>

#define KM_VERSION_STR "1.0.0"
#define KM_APP_NAME    L"keepMoving"

/* Bugcheck code that will be shown on the blue screen. */
#define KM_BUGCHECK_CODE 0xABCDEF12UL

/* HARDERROR_RESPONSE_OPTION (winnt.h, zero based) */
#define KM_OPTION_SHUTDOWN_SYSTEM 6UL

/* Interval limits, in seconds. */
#define KM_MIN_INTERVAL_SEC 1UL
#define KM_MAX_INTERVAL_SEC 604800UL   /* 7 days */

/* Named kernel objects, session local so several interactive users stay apart. */
#define KM_EVENT_STOP L"Local\\keepMoving.stop"
#define KM_MUTEX_ONE  L"Local\\keepMoving.singleton"

/* State file format tag. */
#define KM_STATE_FORMAT "keepMoving-state-1"

/* Exit codes. */
#define KM_EXIT_OK               0
#define KM_EXIT_USAGE            1
#define KM_EXIT_ALREADY_RUNNING  2
#define KM_EXIT_NOT_RUNNING      3
#define KM_EXIT_ERROR            4
#define KM_EXIT_NO_INPUT_WATCH   5
#define KM_EXIT_CRASH_CALL_FAIL  7

/* Watchdog behaviour when the idle limit is reached. */
#define KM_MODE_CRASH 0   /* bugcheck the machine (NtRaiseHardError) */
#define KM_MODE_DRY   1   /* log and disarm, no crash, no dialog */
#define KM_MODE_WARN  2   /* pop a topmost message box, then disarm */

/* ------------------------------------------------------------------ paths */
typedef struct km_paths_s {
    wchar_t dir[MAX_PATH];
    wchar_t log[MAX_PATH];
    wchar_t log_old[MAX_PATH];
    wchar_t state[MAX_PATH];
    wchar_t state_tmp[MAX_PATH];
    wchar_t crash[MAX_PATH];
} km_paths;

/* ------------------------------------------------------------------ state */
typedef struct km_state_s {
    int       valid;
    DWORD     pid;
    DWORD     interval;
    int       dry_run;
    int       warn_run;
    ULONGLONG proc_create;   /* creation timestamp of the worker process */
} km_state;

/* -------------------------------------------------------------------- api */
/* util.c */
void       km_win32_err(DWORD code, char *buf, size_t cap);
ULONGLONG  km_ft_to_u64(FILETIME ft);
ULONGLONG  km_self_create_time(void);
void       km_now_str(char *buf, size_t cap);
BOOL       km_mkdir_p(const wchar_t *dir);
BOOL       km_paths_init(km_paths *p, char *err, size_t cap);
BOOL       km_pid_alive(DWORD pid, ULONGLONG expected_create);
BOOL       km_pid_create_time(DWORD pid, ULONGLONG *out);
void       km_json_str(const wchar_t *src, char *dst, size_t cap);

/* log.c */
void  km_log_open(const wchar_t *path, const wchar_t *old_path);
void  km_log_close(void);
void  km_log_console(BOOL on);
void  km_log_flush(void);
void  km_logf(const char *level, const char *fmt, ...);

#define KM_DEBUG(...) km_logf("DEBUG", __VA_ARGS__)
#define KM_INFO(...)  km_logf("INFO",  __VA_ARGS__)
#define KM_WARN(...)  km_logf("WARN",  __VA_ARGS__)
#define KM_ERROR(...) km_logf("ERROR", __VA_ARGS__)

/* state.c */
BOOL  km_state_read(const km_paths *p, km_state *st);
BOOL  km_state_write(const km_paths *p, const km_state *st);
BOOL  km_state_remove(const km_paths *p);
BOOL  km_crash_marker_read(const km_paths *p, char *buf, size_t cap);
BOOL  km_crash_marker_write(const km_paths *p, const char *text);
BOOL  km_crash_marker_clear(const km_paths *p);

/* input.c */
BOOL  km_input_probe(char *err, size_t cap);
BOOL  km_input_last_tick(DWORD *tick);
/*
 * Idle time in milliseconds, reconstructed against GetTickCount64 so a 32-bit
 * tick wrap at ~49.7 days is handled explicitly.  Returns (ULONGLONG)-1 when
 * GetLastInputInfo itself fails.
 */
ULONGLONG km_input_idle_ms(void);

/* bsod.c */
BOOL  km_enable_privilege(const wchar_t *name, BOOL *held, char *err, size_t cap);
int   km_raise_hard_error(ULONG_PTR a1, ULONG_PTR a2, ULONG_PTR a3,
                          ULONG_PTR a4, char *err, size_t cap);

/* worker.c */
int   km_worker_core(DWORD interval, int mode, BOOL console);
int   km_cmd_start(DWORD interval, int mode, int quiet);
int   km_cmd_stop(int force);
int   km_cmd_query(int as_json, int clear_crash, int watch);
int   km_cmd_diag(void);
int   km_cmd_run(DWORD interval, int mode);
int   km_cmd_crash(void);

#endif /* KM_H */
