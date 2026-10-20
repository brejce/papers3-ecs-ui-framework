# 真终端（设备上的 shell）—— 设计稿

> 状态：**设计待拍板**（2026-10-20）。实现按 §11 的里程碑走，先把 §12 的 4 个问题定下来。
> 相关：`docs/architecture/READER_API_SURFACE.md`（"能力在固件、决策在 .so"的分层原则）、
> `docs/guide/APP_AUTHORING.md`（.so 入口/白名单/上传）、`docs/design/UI_DESIGN.md`（终端风 v3）。

---

## 1. 目标 / 非目标

**目标**：设备上有一个**真的能执行命令**的终端 —— 敲一行，命令在这台设备上真正跑（列目录、看文件、
查内存、看任务、改配置、重启），不是现在那块装饰屏。

**非目标（明确不做，别指望）**：

| 不做 | 为什么 |
|---|---|
| 跑任意 ELF 可执行文件 / 装软件包 | 没有 MMU、没有文件系统权限模型、没有动态链接器可用 |
| 真 POSIX shell（管道 `\|`、重定向 `>`、`&&`、子 shell、作业控制） | 没有 fork/exec；`ls \| grep` 这类要自己实现，v1 不做 |
| `sudo` / 多用户 / 权限 | 单用户裸机 |
| 常驻后台的 daemon 管理 | 只有 FreeRTOS 任务，没有 init/service 概念（能做 `ps`，不能 kill 别人） |
| `python` / `lua` / `js` 解释器 | 内存和体积都不是这次的目标（`research/WASM_FEASIBILITY.md` 另说） |

**能被"操作"的真实边界**：文件系统（`/sdcard`）、固件配置（NVS）、网络、电源/重启、任务与内存的**只读**观察。

---

## 2. 架构：能力在固件，界面在 .so

现在 `apps/terminal` 是个**装饰屏**（`world.execute(me);` 那套：CPU 负载条 / 内存面板 / 假日志 / EXECUTE）。
真终端要动真东西，而 `.so` 里**只有** `so_api.h` 白名单那几十个函数：没有 `fopen/opendir/stat`，
也没有 `esp_*`。所以：

```
┌──────────────────────────── .so（apps/terminal）────────────────────────────┐
│  UI：状态栏 / 滚动输出区 / 输入行 / 提示行                                  │
│  ・行缓冲、滚动、折行、自动滚到底                                           │
│  ・输入：so_ime_open() 拉固件输入法，拿回一整行字符串                       │
│  ・本地命令：clear / exit / help（纯 UI 行为）                              │
└───────────────────────────────────┬─────────────────────────────────────────┘
                                    │  一次一条：int so_sh_exec(line, out, cap)
┌───────────────────────────────────▼─────────────────────────────────────────┐
│  固件 main/platform/sh.h  namespace m5ui::sh                                │
│  ・虚拟 cwd + 路径夹取（只能在 /sdcard 里）                                 │
│  ・命令分派（表驱动：名字 / 最小参数 / 权限标记 / 帮助一行）                │
│  ・有完全的文件系统与 IDF 能力：dirent/stat/remove/rename/mkdirfs/          │
│    esp_vfs_fat_info / uxTaskGetSystemState / esp_timer / esp_restart        │
│  ・输出按 cap 截断（UTF-8 安全），末尾补 "(truncated N bytes)"              │
└─────────────────────────────────────────────────────────────────────────────┘
```

**为什么不让 .so 自己实现**：`.so` 里 `malloc` 只能拿到内部 RAM（设备内部余量常年在 10~20KB），
而固件侧能用 PSRAM、能读 `stat`、能 `esp_restart()`。这不是选择，是唯一可行解。

**为什么一次一调用（不带流式）**：输出是几十行文本，同步一次返回最简单；
`.so` 不需要处理"边跑边收"的回调生命周期（回调里不能重入推屏）。

---

## 3. 新增 API（固件 → .so）

