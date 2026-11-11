/*
 * The watchdog itself, plus every CLI command that touches it.
 *
 * start : creates a fully detached worker process, verifies it came up, exits.
 * stop  : signals the worker through a named event and waits for it to leave.
 * query : prints the state of the watchdog and how close it is to firing.
 */

#include "km.h"

#define KM_POLL_MS            50UL
#define KM_HEARTBEAT_MS       30000UL
#define KM_CRASH_RETRY_MS     60000UL
#define KM_START_TIMEOUT_MS   2000UL
#define KM_STOP_TIMEOUT_MS    10000UL
#define KM_WATCH_TICK_MS      1000UL

static HANDLE g_stop_event = NULL;

static BOOL WINAPI km_ctrl_handler(DWORD type)
{
    (void)type;
    if (g_stop_event != NULL)
        SetEvent(g_stop_event);
    return TRUE;
}

static void km_format_uptime(ULONGLONG seconds, char *buf, size_t cap)
{
    _snprintf_s(buf, cap, _TRUNCATE, "%02llu:%02llu:%02llu",
                (unsigned long long)(seconds / 3600),
                (unsigned long long)((seconds % 3600) / 60),
                (unsigned long long)(seconds % 60));
}

static int km_try_crash(DWORD interval, DWORD idle_ms, ULONG limit_ms)
{
    km_paths paths;
    char     err[256];
    char     detail[512];
    char     now[64];
    char     e1[256];
    char     e2[256];
    BOOL     held = FALSE;
    BOOL     ok;
    int      status;

    if (!km_paths_init(&paths, err, sizeof(err)))
        return KM_EXIT_CRASH_CALL_FAIL;

    KM_WARN("IDLE LIMIT REACHED: no keyboard/mouse input for %.1f s (limit %lu s)",
            (double)idle_ms / 1000.0, (unsigned long)interval);

    km_now_str(now, sizeof(now));
    _snprintf_s(detail, sizeof(detail), _TRUNCATE,
                "time=%s\r\npid=%lu\r\ninterval_sec=%lu\r\nidle_ms=%lu\r\n"
                "bugcheck=0x%08lX\r\nmode=crash\r\n",
                now, (unsigned long)GetCurrentProcessId(),
                (unsigned long)interval, (unsigned long)idle_ms,
                (unsigned long)KM_BUGCHECK_CODE);

    if (km_crash_marker_write(&paths, detail))
        KM_INFO("crash marker written to %ls", paths.crash);
    else
        KM_ERROR("failed to write crash marker %ls", paths.crash);

    km_log_flush();

    e1[0] = 0;
    ok = km_enable_privilege(SE_SHUTDOWN_NAME, &held, e1, sizeof(e1));
    KM_INFO("SeShutdownPrivilege: %s%s%s",
            ok ? "enabled" : "NOT enabled",
            ok ? "" : " - ",
            ok ? "" : e1);

    e2[0] = 0;
    ok = km_enable_privilege(SE_TCB_NAME, &held, e2, sizeof(e2));
    KM_INFO("SeTcbPrivilege: %s%s%s",
            ok ? "enabled" : "NOT enabled",
            ok ? "" : " - ",
            ok ? "" : e2);

    KM_INFO("calling NtRaiseHardError(status=0x%08lX, params={idle_ms=%lu, "
            "limit_ms=%lu, interval_sec=%lu, tag=0x%08lX}, "
            "response_option=%lu)",
            (unsigned long)KM_BUGCHECK_CODE, (unsigned long)idle_ms,
            (unsigned long)limit_ms, (unsigned long)interval,
            0x4B4D5645UL, (unsigned long)KM_OPTION_SHUTDOWN_SYSTEM);

    km_log_flush();

    e2[0] = 0;
    status = km_raise_hard_error((ULONG_PTR)idle_ms, (ULONG_PTR)limit_ms,
                                (ULONG_PTR)interval, (ULONG_PTR)0x4B4D5645UL,
                                e2, sizeof(e2));

    KM_ERROR("NtRaiseHardError did not stop the machine: %s", e2);
    KM_ERROR("the watchdog stays armed and will retry in %lu s",
             (unsigned long)(KM_CRASH_RETRY_MS / 1000));
    km_log_flush();

    (void)status;
    return KM_EXIT_CRASH_CALL_FAIL;
}

