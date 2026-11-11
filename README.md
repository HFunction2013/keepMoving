# keepMoving

A watchdog that actually makes you move the mouse: if the keyboard and mouse
stay idle for the number of seconds you specify, it raises a bugcheck via
`NtRaiseHardError` with the stop code fixed at **0xABCDEF12**.

Pure user mode, MSVC-built, CLI, `start` returns immediately without blocking
your terminal, logs go to a file instead of flooding the console, and — hard
design constraint — it **changes no system settings** (no screensaver, no
registry, no power policy).

```
keepMoving start 300         # bugcheck after 5 minutes of no mouse/keyboard
keepMoving start 300 --warn  # instead: pop a topmost message box, then disarm
keepMoving query              # how many seconds are left
keepMoving query --watch      # refresh a live countdown line every second
keepMoving stop               # disarm the watchdog
```

> **Warning**: this is a real bugcheck, not a simulation. Unsaved work is
> lost when it fires, Windows writes a dump per your current dump settings
> (defaults to `%SystemRoot%\MEMORY.DMP` when memory allows), and BitLocker
> may trigger a recovery flow. Run `--dry-run` first to confirm the behaviour
> before pointing it at a real machine.

---

## 1. How "no keyboard/mouse activity" is detected

It uses `GetLastInputInfo()`, the user32 API that answers "when was the last
keyboard/mouse input in this session?". It returns a tick count; the program
polls it every 50 ms and subtracts `GetTickCount64()` to get the idle time.

Why this API, mapped to the constraints:

| Requirement | Decision |
| --- | --- |
| Change no system settings | No registry, no screensaver, no power policy — one read-only API |
| No kernel driver | Not needed; the kernel itself maintains the input timestamp |
| No hooks | No hooks installed, no injection, nothing in AV red zones |
| Cover keyboard and mouse | The API counts session-wide input events, not one window's messages |