```c
// so_api.h，MINOR 13 → 14
// 执行一条命令行，把文本输出写进 out（以 '\0' 结尾，行间 '\n'，不带 ANSI 转义）。
//   line  : 一整行（不含换行符），最长 200 字节，超了按 200 截断
//   out   : 输出缓冲；cap 建议 3072~4096
//   返回  : 写出的字节数（>=0）；负值 = 错误
//            -1 = 固件不支持（老固件）→ app 显示"固件太旧，请刷机"
//            -2 = 参数非法
//   *code : 命令退出码（0 = 成功，1 = 命令错，2 = 用法错，3 = 权限/路径越界，4 = 执行失败）
//   *flags: bit0 = 命令已请求重启（so 自己在推完屏后调 so_exit() 前不要做别的）
int so_sh_exec(const char* line, char* out, int cap, int* code, int* flags);
```

- `help` / `clear` / `exit` / 历史 这类**纯界面行为**在 `.so` 里处理，不下发；
- 命令执行是**同步**的，`reboot` 也走"先返回文本，app 推完屏再 `esp_restart()`"的路径（`flags` bit0），
  避免屏幕停在半句话上；
- 版本不匹配的处理沿用现有握手机制：`terminal.so` 的 `so_app_api_version()` 用新的 MINOR，
  老固件会**拒绝加载**并在 launcher 显示原因（比跑起来崩成白屏强）。

---

## 4. 命令集 v1

约定：`<必选>` `[可选]`；路径可相对（相对虚拟 cwd）或绝对（`/sdcard/...` 或 `/apps/...`，见 §5）。

### 4.1 浏览

| 命令 | 作用 | 输出 / 备注 |
|---|---|---|
| `pwd` | 当前目录 | `/sdcard/apps` |
| `cd [dir]` | 换目录（无参 = 回 `/sdcard`） | 失败：`cd: no such directory: x` |
| `ls [dir]` | 列表 | 每行 `d  2026-10-20 12:31   4096  apps`；先目录后文件，按名字排序；`-l` 省略（默认就是长格式） |
| `cat <file>` | 看文本 | 最多 4KB，超了截断并提示；二进制（含 NUL）提示 `binary file` |
| `head [-n N] <file>` | 前 N 行（默认 10） | |
| `tail [-n N] <file>` | 后 N 行（默认 10） | 从文件尾反向读 4KB 再切行 |
| `wc <file>` | 行数 / 字节 | `12 lines, 3456 bytes` |
| `df` | SD 容量 | `942MB total, 812MB free (/sdcard)` |

### 4.2 文件操作（写）

| 命令 | 作用 |
|---|---|
| `mkdir <dir>` | 建目录 |
| `rm <file>` | 删文件 |
| `rm -r <dir>` | 递归删目录（**必须**带 `-r`；`rm` 不带 flag 遇到目录就拒绝） |
| `mv <src> <dst>` | 改名 / 移动 |
| `cp <src> <dst>` | 复制（≤8MB；>8MB 直接拒绝并说明） |
| `echo <text> > <file>` 或 `echo <text> >> <file>` | **v1 支持最简单的写**：`write <file> <text>` 更直白（见下），`>` 形式放 v2 |
| `write <file> <text...>` | 覆盖写一行文本（调试用，比如写 cfg） |
| `append <file> <text...>` | 追加一行 |

> 写操作一律限制在 `/sdcard` 下，且**不允许**写到 `/sdcard/apps/*.so`（避免一手把自己删了）。

### 4.3 系统观察

| 命令 | 作用 |
|---|---|
| `free` / `mem` | 内部 SRAM / PSRAM 的总量、剩余、最大块 |
| `ps` / `tasks` | 任务表：名字、状态、优先级、栈余量（`uxTaskGetSystemState`，最多 24 条） |
| `cpu` | CPU 占用（复用 `so_cpu_load` 的采样） |
| `uptime` | `up 3h 12m 05s` |
| `date` | `2026-10-20 12:31:07 CST (synced)` / `(RTC 未校准)` |
| `bat` | `battery 87% · charging` |
| `ver` / `uname` | 固件名/版本/编译时间、芯片型号、API 版本 |
| `log [n]` | 末尾 n 行 `/sdcard/apps/reader.log`（默认 20） |

### 4.4 网络 / 配置 / 电源