static void km_show_warn_box(DWORD interval, DWORD idle_ms)
{
    wchar_t text[320];
    int     r;

    StringCchPrintfW(text, 320,
                     L"keepMoving: no keyboard or mouse input for %lu s "
                     L"(limit %lu s).\r\n\r\n"
                     L"Press OK to disarm the watchdog.",
                     (unsigned long)(idle_ms / 1000),
                     (unsigned long)interval);

    KM_WARN("idle limit reached in warn mode, showing topmost dialog");
    km_log_flush();

    r = MessageBoxW(NULL, text, L"keepMoving",
                    MB_OK | MB_ICONWARNING | MB_TOPMOST |
                    MB_SETFOREGROUND | MB_SYSTEMMODAL);

    KM_INFO("warning dialog dismissed (result=%d), disarming", r);
}

int km_worker_core(DWORD interval, int mode, BOOL console)
{
    km_paths   paths;
    km_state   st;
    char       err[256];
    HANDLE     mutex = NULL;
    HANDLE     stop_ev = NULL;
    DWORD      waitres;
    ULONG      limit_ms;
    ULONGLONG  now64 = 0;
    ULONGLONG  idle = 0;
    ULONGLONG  remaining = 0;
    ULONGLONG  last_beat = 0;
    ULONGLONG  crash_retry_until = 0;
    DWORD      input_fail = 0;
    ULONGLONG  input_fail_log = 0;
    BOOL       warned_30 = FALSE;
    BOOL       warned_10 = FALSE;
    int        rc = KM_EXIT_OK;

    err[0] = 0;

    if (!km_paths_init(&paths, err, sizeof(err))) {
        if (console)
            printf("keepMoving: cannot prepare data directory: %s\n", err);
        return KM_EXIT_ERROR;
    }

    km_log_open(paths.log, paths.log_old);
    if (console) {
        km_log_console(TRUE);
        SetConsoleCtrlHandler(km_ctrl_handler, TRUE);
    }

    KM_INFO("========================================================");
    KM_INFO("keepMoving worker starting, version %s", KM_VERSION_STR);
    KM_INFO("pid=%lu interval=%lu s mode=%d console=%d",
            (unsigned long)GetCurrentProcessId(), (unsigned long)interval,
            mode, (int)console);
    KM_INFO("data directory: %ls", paths.dir);
    KM_INFO("log file      : %ls", paths.log);

    /*
     * Single instance.  A mutex held by a dead process turns abandoned, so a
     * stale entry can never block a new start.
     */
    mutex = CreateMutexW(NULL, FALSE, KM_MUTEX_ONE);
    if (mutex == NULL) {
        KM_ERROR("CreateMutex failed");
        rc = KM_EXIT_ERROR;
        goto cleanup;
    }

    waitres = WaitForSingleObject(mutex, 0);
    if (waitres == WAIT_TIMEOUT) {
        KM_WARN("another keepMoving worker already owns the singleton mutex");
        rc = KM_EXIT_ALREADY_RUNNING;
        goto cleanup;
    }
    if (waitres == WAIT_ABANDONED)
        KM_WARN("previous worker died without releasing the singleton mutex");

    stop_ev = CreateEventW(NULL, TRUE, FALSE, KM_EVENT_STOP);
    if (stop_ev == NULL) {
        KM_ERROR("CreateEvent(%ls) failed: %lu", KM_EVENT_STOP,
                 (unsigned long)GetLastError());
        rc = KM_EXIT_ERROR;
        goto cleanup;
    }

    ResetEvent(stop_ev);
    g_stop_event = stop_ev;

    /*
     * Refuse to pretend.  If input activity cannot be observed there is no
     * point in being armed: log the reason and leave.
     */
    if (!km_input_probe(err, sizeof(err))) {
        KM_ERROR("cannot read keyboard/mouse activity: %s", err);
        KM_ERROR("watchdog NOT armed. user mode polling needs an interactive "
                 "window station; run keepMoving from your own desktop session");
        rc = KM_EXIT_NO_INPUT_WATCH;
        goto cleanup;
    }

    KM_INFO("input source: GetLastInputInfo (user mode, no hooks, no driver)");

    ZeroMemory(&st, sizeof(st));
    st.pid = GetCurrentProcessId();
    st.interval = interval;
    st.dry_run = (mode == KM_MODE_DRY);
    st.warn_run = (mode == KM_MODE_WARN);
    st.proc_create = km_self_create_time();

    if (!km_state_write(&paths, &st)) {
        KM_ERROR("cannot write state file %ls", paths.state);
        rc = KM_EXIT_ERROR;
        goto cleanup;
    }
    KM_INFO("state file written: %ls", paths.state);

    {
        char marker[512];
        if (km_crash_marker_read(&paths, marker, sizeof(marker))
            && marker[0] != 0) {
            KM_INFO("previous crash marker present: %s", marker);
        }
    }

    limit_ms = interval * 1000UL;
    KM_INFO("watchdog armed, idle limit %lu ms, poll interval %lu ms",
            (unsigned long)limit_ms, (unsigned long)KM_POLL_MS);

    while (TRUE) {
        waitres = WaitForSingleObject(stop_ev, KM_POLL_MS);

        if (waitres == WAIT_OBJECT_0) {
            KM_INFO("stop signal received");
            break;
        }
        if (waitres != WAIT_TIMEOUT) {
            KM_WARN("unexpected wait result %lu", (unsigned long)waitres);
        }

        now64 = GetTickCount64();
        idle  = km_input_idle_ms();

        if (idle == (ULONGLONG)-1) {
            ++input_fail;
            if (input_fail == 1 ||
                (now64 - input_fail_log) >= KM_HEARTBEAT_MS) {
                KM_ERROR("GetLastInputInfo failed while the watchdog is running");
                input_fail_log = now64;
            }
            if (input_fail > 120) {
                KM_ERROR("input tracking lost permanently, disarming");
                rc = KM_EXIT_NO_INPUT_WATCH;
                break;
            }
            continue;
        }
        input_fail = 0;

        remaining = (idle >= limit_ms) ? 0ULL : ((ULONGLONG)limit_ms - idle);

        if (remaining > 30000ULL) {
            warned_30 = FALSE;
            warned_10 = FALSE;
        }

        if (idle >= limit_ms) {
            if (mode == KM_MODE_WARN) {
                km_show_warn_box(interval, (DWORD)idle);
                break;
            }
            if (mode == KM_MODE_DRY) {
                KM_INFO("dry run: limit reached, disarming without crash");
                break;
            }
            if (crash_retry_until != 0 && now64 < crash_retry_until) {
                /* cooling down after a failed crash attempt */
            } else {
                rc = km_try_crash(interval, (DWORD)idle, limit_ms);
                crash_retry_until = now64 + KM_CRASH_RETRY_MS;
                rc = KM_EXIT_OK;
            }
        } else {
            crash_retry_until = 0;

            if (remaining <= 10000ULL && !warned_10) {
                KM_WARN("10 s left: idle %.1f s of %lu s, move the mouse now",
                        (double)idle / 1000.0, (unsigned long)interval);
                warned_10 = TRUE;
            } else if (remaining <= 30000ULL && !warned_30) {
                KM_WARN("30 s left: idle %.1f s of %lu s",
                        (double)idle / 1000.0, (unsigned long)interval);
                warned_30 = TRUE;
            }
        }

        if ((now64 - last_beat) >= KM_HEARTBEAT_MS) {
            KM_INFO("heartbeat: idle %.1f s, remaining %.1f s, limit %lu s",
                    (double)idle / 1000.0, (double)remaining / 1000.0,
                    (unsigned long)interval);
            last_beat = now64;
        }
    }

cleanup:
    g_stop_event = NULL;

    if (stop_ev != NULL)
        CloseHandle(stop_ev);

    km_state_remove(&paths);

    if (mutex != NULL)
        CloseHandle(mutex);          /* releases the singleton */

    KM_INFO("worker exiting with code %d", rc);
    km_log_close();

    return rc;
}

