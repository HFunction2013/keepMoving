# keepMoving

一个“你必须真的动鼠标”的守夜程序：只要键盘和鼠标在你指定的秒数内一直没有动过，就用
`NtRaiseHardError` 触发一次 bugcheck，停机码固定为 **0xABCDEF12**。

纯用户态、MSVC 编译、CLI 程序、`start` 立即返回不阻塞终端、日志写文件不刷屏，并且
**不修改系统设置**（不碰屏保、不碰注册表、不碰电源策略），这些约束是设计的硬前提。

```
keepMoving start 300         # 5 分钟不动键盘鼠标就蓝屏
keepMoving query             # 现在还有多少秒
keepMoving stop              # 拆掉定时器
```

> **警告**：这是真的蓝屏，不是模拟。触发时未保存的工作会丢失，Windows 会按系统当前的
> 转储设置写 dump（内存足够时默认可能写 `%SystemRoot%\MEMORY.DMP`），如果开了 BitLocker
> 可能弹出恢复流程。先 `--dry-run` 跑通确认行为，再上真机。

---

## 1. 怎么判定“键鼠没有活动”

用 `GetLastInputInfo()`，这是 user32 暴露的、专门回答“本会话最后一次键鼠输入是什么时候”
的 API。它返回一个 tick 值，程序每 500 ms 取一次，用 `GetTickCount()` 相减就得到空闲时长。

选择它的理由，正好对应题目里的约束：

| 需求 | 结论 |
| --- | --- |
| 不改系统设置 | 不用注册表、不用屏保、不用电源策略，只调一个只读 API |
| 不要内核驱动 | 不需要，输入时间戳由系统自己维护 |
| 不要 hook | 不安装任何钩子，不注入进程，不碰安全软件红区 |
| 能覆盖键盘和鼠标 | 该 API 统计的是整个会话的输入事件，不是某一个窗口的消息 |

已知边界（不要指望它做不到的事）：

- 只覆盖**当前会话**。同一台机器上另一个用户会话的输入不算，反过来也一样。
- 必须有交互式窗口站（`WinSta0`）。在无交互的会话里跑，`GetLastInputInfo` 会失败；
  此时程序**不会装作在工作**，`start` 直接失败，日志写明原因，退出码 5。
- 触摸、手写笔、远程桌面注入的输入在系统里同样计为输入，所以“远程连着不动”也算空闲。
- 播放视频不等于有输入，所以看片不碰鼠标也会蓝屏——这正是这个工具存在的意义。
- **`GetTickCount` 的 49.7 天回绕已显式处理**：内部计时全部用 `GetTickCount64()`
  （64 位，理论上不回绕），`GetLastInputInfo().dwTime` 那个 32 位 tick 在检测到
  “当前低 32 位比上次输入还小”时，判定中间发生过一次回绕，按上一个窗口重建空闲值。
  所以机器连续开机 50 天也不会误判。

如果哪天真机环境下这个 API 不可用，本程序的选择是**明确失败**，而不是偷偷降级。备选方案
留到第 7 节讨论。

## 2. 蓝屏是怎么触发的

`ntdll!NtRaiseHardError` 的第三个“响应选项”参数取 `OptionShutdownSystem`（枚举值 6，
`winnt.h` 里零基）时，执行体不会弹框，而是直接按传入的 NTSTATUS 触发一次 bugcheck：

```c
ULONG_PTR params[4] = { idle_ms, limit_ms, interval_sec, 0x4B4D5645 };  /* "KMVE" */
NtRaiseHardError(0xABCDEF12, 4, 0, params, OptionShutdownSystem, &response);
```

于是蓝屏上出现：

```
Stop code: 0xABCDEF12
```

四个参数会显示成 `(0x..., 0x..., 0x..., 0x...)`，内容分别是“实际空闲毫秒、阈值毫秒、
设定秒数、标记”，方便你在 dump 里一眼认出这次是 keepMoving 干的，而不是真的硬件故障。

这条路径需要特权：调用者令牌必须能打开 `SeShutdownPrivilege`（部分配置下还会检查
`SeTcbPrivilege`）。程序会**两个都尝试**，并把每一步的结果写进日志：

- 成功 → 直接蓝屏，日志最后一行是 `calling NtRaiseHardError(...)`。
- 失败 → 记下 `NTSTATUS`，程序保持武装，60 秒后重试一次，不会静默失败，也不会刷屏。