| 命令 | 作用 |
|---|---|
| `wifi` | 状态 + SSID + IP |
| `wifi scan` | 扫 8 个热点（SSID + RSSI） |
| `wifi on` / `wifi off` | **开关设备热点**（AP） |
| `wifi conn <ssid> [pass]` | 连热点（等价设置页那套） |
| `ip` / `ifconfig` | `sta 192.168.43.14/24 · ap off · web on` |
| `cfg` | 打印全部键值（一行一个 `key = value`） |
| `cfg get <key>` | 读一个键 |
| `cfg set <key> <value>` | 改内存 |
| `cfg save` | 落盘 NVS（**必须** `save`，和设置页的 COMMIT 一致） |
| `reboot` | 重启设备（返回提示文本，app 推屏后真重启） |
| `reboot -s` | 进深睡？（**v1 不做**，深睡唤醒要靠硬件按键，语义没设计好） |

### 4.5 界面本地命令（不下发固件）

| 命令 | 作用 |
|---|---|
| `clear` | 清屏 |
| `help` | 打印命令表（分 4 组，一行一条） |
| `exit` / `quit` | 回 launcher |
| `history` | 最近 20 条（内存里，掉电不存） |

**v2 候选**（这次不做，写在这里免得以后重复讨论）：管道/重定向、`grep`、`hexdump`、
自绘 ASCII 键盘、USB 串口桥（PC 上敲 `idf.py monitor` 直接操作）、`launch <app>`（需要新增宿主 `so_launch`）。

---

## 5. 路径与安全

- **虚拟 cwd** 存在固件侧（`static char g_cwd[160]`），初值 `/sdcard`；`cd` 只改它。重启自然复位。
- **解析顺序**：以 `/` 开头 → 绝对；否则 `cwd + "/" + arg`。然后规范化：去掉 `//`、`.`、处理 `..`
  （但**不许**越过 `/sdcard`）→ 越界直接 `path outside /sdcard`。
- **简写**：`/apps`、`/books`、`/fonts`、`/sounds`、`/shots` 自动展开成 `/sdcard/...`（敲起来短）。
- **写保护**：`/sdcard/apps/*.so` 只读（防止把正在用的 app 删了）；`/sdcard/apps/*.idx`、`.cfg` 可写。
- **危险命令**：`rm -r` 与 `reboot` 直接执行，**不做二次确认**——手上有键盘就意味着手上有责任；
  但 `rm -r /sdcard`（根）和 `rm -r` 无参数会被拒绝（`refusing to remove root`）。

---

## 6. 输出策略

- 单次输出上限 **3KB**（cap 3072）。超了按 UTF-8 字符边界截断，末尾补
  `\n… (truncated, 12345 more bytes)\n`——**绝不**切断半个汉字（`u8_len` 那套逻辑照抄阅读器）。
- `cat`/`tail` 大文件：只读窗口（`cat` 前 4KB、`tail` 末 4KB）。
- 输出里**不含** ANSI 颜色；"颜色"靠 app 侧按行首前缀区分（`err:` 红/黑、`warn:` 灰），
  固件只返回纯文本 + `code`。
- 行宽由 app 折行（**固件不折行**：它不知道屏幕宽度），折行按字符算，中文占 2 列。

---

## 7. 终端 app 版面（540×960）

```
 0  ┌ < EXIT                 12:31        BAT 87% ┐  60，底 1px 线
64  │ m5ui terminal · v1                            │
    │ 输入 help 看命令，点最下面那行开始敲           │
    │ > ls /apps                                     │
    │ apps/  reader.so  61888                        │
    │ ✓ 3 entries                                    │
    │ …（滚动区：22 行 × 26px = 572px，自动滚到底）  │
864 ├────────────────────────────────────────────────┤ 1px 点线
872 │ > ls /apps▌                                     │  输入行 44px（点它 = 拉输入法）
916 │ // 上一条 ↑    // clear    // help              │  提示行 28px（可点的快捷键）
```

- 字号：正文 **20px**（行高 26），每屏 22 行；宽 492px ≈ 24 个汉字 / 48 个 ASCII。
- 颜色：命令回显 `> ` 用灰（`G99`→改 `G55`），输出纯黑，错误行黑底反白（只给"命令失败"一条）。
- 刷新：进屏 `QUALITY` 整屏；之后每条命令 = **一次** TEXT 推屏（只推滚动区 + 输入行）；
  输入行光标用 FASTEST 只翻那一小块。
- 滚动：每次执行后自动到底；输出区**不可拖动**（v1），靠 `clear` 与分页（v2 `more`）解决长输出。

---

## 8. 输入

**v1：借固件输入法**（`so_ime_open("", done, cancel)`）——按一下输入行，弹出系统键盘（QWERTY + 中文），
敲完回车拿回**整行**字符串，直接执行。