int km_cmd_run(DWORD interval, int mode)
{
    return km_worker_core(interval, mode, TRUE);
}

static const wchar_t *km_mode_suffix(int mode)
{
    if (mode == KM_MODE_DRY)  return L" --dry-run";
    if (mode == KM_MODE_WARN) return L" --warn";
    return L"";
}

static const char *km_mode_label(int mode)
{
    if (mode == KM_MODE_DRY)  return "dry run";
    if (mode == KM_MODE_WARN) return "warn";
    return "crash";
}

int km_cmd_start(DWORD interval, int mode, int quiet)
{
    km_paths            paths;
    km_state            existing;
    km_state            fresh;
    char                err[256];
    wchar_t             exe[MAX_PATH];
    wchar_t             cmd[2 * MAX_PATH + 64];
    STARTUPINFOW        si;
    PROCESS_INFORMATION pi;
    DWORD               flags;
    BOOL                created = FALSE;
    BOOL                up = FALSE;
    DWORD               i;
    DWORD               exit_code = 0;

    if (!km_paths_init(&paths, err, sizeof(err))) {
        if (!quiet)
            printf("keepMoving: cannot prepare data directory: %s\n", err);
        return KM_EXIT_ERROR;
    }

    km_log_open(paths.log, paths.log_old);
    KM_INFO("start requested: interval=%lu s mode=%s",
            (unsigned long)interval, km_mode_label(mode));

    if (interval < 10)
        KM_WARN("interval %lu s is very short, the machine may bugcheck almost "
                "immediately", (unsigned long)interval);

    if (km_state_read(&paths, &existing) && existing.valid &&
        existing.pid != GetCurrentProcessId() &&
        km_pid_alive(existing.pid, existing.proc_create)) {
        KM_WARN("start rejected: already running pid=%lu", (unsigned long)existing.pid);
        if (!quiet)
            printf("keepMoving is already running (pid=%lu, interval=%lu s). "
                   "Run 'keepMoving stop' first.\n",
                   (unsigned long)existing.pid, (unsigned long)existing.interval);
        km_log_close();
        return KM_EXIT_ALREADY_RUNNING;
    }

    if (km_state_read(&paths, &existing) && existing.valid)
        KM_WARN("cleaning stale state of pid=%lu", (unsigned long)existing.pid);

    km_state_remove(&paths);

    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) {
        KM_ERROR("GetModuleFileName failed");
        if (!quiet)
            printf("keepMoving: cannot resolve own path\n");
        km_log_close();
        return KM_EXIT_ERROR;
    }

    StringCchPrintfW(cmd, sizeof(cmd) / sizeof(cmd[0]),
                     L"\"%s\" __worker --interval %lu%s",
                     exe, (unsigned long)interval, km_mode_suffix(mode));

    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_BREAKAWAY_FROM_JOB;

    KM_INFO("spawning: %ls", cmd);

    created = CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags, NULL, NULL,
                             &si, &pi);
    if (!created && GetLastError() == ERROR_ACCESS_DENIED) {
        KM_WARN("breakaway from job refused, retrying without it");
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        created = CreateProcessW(exe, cmd, NULL, NULL, FALSE, flags, NULL, NULL,
                                 &si, &pi);
    }

    if (!created) {
        char e[256];
        km_win32_err(GetLastError(), e, sizeof(e));
        KM_ERROR("CreateProcess failed: %s", e);
        if (!quiet)
            printf("keepMoving: failed to start the worker: %s\n", e);
        km_log_close();
        return KM_EXIT_ERROR;
    }

    KM_INFO("worker process created, pid=%lu", (unsigned long)pi.dwProcessId);

    for (i = 0; i < KM_START_TIMEOUT_MS / 100; ++i) {
        Sleep(100);

        if (km_state_read(&paths, &fresh) && fresh.valid &&
            fresh.pid == pi.dwProcessId &&
            km_pid_alive(fresh.pid, fresh.proc_create)) {
            up = TRUE;
            break;
        }

        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
            GetExitCodeProcess(pi.hProcess, &exit_code);
            break;
        }
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!up) {
        KM_ERROR("worker did not come up (exit code %lu), see %ls",
                 (unsigned long)exit_code, paths.log);
        if (!quiet) {
            printf("keepMoving: worker failed to start (exit code %lu).\n"
                   "Check the log: %ls\n", (unsigned long)exit_code, paths.log);
        }
        km_log_close();
        return KM_EXIT_ERROR;
    }

    KM_INFO("watchdog is running: pid=%lu interval=%lu s mode=%s",
            (unsigned long)pi.dwProcessId, (unsigned long)interval,
            km_mode_label(mode));

    if (!quiet) {
        printf("keepMoving: watchdog started (pid=%lu, interval=%lu s, mode=%s)\n",
               (unsigned long)pi.dwProcessId, (unsigned long)interval,
               km_mode_label(mode));
        printf("keepMoving: log %ls\n", paths.log);
    }

    km_log_close();
    return KM_EXIT_OK;
}