所以：**用管理员身份运行**（`whoami` 显示在管理员组还不够，要真的 elevated）。如果日志里
出现 `STATUS_PRIVILEGE_NOT_HELD`，说明当前令牌拿不到权限，此时用 SYSTEM 身份跑（服务或
`psexec -s`）才行。`keepMoving diag` 会提前告诉你这两个权限到底能不能拿到。

## 3. 用法

```
keepMoving start <秒> [--dry-run] [--quiet]
keepMoving stop [--force]
keepMoving query [--json] [--clear]
keepMoving run <秒> [--dry-run]
keepMoving diag
keepMoving crash --confirm
keepMoving version | help
```

- `start`：拉起一个脱离终端的后台 worker（`DETACHED_PROCESS`），确认它起来了、状态文件
  写好了，然后立刻返回。终端关掉、SSH 断掉都不影响它。
  秒数支持 `30`、`90s`、`5m`、`1h` 这样的写法，范围 1 秒 ~ 7 天。
- `stop`：通过命名事件通知 worker 自己退出，最多等 10 秒；它赖着不走时用 `--force`。
- `query`：`state / pid / interval / idle / remaining / uptime`，`--json` 给脚本用，
  `--clear` 清掉上次蓝屏留下的标记文件。
- `run`：前台模式，日志同时打到控制台，Ctrl+C 即停。调参、验证阈值时用它，别用它守夜。
- `diag`：会话号、窗口站名、`GetLastInputInfo` 是否可用、`SeShutdown`/`SeTcb` 是否可
  取、数据目录在哪。**上线前先跑这个**。
- `crash --confirm`：立刻触发一次蓝屏，用来验证整条链路真的通。故意要 `--confirm`，
  免得手滑。

