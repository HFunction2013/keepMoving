/* Command line entry point. */

#include "km.h"
#include <wctype.h>

static void print_usage(void)
{
    printf(
        "keepMoving %s - bugchecks the machine if the keyboard and mouse stay\n"
        "               untouched for the given number of seconds.\n"
        "\n"
        "usage:\n"
        "  keepMoving start <seconds> [--dry-run] [--quiet]\n"
        "  keepMoving stop [--force]\n"
        "  keepMoving query [--json] [--clear]\n"
        "  keepMoving run <seconds> [--dry-run]\n"
        "  keepMoving diag\n"
        "  keepMoving crash --confirm\n"
        "  keepMoving version\n"
        "\n"
        "commands:\n"
        "  start   arm the watchdog in a detached background worker, then return.\n"
        "  stop    disarm it and wait for the worker to exit.\n"
        "  query   show whether it is armed, how idle the machine is and how much\n"
        "          time is left before the bugcheck.\n"
        "  run     same as start but stays in the foreground, log mirrored on the\n"
        "          console (Ctrl+C disarms).\n"
        "  diag    print environment diagnostics: session, window station, the\n"
        "          availability of GetLastInputInfo and of the shutdown privileges.\n"
        "  crash   raise the bugcheck immediately, for verification. Needs\n"
        "          --confirm, because unsaved work will be lost.\n"
        "\n"
        "seconds accept a plain number or a suffix: 30, 90s, 5m, 1h.\n"
        "bugcheck code: 0x%08lX, raised through NtRaiseHardError.\n"
        "log file: %%LOCALAPPDATA%%\\keepMoving\\keepMoving.log\n",
        KM_VERSION_STR, (unsigned long)KM_BUGCHECK_CODE);
}

static BOOL parse_interval(const wchar_t *text, DWORD *out)
{
    wchar_t           *end = NULL;
    unsigned long long v;
    unsigned long long mul = 1;

    if (text == NULL || text[0] == 0 || out == NULL)
        return FALSE;

    v = _wcstoui64(text, &end, 10);
    if (end == text)
        return FALSE;

    if (*end != 0) {
        wchar_t unit = (wchar_t)towlower((wint_t)*end);

        if (end[1] != 0)
            return FALSE;

        switch (unit) {
        case L's': mul = 1;    break;
        case L'm': mul = 60;   break;
        case L'h': mul = 3600; break;
        default:   return FALSE;
        }
    }

    v *= mul;

    if (v < (unsigned long long)KM_MIN_INTERVAL_SEC ||
        v > (unsigned long long)KM_MAX_INTERVAL_SEC)
        return FALSE;

    *out = (DWORD)v;
    return TRUE;
}

static BOOL arg_is(const wchar_t *arg, const wchar_t *name)
{
    return (wcscmp(arg, name) == 0);
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 2) {
        print_usage();
        return KM_EXIT_USAGE;
    }

    if (arg_is(argv[1], L"help") || arg_is(argv[1], L"-h") ||
        arg_is(argv[1], L"--help") || arg_is(argv[1], L"/?")) {
        print_usage();
        return KM_EXIT_OK;
    }

    if (arg_is(argv[1], L"version") || arg_is(argv[1], L"-v") ||
        arg_is(argv[1], L"--version")) {
        printf("keepMoving %s\n", KM_VERSION_STR);
        return KM_EXIT_OK;
    }

    if (arg_is(argv[1], L"start")) {
        DWORD interval = 0;
        int   dry_run = 0;
        int   quiet = 0;
        int   i;

        if (argc < 3 || !parse_interval(argv[2], &interval)) {
            printf("keepMoving: start needs a valid interval in seconds "
                   "(%lu..%lu).\n",
                   (unsigned long)KM_MIN_INTERVAL_SEC,
                   (unsigned long)KM_MAX_INTERVAL_SEC);
            return KM_EXIT_USAGE;
        }

        for (i = 3; i < argc; ++i) {
            if (arg_is(argv[i], L"--dry-run"))
                dry_run = 1;
            else if (arg_is(argv[i], L"--quiet"))
                quiet = 1;
            else {
                printf("keepMoving: unknown option for start: %ls\n", argv[i]);
                return KM_EXIT_USAGE;
            }
        }

        return km_cmd_start(interval, dry_run, quiet);
    }

    if (arg_is(argv[1], L"stop")) {
        int force = 0;
        int i;

        for (i = 2; i < argc; ++i) {
            if (arg_is(argv[i], L"--force"))
                force = 1;
            else {
                printf("keepMoving: unknown option for stop: %ls\n", argv[i]);
                return KM_EXIT_USAGE;
            }
        }

        return km_cmd_stop(force);
    }

    if (arg_is(argv[1], L"query") || arg_is(argv[1], L"status")) {
        int json = 0;
        int clear = 0;
        int i;

        for (i = 2; i < argc; ++i) {
            if (arg_is(argv[i], L"--json"))
                json = 1;
            else if (arg_is(argv[i], L"--clear"))
                clear = 1;
            else {
                printf("keepMoving: unknown option for query: %ls\n", argv[i]);
                return KM_EXIT_USAGE;
            }
        }

        return km_cmd_query(json, clear);
    }

    if (arg_is(argv[1], L"run")) {
        DWORD interval = 0;
        int   dry_run = 0;
        int   i;

        if (argc < 3 || !parse_interval(argv[2], &interval)) {
            printf("keepMoving: run needs a valid interval in seconds.\n");
            return KM_EXIT_USAGE;
        }

        for (i = 3; i < argc; ++i) {
            if (arg_is(argv[i], L"--dry-run"))
                dry_run = 1;
            else {
                printf("keepMoving: unknown option for run: %ls\n", argv[i]);
                return KM_EXIT_USAGE;
            }
        }

        return km_cmd_run(interval, dry_run);
    }

    if (arg_is(argv[1], L"diag")) {
        return km_cmd_diag();
    }

    if (arg_is(argv[1], L"crash")) {
        int i;
        int confirmed = 0;

        for (i = 2; i < argc; ++i) {
            if (arg_is(argv[i], L"--confirm"))
                confirmed = 1;
            else {
                printf("keepMoving: unknown option for crash: %ls\n", argv[i]);
                return KM_EXIT_USAGE;
            }
        }

        if (!confirmed) {
            printf("keepMoving: 'crash' stops the machine immediately and any "
                   "unsaved work is lost.\n"
                   "Run 'keepMoving crash --confirm' if that is what you want.\n");
            return KM_EXIT_USAGE;
        }

        return km_cmd_crash();
    }

    /*
     * Internal: the detached worker started by 'keepMoving start'.  Not meant
     * to be typed by hand.
     */
    if (arg_is(argv[1], L"__worker")) {
        DWORD interval = 0;
        int   dry_run = 0;
        int   i;

        for (i = 2; i < argc; ++i) {
            if (arg_is(argv[i], L"--interval") && i + 1 < argc) {
                if (!parse_interval(argv[++i], &interval))
                    return KM_EXIT_USAGE;
            } else if (arg_is(argv[i], L"--dry-run")) {
                dry_run = 1;
            } else {
                return KM_EXIT_USAGE;
            }
        }

        if (interval == 0)
            return KM_EXIT_USAGE;

        return km_worker_core(interval, dry_run, FALSE);
    }

    printf("keepMoving: unknown command: %ls\n\n", argv[1]);
    print_usage();
    return KM_EXIT_USAGE;
}