int km_cmd_stop(int force)
{
    km_paths paths;
    km_state st;
    char     err[256];
    HANDLE   ev = NULL;
    HANDLE   proc = NULL;
    BOOL     have;
    DWORD    waitres;
    int      rc = KM_EXIT_OK;

    if (!km_paths_init(&paths, err, sizeof(err))) {
        printf("keepMoving: cannot prepare data directory: %s\n", err);
        return KM_EXIT_ERROR;
    }

    km_log_open(paths.log, paths.log_old);
    KM_INFO("stop requested (force=%d)", force);

    have = km_state_read(&paths, &st);

    if (!have || !km_pid_alive(st.pid, st.proc_create)) {
        KM_WARN("stop requested but no live watchdog was found");
        km_state_remove(&paths);
        printf("keepMoving: no active watchdog.\n");
        km_log_close();
        return KM_EXIT_NOT_RUNNING;
    }

    KM_INFO("stopping worker pid=%lu", (unsigned long)st.pid);

    ev = OpenEventW(EVENT_MODIFY_STATE, FALSE, KM_EVENT_STOP);
    if (ev == NULL)
        ev = CreateEventW(NULL, TRUE, FALSE, KM_EVENT_STOP);

    if (ev != NULL) {
        SetEvent(ev);
        CloseHandle(ev);
        KM_INFO("stop event signalled");
    } else {
        KM_ERROR("cannot open stop event, will rely on termination only");
    }

    proc = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE |
                       PROCESS_QUERY_LIMITED_INFORMATION, FALSE, st.pid);
    if (proc == NULL) {
        char e[256];
        km_win32_err(GetLastError(), e, sizeof(e));
        KM_ERROR("OpenProcess(%lu) failed: %s", (unsigned long)st.pid, e);
        printf("keepMoving: cannot reach worker pid %lu: %s\n",
               (unsigned long)st.pid, e);
        km_log_close();
        return KM_EXIT_ERROR;
    }

    waitres = WaitForSingleObject(proc, KM_STOP_TIMEOUT_MS);

    if (waitres != WAIT_OBJECT_0) {
        if (force) {
            KM_WARN("graceful stop timed out, terminating pid=%lu",
                    (unsigned long)st.pid);
            TerminateProcess(proc, 1);
            WaitForSingleObject(proc, 2000);
            rc = KM_EXIT_OK;
        } else {
            KM_ERROR("worker did not exit within %lu ms",
                     (unsigned long)KM_STOP_TIMEOUT_MS);
            printf("keepMoving: worker pid %lu did not exit, retry with "
                   "'keepMoving stop --force'\n", (unsigned long)st.pid);
            CloseHandle(proc);
            km_log_close();
            return KM_EXIT_ERROR;
        }
    } else {
        KM_INFO("worker exited cleanly");
    }

    CloseHandle(proc);
    km_state_remove(&paths);

    printf("keepMoving: watchdog stopped (pid=%lu).\n", (unsigned long)st.pid);
    km_log_close();
    return rc;
}