### 示例

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
mechanism : GetLastInputInfo polling every 500 ms
bugcheck  : 0xABCDEF12 (NtRaiseHardError, OptionShutdownSystem)
log       : C:\Users\me\AppData\Local\keepMoving\keepMoving.log
```

### 退出码

| 码 | 含义 |
| --- | --- |
| 0 | 成功 |
| 1 | 参数错误 |
| 2 | 已经有一个 worker 在跑 |
| 3 | `stop` 时没有在跑的 worker |
| 4 | 内部错误（拉不起 worker、写不了状态文件等） |
| 5 | 拿不到键鼠活动（无交互窗口站），拒绝武装 |
| 7 | `NtRaiseHardError` 返回了，没蓝屏（通常是没权限） |

## 4. 日志

`%LOCALAPPDATA%\keepMoving\keepMoving.log`，超过 2 MiB 滚动成 `keepMoving.log.1`。
每行带毫秒时间戳、pid、tid、级别，每次写入都 `fflush` —— 蓝屏前那一行必须落盘，
否则日志就失去了唯一的证据价值。

一次完整的生命周期大概长这样：

```
2026-11-10 22:40:01.123 pid=18424 tid=19012 INFO  keepMoving worker starting, version 1.0.0
2026-11-10 22:40:01.126 pid=18424 tid=19012 INFO  input source: GetLastInputInfo (user mode, no hooks, no driver)
2026-11-10 22:40:01.130 pid=18424 tid=19012 INFO  watchdog armed, idle limit 300000 ms, poll interval 500 ms
2026-11-10 22:40:31.132 pid=18424 tid=19012 INFO  heartbeat: idle 12.0 s, remaining 288.0 s, limit 300 s
2026-11-10 22:44:50.140 pid=18424 tid=19012 WARN  10 s left: idle 290.0 s of 300 s, move the mouse now
2026-11-10 22:45:00.145 pid=18424 tid=19012 WARN  IDLE LIMIT REACHED: no keyboard/mouse input for 300.1 s (limit 300 s)
2026-11-10 22:45:00.150 pid=18424 tid=19012 INFO  SeShutdownPrivilege: enabled
2026-11-10 22:45:00.151 pid=18424 tid=19012 INFO  calling NtRaiseHardError(status=0xABCDEF12, ...)
```

同时会写一个 `%LOCALAPPDATA%\keepMoving\last-crash.txt`，重启后 `query` 会把它显示出来，
这样“上次为什么蓝屏”不用去翻事件查看器。

`%LOCALAPPDATA%\keepMoving\state.ini` 是 worker 的身份证（pid、间隔、进程创建时间、
是否 dry-run）。用进程创建时间做二次校验，避免 pid 被系统复用后误判“还在跑”。

## 5. 为什么不是内核驱动，也不是屏保

题目给了两条备选路线，这里都没有采用，理由如下。

**内核驱动**：要拿到键鼠活动，驱动得挂 `kbdclass`/`mouclass` 的上层过滤，或者注册
`PsSetLoadImageNotify`、定时读 `\Device\KeyboardClass0` 之类的东西。代价是必须签名
（测试签名要开 `bcdedit /set testsigning on`，等于改系统启动配置）、必须处理
`IRQL`/`PnP` 电源状态/卸载竞态，一个 bug 就是真蓝屏而不是“按需蓝屏”。而它换来的能力
（“在没有交互会话的机器上也能判断输入”）在本工具的使用场景里恰恰没有意义：没人坐在
电脑前的时候，本来就不该期待它判断“你有没有动鼠标”。

**屏保方案**：把 exe 改名成 `.scr`、`SCRNSAVE.EXE` 指向它，确实能在“系统认为空闲”时
被拉起。但它改的是系统设置（这一点直接违背前提），只能拿到系统自己的空闲判定（阈值
在电源计划里，最细 1 分钟），而且屏保被用户一按键盘就退出，进程生命周期不受控，
退出后还得有人再把它拉起来。作为“触发手段”它不如一次 `start` 干净。

所以主路径就是 `GetLastInputInfo` 轮询：0 个驱动、0 个注册表写入、0 个系统设置变更。

## 6. 构建

产物由 GitHub Actions 的 `windows-latest` runner 用 MSVC 编译，本地不编译：

```powershell
# runner 上实际执行的等价命令
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release   # 已先 ilammy/msvc-dev-cmd 载入 MSVC
cmake --build build --config Release
```

- 矩阵：`x64` + `x86`，产物分别是 `keepMoving-x64.exe` / `keepMoving-x86.exe`。
- CRT 静态链接（`/MT`），目标机器不需要 VC++ 运行库。
- 依赖只有 `user32`、`advapi32`，无第三方库。
- CI 里只跑 `--dry-run` 冒烟测试：`version` / `diag` / `start 120 --dry-run` /
  `query` / 重复 `start`（应被拒绝）/ `stop` / 再 `query`，并把日志尾巴打出来。
  **CI 永远不会真的蓝屏**，否则 runner 会当场挂掉。

## 7. 如果 `GetLastInputInfo` 在你的环境里拿不到

先跑 `keepMoving diag` 看 `window station` 和 `GetLastInputInfo` 两行。绝大多数“拿不到”
都是因为程序跑在服务会话（session 0）或者没有交互式窗口站的容器里。此时可选：

1. **换到用户桌面会话里跑**（最省事）：交互式登录后的 cmd/PowerShell，管理员身份。
2. **加一层原始输入兜底**：建一个 message-only 窗口，`RegisterRawInputDevices` 带
   `RIDEV_INPUTSINK`，用 `WM_INPUT` 的时间戳当“最后输入时间”，和 `GetLastInputInfo`
   取较大值。仍然是纯用户态、仍然不改系统设置，但只能在有窗口站的进程里工作，所以它
   救不了第 1 条救不了的场景。本版本没有内置，接口已经留在 `input.c` 里。
3. **内核驱动**：只有“必须在无交互会话里判断真实硬件输入”才值得付这个代价（签名、
   WDK、蓝屏风险），见第 5 节。
4. **屏保**：会改系统设置，不建议。

## 8. 目录结构

```
keepMoving/
├─ CMakeLists.txt
├─ README.md
├─ .github/workflows/build.yml     # windows-latest + MSVC + Ninja 矩阵构建
├─ scripts/smoke-test.ps1          # 只跑 dry-run 的冒烟测试
└─ src/
   ├─ km.h          # 公共声明、常量、退出码
   ├─ main.c        # wmain：命令解析与分发
   ├─ worker.c      # 守夜循环、start/stop/query/diag/crash
   ├─ input.c       # GetLastInputInfo 封装
   ├─ bsod.c        # 权限提升 + NtRaiseHardError
   ├─ state.c       # state.ini 与 last-crash.txt
   ├─ log.c         # 带时间戳、pid、tid 的文件日志
   └─ util.c        # 路径、进程探活、错误格式化
```

## 9. 一句话总结风险

这程序的功能就是“在你不听话的时候把机器打停”。它按你的要求做了，也按你的要求尽量
不污染系统：只写自己目录下的三个文件，只调一个只读 API，不改任何系统设置。但蓝屏本身
一定会带走未保存的数据，第一次请先 `--dry-run`，第二次再 `start 5m`。