Known boundaries (don't expect more than it can deliver):

- Covers only the **current session**. Input in another user session on the
  same machine does not count, and vice versa.
- Needs an interactive window station (`WinSta0`). Run it in a non-interactive
  session and `GetLastInputInfo` fails; the program **does not pretend to
  work** — `start` fails outright, the log says why, exit code 5.
- Touch, pen and RDP-injected input all count as input, so "connected via RDP
  but idle" counts as idle.
- Playing a video is not input, so watching a movie without touching the
  mouse will still bugcheck — that is the whole point of the tool.
- **The 49.7-day `GetTickCount` wrap is handled explicitly**: internal timing
  uses `GetTickCount64()` (64-bit, effectively never wraps), and when the 32-bit
  `GetLastInputInfo().dwTime` shows "the current low 32 bits are smaller than
  the last input", the code detects a wrap and reconstructs the idle value
  against the previous window. A machine uptimes of 50 days does not misfire.

If on a real machine this API is genuinely unavailable, the program chooses to
**fail loudly** rather than silently degrade. Fallbacks are discussed in
section 7.

## 2. How the bugcheck is raised

When the third "response option" argument of `ntdll!NtRaiseHardError` is
`OptionShutdownSystem` (enum value 6, zero-based in `winnt.h`), the executive
does not show a dialog — it triggers a bugcheck with the given NTSTATUS:

```c
ULONG_PTR params[4] = { idle_ms, limit_ms, interval_sec, 0x4B4D5645 };  /* "KMVE" */
NtRaiseHardError(0xABCDEF12, 4, 0, params, OptionShutdownSystem, &response);
```

The bugcheck screen then shows:

```
Stop code: 0xABCDEF12
```

The four params show up as `(0x..., 0x..., 0x..., 0x...)` — actual idle ms,
limit ms, configured seconds, tag — so you can tell from a dump that this was
keepMoving, not a real hardware fault.

This path needs privileges: the caller's token must be able to enable
`SeShutdownPrivilege` (some configurations also check `SeTcbPrivilege`). The
program **tries both** and logs every step:

- Success → immediate bugcheck; the last log line is `calling NtRaiseHardError(...)`.
- Failure → the NTSTATUS is recorded, the watchdog stays armed, and it retries
  in 60 seconds. It neither silently fails nor floods the log.

So: **run it elevated** (being in the Administrators group per `whoami` is not
enough — you need a truly elevated token). If the log shows
`STATUS_PRIVILEGE_NOT_HELD`, run as SYSTEM (a service or `psexec -s`).
`keepMoving diag` tells you up front whether these privileges are available.

## 3. Usage

```
keepMoving start <seconds> [--dry-run] [--warn] [--quiet]
keepMoving stop [--force]
keepMoving query [--json] [--clear] [--watch]
keepMoving run <seconds> [--dry-run] [--warn]
keepMoving diag
keepMoving crash --confirm
keepMoving version | help
```

- `start`: spawns a detached background worker (`DETACHED_PROCESS`), confirms
  it is up and the state file is written, then returns immediately. Closing
  the terminal or dropping the SSH session does not affect it.
  Intervals accept `30`, `90s`, `5m`, `1h`; range 1 second to 7 days.
- `stop`: signals the worker via a named event and waits up to 10 s; use
  `--force` if it refuses to leave.
- `query`: prints `state / pid / interval / idle / remaining / uptime`.
  `--json` is for scripts. `--watch` refreshes one line in place every second
  (Ctrl+C to quit; note that typing `query` itself counts as keyboard input and
  resets the idle timer, so use `--watch` for long observation).
  `--clear` wipes the previous crash marker.
- `run`: foreground mode, log mirrored to the console, Ctrl+C to stop. Use it
  to tune thresholds, not to stand guard.
- `diag`: session id, window station, whether `GetLastInputInfo` works, whether
  `SeShutdown`/`SeTcb` can be held, where the data directory is. **Run this first.**
- `crash --confirm`: triggers a bugcheck immediately to verify the whole chain.
  The `--confirm` guard is deliberate.

### Examples

```
> keepMoving diag
keepMoving diagnostics
version        : 1.0.0
session id     : 1
window station : WinSta0
interactive    : yes
GetLastInputInfo: ok (last input 3210 ms ago)
SeShutdownPriv : held
SeTcbPriv     : NOT held - privilege SeTcbPrivilege is not held by this token
...

> keepMoving start 5m
keepMoving: watchdog started (pid=18424, interval=300 s)

> keepMoving query
state     : RUNNING
pid       : 18424
interval  : 300 s
idle      : 42.5 s
remaining : 257.5 s
started   : uptime 00:03:11
mechanism : GetLastInputInfo polling every 50 ms
bugcheck  : 0xABCDEF12 (NtRaiseHardError, OptionShutdownSystem)
log       : C:\Users\me\AppData\Local\keepMoving\keepMoving.log
```

### Exit codes

| Code | Meaning |
| --- | --- |
| 0 | OK |
| 1 | Bad arguments |
| 2 | A worker is already running |
| 3 | `stop` called with no running worker |
| 4 | Internal error (spawn failed, state file unwritable, etc.) |
| 5 | Cannot observe input (non-interactive window station); refusing to arm |
| 7 | `NtRaiseHardError` returned without bugchecking (usually missing privilege) |

## 4. Logging

`%LOCALAPPDATA%\keepMoving\keepMoving.log`, rolling over to `keepMoving.log.1`
above 2 MiB. Every line carries a millisecond timestamp, pid, tid and level, and
every write is `fflush`ed — the last line before the bugcheck must hit disk, or
the log loses its only evidence value.

A full lifecycle looks roughly like this:

```
2026-11-10 22:40:01.123 pid=18424 tid=19012 INFO  keepMoving worker starting, version 1.0.0
2026-11-10 22:40:01.126 pid=18424 tid=19012 INFO  input source: GetLastInputInfo (user mode, no hooks, no driver)
2026-11-10 22:40:01.130 pid=18424 tid=19012 INFO  watchdog armed, idle limit 300000 ms, poll interval 50 ms
2026-11-10 22:40:31.132 pid=18424 tid=19012 INFO  heartbeat: idle 12.0 s, remaining 288.0 s, limit 300 s
2026-11-10 22:44:50.140 pid=18424 tid=19012 WARN  10 s left: idle 290.0 s of 300 s, move the mouse now
2026-11-10 22:45:00.145 pid=18424 tid=19012 WARN  IDLE LIMIT REACHED: no keyboard/mouse input for 300.1 s (limit 300 s)
2026-11-10 22:45:00.150 pid=18424 tid=19012 INFO  SeShutdownPrivilege: enabled
2026-11-10 22:45:00.151 pid=18424 tid=19012 INFO  calling NtRaiseHardError(status=0xABCDEF12, ...)
```

A `%LOCALAPPDATA%\keepMoving\last-crash.txt` is also written; after a reboot
`query` surfaces it, so you do not have to dig through Event Viewer to find out
why the last crash happened.

`%LOCALAPPDATA%\keepMoving\state.ini` is the worker's identity card (pid,
interval, process creation time, dry-run / warn flags). Process creation time
is cross-checked so a reused pid does not make us believe the old worker is
still alive.