int km_cmd_query(int as_json, int clear_crash, int watch)
{
    km_paths  paths;
    km_state  st;
    char      err[256];
    char      marker[512];
    char      log_json[1024];
    char      uptime[32];
    BOOL      running = FALSE;
    ULONGLONG created = 0;
    BOOL      have_tick = FALSE;
    double    idle_s = 0.0;
    double    remaining_s = 0.0;
    ULONGLONG uptime_s = 0;

    if (!km_paths_init(&paths, err, sizeof(err))) {
        printf("keepMoving: cannot prepare data directory: %s\n", err);
        return KM_EXIT_ERROR;
    }

    ZeroMemory(&st, sizeof(st));

    if (km_state_read(&paths, &st) && st.valid)
        running = (st.pid == GetCurrentProcessId()) ||
                  km_pid_alive(st.pid, st.proc_create);

    if (running) {
        created = 0;
        km_pid_create_time(st.pid, &created);
        if (created != 0) {
            FILETIME  sysnow;
            ULONGLONG now64;

            GetSystemTimeAsFileTime(&sysnow);
            now64 = km_ft_to_u64(sysnow);
            if (now64 > created)
                uptime_s = (now64 - created) / 10000000ULL;
        }

        {
            ULONGLONG idle = km_input_idle_ms();

            have_tick = (idle != (ULONGLONG)-1);
            if (have_tick) {
                double limit_ms = (double)st.interval * 1000.0;

                idle_s = (double)idle / 1000.0;
                remaining_s = (limit_ms - (double)idle) / 1000.0;
                if (remaining_s < 0.0)
                    remaining_s = 0.0;
            }
        }
    }

    km_format_uptime(uptime_s, uptime, sizeof(uptime));
    marker[0] = 0;
    km_crash_marker_read(&paths, marker, sizeof(marker));
    km_json_str(paths.log, log_json, sizeof(log_json));

    if (watch && !as_json) {
        printf("watching keepMoving (Ctrl+C to stop)...\n");
        while (TRUE) {
            km_state  cur;
            ULONGLONG cur_idle;
            double    i_s;
            double    rem_s;
            double    lim;
            BOOL      still;

            ZeroMemory(&cur, sizeof(cur));
            still = (km_state_read(&paths, &cur) && cur.valid &&
                     km_pid_alive(cur.pid, cur.proc_create));

            if (!still) {
                printf("\r[STOPPED] watchdog is not running.                              \n");
                break;
            }

            cur_idle = km_input_idle_ms();
            if (cur_idle == (ULONGLONG)-1) {
                printf("\r[RUNNING] pid=%-5lu idle=n/a (GetLastInputInfo failed)     ",
                       (unsigned long)cur.pid);
            } else {
                lim = (double)cur.interval * 1000.0;
                i_s = (double)cur_idle / 1000.0;
                rem_s = (lim - (double)cur_idle) / 1000.0;
                if (rem_s < 0.0)
                    rem_s = 0.0;
                printf("\r[RUNNING] idle %6.1fs / %-5lus   remaining %6.1fs   "
                       "pid=%-5lu%-12s ",
                       i_s, (unsigned long)cur.interval, rem_s,
                       (unsigned long)cur.pid,
                       cur.warn_run ? " (warn)" : (cur.dry_run ? " (dry)" : ""));
            }
            fflush(stdout);
            Sleep(KM_WATCH_TICK_MS);
        }
        return KM_EXIT_OK;
    }

    if (as_json) {
        if (running) {
            printf("{\"state\":\"running\",\"pid\":%lu,\"interval_sec\":%lu,"
                   "\"mode\":\"%s\",\"idle_sec\":%.1f,\"remaining_sec\":%.1f,"
                   "\"uptime_sec\":%llu,\"input_readable\":%s,\"log\":\"%s\"%s%s}\n",
                   (unsigned long)st.pid, (unsigned long)st.interval,
                   st.warn_run ? "warn" : (st.dry_run ? "dry" : "crash"),
                   idle_s, remaining_s, (unsigned long long)uptime_s,
                   have_tick ? "true" : "false",
                   log_json,
                   marker[0] ? ",\"last_crash\":\"" : "",
                   marker[0] ? marker : "");
        } else {
            printf("{\"state\":\"stopped\",\"log\":\"%s\"%s%s}\n",
                   log_json,
                   marker[0] ? ",\"last_crash\":\"" : "",
                   marker[0] ? marker : "");
        }
    } else {
        if (running) {
            printf("state     : RUNNING%s%s\n",
                   st.dry_run ? " (dry run)" : "",
                   st.warn_run ? " (warn)" : "");
            printf("pid       : %lu\n", (unsigned long)st.pid);
            printf("interval  : %lu s\n", (unsigned long)st.interval);
            if (have_tick) {
                printf("idle      : %.1f s\n", idle_s);
                printf("remaining : %.1f s\n", remaining_s);
            } else {
                printf("idle      : unknown (GetLastInputInfo failed here)\n");
            }
            printf("started   : uptime %s\n", uptime);
            printf("mechanism : GetLastInputInfo polling every %lu ms\n",
                   (unsigned long)KM_POLL_MS);
            printf("bugcheck  : 0x%08lX (NtRaiseHardError, OptionShutdownSystem)\n",
                   (unsigned long)KM_BUGCHECK_CODE);
        } else {
            printf("state     : STOPPED\n");
            if (km_state_read(&paths, &st) && st.valid)
                printf("note      : stale state of pid %lu was ignored\n",
                       (unsigned long)st.pid);
        }
        printf("log       : %ls\n", paths.log);
        if (marker[0])
            printf("last crash: %s\n", marker);
    }

    if (clear_crash && marker[0]) {
        km_crash_marker_clear(&paths);
        if (!as_json)
            printf("last crash marker cleared\n");
    }

    return KM_EXIT_OK;
}