- 好处：零工作量拿到完整键盘（含 `/ _ - .` 这些 shell 必需符号），还能打中文文件名；
- 代价：不是"敲一个字符立刻回显"的真终端手感；每条命令多一次弹层。
- 提示行给三个可点快捷键：`[ 上一条 ]`（把上一条命令塞回输入行再编辑）、`[ clear ]`、`[ help ]`。

**v2：自绘 ASCII 键盘**（若嫌输入法弹层烦）——固定 4×10 键位（字母/数字/符号三页），
逐字回显，行内可退格。工作量约等于现在的输入法键盘页，但少了中文。

---

## 9. 错误处理

| 情况 | 表现 |
|---|---|
| 老固件没有 `so_sh_exec` | 进 app 就一行黑底反白：`固件太旧：需要 API v1.14，请刷机`，其他不画 |
| 命令不存在 | `sh: unknown command: xyz`，退出码 1 |
| 参数不足 | `usage: cp <src> <dst>`，退出码 2 |
| 路径越界 / 写保护 | `sh: path outside /sdcard` / `sh: read-only: apps/reader.so`，退出码 3 |
| 文件不存在 | `cat: no such file: x.txt`，退出码 4 |
| 输出被截断 | 末尾 `… (truncated, N more bytes)`，退出码仍 0 |
| 命令超时（无此机制） | v1 不做超时；所有命令都应 < 200ms（`wifi scan` 例外，约 2–3s，期间屏幕不动） |

---

## 10. 内存与预算

| 项 | 预算 | 说明 |
|---|---|---|
| 固件侧 shell 静态量 | ≈ 2KB（cwd 160 + 输出拼装 3KB 走调用方缓冲） | 不新增常驻任务 |
| `.so` 侧滚动缓冲 | **8KB PSRAM**（`so_psram_alloc`）= 128 行 × 64B 的环 | 不占内部 RAM |
| `.so` 侧代码/常量 | ≈ 12–16KB | 现在 terminal.so 有多大，重写后应持平 |

> 内部 RAM 现在只剩 10–20KB，**任何新增都要走 PSRAM**；这是本项目最容易翻车的地方（见 `architecture/ECS_OPTIMIZATION.md`）。

---

## 11. 里程碑

| 段 | 内容 | 验收 |
|---|---|---|
| **M1** | 固件 `sh.h`（cwd/路径/分派/Out）+ `so_sh_exec` 导出 + API MINOR 14；命令：`help pwd cd ls cat head tail wc df echo write append mkdir rm mv cp` | 串口里 `so_sh_exec("ls /apps")` 输出 3 行；`cat` 一本 txt 前 10 行正确 |
| **M2** | 系统观察命令：`free ps cpu uptime date bat ver log cfg reboot` | `ps` 打 14 个任务；`cfg` 与设置页读到的一致；`reboot` 真重启 |
| **M3** | `.so` 重写成终端 UI（滚动区 + IME 输入 + 快捷键 + 历史）替换装饰屏 | 设备上敲 `ls /apps` → 看到 3 个 .so；`free` 数字对得上 |
| **M4** | 打磨：错误行反白、输出折行中文对齐、`history`、长输出分页 | 逐条点检 §9 的错误表 |

估计：M1+M2 一次编译周期内可完成（都是固件侧纯逻辑），M3 是大头（约等于重写一个 app）。

---

## 12. 要你拍板的 4 件事

1. **原来的装饰终端（world.execute(me)：负载条/内存面板/日志流）怎么处理？**
   ① 整个替换成真终端（推荐，风格不变、只是内容变真）② 保留成"监控页"，加一个 tab 切到终端 ③ 两个 app 并存。
2. **输入方式**：v1 用系统输入法弹层（马上能用，能打中文）先上，还是直接做自绘 ASCII 键盘（手感像终端，多花一天）？
3. **危险命令**：`rm -r <dir>` 和 `reboot` 要不要二次确认（例如 `reboot!` 才执行）？还是直接执行？
4. **要不要顺带做"从电脑敲"**：设备 USB 串口现在只会喷日志。加一个 UART 命令行（PC 上 `idf.py monitor` 就能敲 `ls`）
   是另一条更省事的路径（不用碰屏幕、能复制粘贴），但要小心和日志输出抢串口。做 / 不做？