## 5. Why not a kernel driver, and not a screensaver

The brief offered two alternatives; neither is taken, for these reasons.

**Kernel driver**: to observe input, a driver would have to layer-filter
`kbdclass`/`mouclass`, or register `PsSetLoadImageNotify` and poll
`\Device\KeyboardClass0`. The price is signing (test-signing means
`bcdedit /set testsigning on` — itself a boot-config change), IRQL/PnP/power
handling, and unload races — one bug and you get an *unwanted* bugcheck. And
the only capability it buys ("detect input on a machine with no interactive
session") is meaningless here: nobody is sitting at the box, so there is
nothing to time out.

**Screensaver path**: renaming the exe to `.scr` and pointing `SCRNSAVE.EXE`
at it does get launched when the system thinks it is idle. But it changes
system settings (directly against the constraint), only gives you the system's
own idle judgement (granularity 1 minute, lives in the power plan), and the
screensaver exits on any keypress — its lifetime is out of your control and
something would have to relaunch it. As a trigger it is less clean than one
`start`.

So the main path is `GetLastInputInfo` polling: 0 drivers, 0 registry writes,
0 system-setting changes.

## 6. Building

Artifacts are built by GitHub Actions on `windows-latest` with MSVC; nothing is
compiled locally:

```powershell
# equivalent of what the runner actually runs
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # MSVC env already loaded via ilammy/msvc-dev-cmd
cmake --build build --config Release
```

- Matrix: `x64` + `x86`, producing `keepMoving-x64.exe` / `keepMoving-x86.exe`.
- CRT statically linked (`/MT`); target machines need no VC++ runtime.
- Only `user32` and `advapi32`; no third-party libraries.
- CI runs a `--dry-run` smoke test: `version` / `diag` / `start 120 --dry-run`
  / `query` / a duplicate `start` (must be rejected) / `stop` / `query` again,
  plus a `start 120 --warn` that is stopped before the dialog fires.
  **CI never actually bugchecks**, or the runner would die on the spot.

## 7. If `GetLastInputInfo` does not work in your environment

Run `keepMoving diag` and look at the `window station` and `GetLastInputInfo`
lines. Most "it doesn't work" cases come from running in session 0 or in a
container without an interactive window station. Options:

1. **Run it in the user's desktop session** (easiest): an interactive cmd /
   PowerShell, elevated.
2. **Add a raw-input fallback**: create a message-only window,
   `RegisterRawInputDevices` with `RIDEV_INPUTSINK`, and use the `WM_INPUT`
   timestamp as the "last input" time, taking the max with
   `GetLastInputInfo`. Still pure user mode, still no system changes — but it
   only works in a process with a window station, so it cannot rescue case 1.
   Not built into this version; the hook is left in `input.c`.
3. **Kernel driver**: only worth it if you truly must detect real hardware
   input in a non-interactive session (signing, WDK, bugcheck risk). See
   section 5.
4. **Screensaver**: it changes system settings; not recommended.

## 8. Layout

```
keepMoving/
├─ CMakeLists.txt
├─ README.md
├─ .github/workflows/build.yml     # windows-latest + MSVC + Ninja matrix build
├─ scripts/smoke-test.ps1          # dry-run-only smoke test
└─ src/
   ├─ km.h          # shared declarations, constants, exit codes
   ├─ main.c        # wmain: command parsing and dispatch
   ├─ worker.c      # watchdog loop, start/stop/query/diag/crash
   ├─ input.c       # GetLastInputInfo wrapper
   ├─ bsod.c        # privilege enablement + NtRaiseHardError
   ├─ state.c       # state.ini and last-crash.txt
   ├─ log.c         # timestamped/pid/tid file logging
   └─ util.c        # paths, process liveness, error formatting
```

## 9. Risk, in one line

This program's job is to stop the machine when you stop touching it. It does
exactly that, and by design pollutes nothing: it writes three files under its
own directory, calls one read-only API, and changes no system setting. But a
bugcheck does destroy unsaved work — run `--dry-run` first, then `start 5m`.