int km_cmd_diag(void)
{
    km_paths     paths;
    km_state     st;
    char         err[256];
    char         e[256];
    DWORD        session = 0;
    DWORD        sid = 0;
    wchar_t      wsname[256];
    DWORD        wslen = 0;
    BOOL         glii_ok;
    ULONGLONG    diag_idle;
    BOOL         held = FALSE;
    BOOL         ok;
    wchar_t      exe[MAX_PATH];
    ULONGLONG    created = 0;

    if (!km_paths_init(&paths, err, sizeof(err))) {
        printf("keepMoving: cannot prepare data directory: %s\n", err);
        return KM_EXIT_ERROR;
    }

    km_log_open(paths.log, paths.log_old);

    ZeroMemory(&st, sizeof(st));

    GetModuleFileNameW(NULL, exe, MAX_PATH);

    wsname[0] = 0;
    GetUserObjectInformationW(GetProcessWindowStation(), UOI_NAME, wsname,
                              sizeof(wsname) - sizeof(wchar_t), &wslen);

    diag_idle = km_input_idle_ms();
    glii_ok = (diag_idle != (ULONGLONG)-1);

    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    sid = session;

    printf("keepMoving diagnostics\n");
    printf("version        : %s\n", KM_VERSION_STR);
    printf("exe            : %ls\n", exe);
    printf("pid            : %lu\n", (unsigned long)GetCurrentProcessId());
    printf("session id     : %lu\n", (unsigned long)sid);
    printf("window station : %ls\n", wsname[0] ? wsname : L"(none)");
    printf("interactive    : %s\n", wsname[0] ? "yes" : "no");
    printf("GetLastInputInfo: %s", glii_ok ? "ok" : "FAILED");
    if (glii_ok) {
        printf(" (last input %.1f s ago)\n", (double)diag_idle / 1000.0);
    } else {
        printf("\n");
    }
    printf("tick count64  : %llu ms (since boot)\n",
           (unsigned long long)GetTickCount64());

    e[0] = 0;
    ok = km_enable_privilege(SE_SHUTDOWN_NAME, &held, e, sizeof(e));
    printf("SeShutdownPriv: %s%s%s\n", ok ? "held" : "NOT held",
           ok ? "" : " - ", ok ? "" : e);

    e[0] = 0;
    ok = km_enable_privilege(SE_TCB_NAME, &held, e, sizeof(e));
    printf("SeTcbPriv     : %s%s%s\n", ok ? "held" : "NOT held",
           ok ? "" : " - ", ok ? "" : e);

    printf("data dir       : %ls\n", paths.dir);
    printf("log            : %ls\n", paths.log);

    if (km_state_read(&paths, &st) && st.valid) {
        ULONGLONG alive_create = 0;

        km_pid_create_time(st.pid, &alive_create);
        created = alive_create;
        printf("state file     : pid=%lu interval=%lu s mode=%s alive=%s\n",
               (unsigned long)st.pid, (unsigned long)st.interval,
               st.warn_run ? "warn" : (st.dry_run ? "dry" : "crash"),
               km_pid_alive(st.pid, st.proc_create) ? "yes" : "no");
    } else {
        printf("state file     : none\n");
    }

    KM_INFO("diag: session=%lu windsta=%ls glii=%d tick64=%llu",
            (unsigned long)sid, wsname[0] ? wsname : L"(none)", (int)glii_ok,
            (unsigned long long)GetTickCount64());

    km_log_close();
    (void)created;
    return KM_EXIT_OK;
}

int km_cmd_crash(void)
{
    km_paths paths;
    char     err[256];
    char     e[256];
    BOOL     ok;
    BOOL     held = FALSE;
    int      status;

    if (!km_paths_init(&paths, err, sizeof(err))) {
        printf("keepMoving: cannot prepare data directory: %s\n", err);
        return KM_EXIT_ERROR;
    }

    km_log_open(paths.log, paths.log_old);

    KM_WARN("manual crash requested, raising NtRaiseHardError(0x%08lX) now",
            (unsigned long)KM_BUGCHECK_CODE);
    km_log_flush();

    printf("keepMoving: raising bugcheck 0x%08lX, the machine will stop now.\n",
           (unsigned long)KM_BUGCHECK_CODE);
    fflush(stdout);

    e[0] = 0;
    ok = km_enable_privilege(SE_SHUTDOWN_NAME, &held, e, sizeof(e));
    KM_INFO("manual crash: SeShutdownPrivilege %s %s", ok ? "enabled" : "FAILED",
            ok ? "" : e);

    e[0] = 0;
    ok = km_enable_privilege(SE_TCB_NAME, &held, e, sizeof(e));
    KM_INFO("manual crash: SeTcbPrivilege %s %s", ok ? "enabled" : "FAILED",
            ok ? "" : e);

    e[0] = 0;
    status = km_raise_hard_error(0, 0, 0, 0x4B4D5645UL, e, sizeof(e));

    KM_ERROR("manual crash failed: %s", e);
    printf("keepMoving: NtRaiseHardError failed: %s\n", e);
    km_log_close();

    (void)status;
    return KM_EXIT_CRASH_CALL_FAIL;
}
