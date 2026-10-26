# papers3 文档地图（总索引）

> **一个文档只负责一个域**，每个域只有**一份权威**。越界的地方引用对方，不抄。
> 找东西的两种用法：**知道要改什么** → §1 权威表；**不知道在哪** → §2 按目录，或 §3 按场景。
> 整理：2026-10-19。

---

## 1. 权威表：一个问题域只有一份权威

| 问题域 | 唯一权威 | 它**不**管（去别处） |
|---|---|---|
| **界面长什么样、怎么画** | `design/UI_DESIGN.md` | 刷新机制 → `display/`；app 怎么建 → `guide/APP_AUTHORING.md` |
| **墨水屏刷新机制 / 残影 / 弹窗** | `display/EPD_TEST_MANUAL.md` | 波形总表 → `display/EPD_LUT_MANUAL.md` |
| **波形（LUT）总表与实验** | `display/EPD_LUT_MANUAL.md` | 刷新纪律（设计侧）→ `design/UI_DESIGN.md` §9 |
| **字体：裁剪 / 分区 / 字符集** | `display/TTF_FONT.md` | 字号档位 → `design/UI_DESIGN.md` §5 |
| **环境 / 构建 / 烧写 / 目录 / 硬约束** | `guide/DEVELOPER.md` | 设计译法 → `design/UI_DESIGN.md` §2–4 |
| **`.so` app：建 / 编 / 传 / 验** | `guide/APP_AUTHORING.md` | 加载器符号解析 → `architecture/ELF_PLUGIN_ARCHITECTURE.md` |
| **ECS 内存额度（加实体前算账）** | `architecture/ECS_OPTIMIZATION.md` | 心智模型 → `guide/DEVELOPER.md` §4 |
| **插件加载 / 符号遮蔽 / 模块依赖** | `architecture/ELF_PLUGIN_ARCHITECTURE.md` | 怎么建 app → `guide/APP_AUTHORING.md` |
| **阅读器依赖的固件 API 面** | `architecture/READER_API_SURFACE.md` | 切块规则 → `reader/READER_TXT_RULES.md` |
| **双核分工与实测负载** | `architecture/DUAL_CORE_SPLIT.md` | — |
| **TXT 切块 / 分卷分章 / 目录** | `reader/READER_TXT_RULES.md` | 研究数据 → `research/TXT_CHAPTER_*.md` |

---

## 2. 按目录找

### `guide/` — 上手与流程

| 文档 | 状态 | 管什么 |
|---|---|---|
| `guide/DEVELOPER.md` | ✅ 活跃 | **开发总册**：环境/构建/烧写/分区表/目录归属/ECS 心智模型/硬约束（踩过的坑）+ ECS 架构附录 |
| `guide/APP_AUTHORING.md` | ✅ 活跃 | **`.so` 应用开发手册**：目录/入口符号/API 版本握手/元信息/编译参数与白名单闸门/上传复位验证/触摸与推屏约定 |

### `design/` — 界面设计（改界面只看这里）

| 文档 | 状态 | 管什么 |
|---|---|---|
| `design/UI_DESIGN.md` | ✅ **唯一权威** | **设计规则 + UI 风格**：风格判定 / 画布 / 色板与灰阶 / 设计原则 / 状态语义 / 字号 / 通用部件 / 每页版面 / 刷新语言 / 文案规范 / 禁止清单 + 附录 A 输入法全文 + 附录 B 应用图标元信息 + 附录 C 触摸反馈音 |
| `design/world-execute-brief.md` | 🗄 归档（来稿） | 最初的设计来稿（《world.execute(me);》意境，灵感/基调来源） |
| `design/terminal-style/*.html` | 🗄 参考稿 | 网页版设计稿（可勾选「显示 push 矩形」）。**旧画法已过时，以 `UI_DESIGN.md` 为准** |

### `display/` — 刷新 / 波形 / 字体

| 文档 | 状态 | 管什么 |
|---|---|---|
| `display/EPD_TEST_MANUAL.md` | ✅ 活跃 | 刷新实测手册 + **机制速览 §〇**（原 `EPD_DRAWING_NOTES.md` 并入）：`_range_mod` 会累积 / 逻辑坐标 ≠ 面板坐标 / 档位双重作用 / 擦除段 |
| `display/EPD_LUT_MANUAL.md` | ✅ 活跃 | 波形（LUT）手册：**持续更新**的波形总表 / 实验台用法 / 动画分档 |
| `display/TTF_FONT.md` | ✅ 活跃 | 字体：裁剪 / 分区 / 字符集审计（鸿蒙 Sans SC 子集） |

### `architecture/` — 架构与额度

| 文档 | 状态 | 管什么 |
|---|---|---|
| `architecture/ECS_OPTIMIZATION.md` | ✅ 活跃 | 内部 RAM 额度实测（184B/实体，flex 四件套 35%），加实体前算额度 |
| `architecture/ELF_PLUGIN_ARCHITECTURE.md` | ✅ 活跃 | 原生 `.so` 插件架构（符号解析链、宿主遮蔽、模块间依赖） |
| `architecture/DUAL_CORE_SPLIT.md` | ✅ 活跃 | 双核分工（**已落地**）：UI 独占 core1，WiFi/lwIP/httpd/EPD 推屏留 core0 + 实测负载 |
| `architecture/READER_API_SURFACE.md` | ✅ 活跃 | 阅读器依赖的固件 API 面清单（决策在 `.so`、能力在固件的分层） |

### `reader/` — 阅读器规则

| 文档 | 状态 | 管什么 |
|---|---|---|
| `reader/READER_TXT_RULES.md` | ✅ 活跃 | TXT 切块 / 分卷分章 / 目录规则：**照做就能实现**（研究数据在 `research/`） |

### `terminal/` — 设备上的真终端

| 文档 | 状态 | 管什么 |
|---|---|---|
| `terminal/TERMINAL_SHELL.md` | 🖊 设计待拍板 | 真 shell 的分层（能力在固件 `papers3::sh`、界面在 `.so`）/ `so_sh_exec` 契约 / 命令集 v1 / 路径与安全 / 版面 / 里程碑 |

### `logs/` — 工作日志（只记"这次发生了什么"）

| 文档 | 状态 | 管什么 |
|---|---|---|
| `logs/2026-10-22.md` | 📝 会话记录 | **全项目代码审核 + 15 项修复**（输入法容量溢出踩结构体、`so_end_write` 负宽、sh 跨核互斥、启动器翻页、内部 RAM 静态缓冲清理…）+ 验证方式 + 未做项 |
| `logs/2026-10-20.md` | 📝 会话记录 | 当天改动清单 + 验证方式 + **未解决项与下一步**（终端 panic 的取证）+ 两条硬教训（内部 RAM 缓冲区、上传回读校验） |

### `research/` — 已完结的调研归档（不再更新）

| 文档 | 状态 | 管什么 |
|---|---|---|
| `research/WASM_FEASIBILITY.md` | 🗄 归档 | WASM 路线调研（结论：推荐原生 `.so`） |
| `research/INK_CROSSING_FEASIBILITY.md` | 🗄 归档 | 《墨森物语》前置研究：距离实现差什么 |
| `research/TXT_CHAPTER_RESEARCH.md` | 🗄 归档 | TXT 章节 / 分卷研究（3 本真书实测，规则已落实成 `tools/scan_chapters.py`） |
| `research/TXT_CHAPTER_ONDEVICE.md` | 🗄 归档 | `old/` 阅读器盘点 + txt 分章核查结论 |

---

## 3. 按场景找文档

| 你要干什么 | 读这份 |
|---|---|
| **接手项目 / 第一次来** | 本文 → `guide/DEVELOPER.md`（含 ECS 架构附录） |
| **改界面**（版面/配色/控件/选中态/字号/文案） | `design/UI_DESIGN.md` |
| **写一个新 `.so` app** | `guide/APP_AUTHORING.md` → `design/UI_DESIGN.md` |
| **给 app 加图标 / 名称 / 描述** | `design/UI_DESIGN.md` 附录 B |
| **加触摸反馈音 / 换音效** | `design/UI_DESIGN.md` 附录 C |
| **改键盘 / 输入法** | `design/UI_DESIGN.md` §8 + 附录 A（全文） |
| **改刷新 / 波形 / 消残影 / 弹窗机制** | `display/EPD_TEST_MANUAL.md`（§〇 机制速览）→ `display/EPD_LUT_MANUAL.md`（波形总表/新波形实验） |
| **换字体 / 裁子集 / 字符集审计** | `display/TTF_FONT.md` |
| **省内存 / 加实体前算额度** | `architecture/ECS_OPTIMIZATION.md` |
| **插件加载 / `.so` 架构** | `architecture/ELF_PLUGIN_ARCHITECTURE.md`（WASM 结论在 `research/WASM_FEASIBILITY.md`） |
| **阅读器 / TXT 分章切块** | `reader/READER_TXT_RULES.md`（研究背景在 `research/TXT_CHAPTER_*.md`，API 边界在 `architecture/READER_API_SURFACE.md`） |
| **做设备上的终端 / shell** | `terminal/TERMINAL_SHELL.md` |
| **双核怎么分的 / 各核负载** | `architecture/DUAL_CORE_SPLIT.md` |

---

## 4. 规矩

1. **一个域一份权威**：写新内容前先问「它属于哪个域」，然后写进那份权威文档；**不要新建一份平行的**。
2. **不抄**：越界处**引用对方**（相对路径 + 章节号）。发现两处都在讲同一件事 → 保留权威那份，另一处改成指针。
3. **按类归档**：完结调研进 `research/`；设计稿进 `design/terminal-style/`；实测/持续更新的手册不进 `research/`。
4. 每份文档开头一句「这份管什么」，并列出「不在这份里」的指引。
5. **UI / 刷新 / 字体三块别互相抄** —— 各管一摊，越界处引用对方。
6. 文档面向**自己读、自己改、给 agent 看** → 只用 markdown。要**发同事**的交付物才做 docx/pdf。

---

## 5. 整理台账（合并 / 归档记录）

### 2026-10-20（晚）：工作日志开张 + 收工记录

| 动作 | 内容 |
|---|---|
| **新增** | `logs/2026-10-20.md`：当天全部改动（阅读器 v4 收尾、设置/SOUND、启动器、`/apps` 私有目录、网页 PSRAM 化）+ 验证方式 + **未解决项**（终端 panic 取证与 4 步下一步）+ 两条硬教训 |
| **规则** | `logs/` 只记"这次发生了什么"，规则/设计仍写各自权威（按 §4 规矩 1） |

### 2026-10-20：真终端（设备上的 shell）设计稿

| 动作 | 内容 |
|---|---|
| **新增** | `terminal/TERMINAL_SHELL.md`：分层（能力在固件 `papers3::sh`、界面在 `.so`）/ `so_sh_exec` 契约 / 命令集 v1（浏览·写·系统·网络配置）/ 路径与安全 / 输出截断 / 版面 / 里程碑 / 4 个待拍板问题 |
| **未动** | `design/UI_DESIGN.md`（等 M3 真正改界面时再加 §7.5 终端页版面） |

### 2026-10-20：阅读器 v4 重写（窗口化 + 五屏 + 自己的设置页）

| 动作 | 内容 |
|---|---|
| **重写** | `apps/reader/main/layout.h`：512KB 整本缓冲 → **256KB 滑动窗口**，页起点用文件字节偏移记身份（`PageStart{off,li}`）；分页与绘制合成同一个 `walk_page(draw=)`；页表改 PSRAM 按需翻倍 |
| **新增** | `apps/reader/main/pages.h`（书目/阅读/目录/阅读设置/分章 五屏）、`ui.h`（终端风 helper）、`cfg.h`（reader.cfg + 阅读进度） |
| **新增** | 阅读器自己的设置页：正文**字号 / 灰度 / 字体**，存 `/sdcard/apps/reader.cfg`；阅读进度存 `reader_progress.cfg` |
| **新增** | `apps/reader/test/`（排版引擎电脑端回归）+ `apps/reader/push-reader.sh`（带产物闸门的上传脚本） |
| **更新** | `design/UI_DESIGN.md` §7.4（阅读页版面：状态栏 / 正文 66..866 / 页脚细线+26 格进度条+点线行）、§12 落地现状 |
| **更新** | `reader/READER_TXT_RULES.md` §0（窗口已落地）、§4（目录页实际规格）、§5（字号缩放 / 章前换页 / 段缩进）、§7.1（引擎回归 3 用例） |

### 2026-10-19：设计文档合并 + `.so` 手册独立

| 动作 | 内容 |
|---|---|
| **合并** | `design/terminal-style/STYLE.md`（终端风格 v3）**全文并入** `design/UI_DESIGN.md`，原文件删除 |
| **重写** | `design/UI_DESIGN.md`：从旧稿（每页版面已过时、与 STYLE.md 重复）重写成**唯一权威**，新增 §0 风格判定、§12 落地现状与已知偏差 |
| **新增** | `guide/APP_AUTHORING.md`：`.so` 建 / 编 / 传 / 验（吸收原 STYLE.md §11 部署须知） |
| **新增** | `design/UI_DESIGN.md` 附录 B（应用图标与 `.m5meta` 元信息）、附录 C（触摸反馈音） |
| **改写** | `guide/DEVELOPER.md` §0.1（旧的「UI 先生 HTML 模拟」与现在的口径冲突 → 改成直接改代码）、§6/§7/§8 改成指向权威文档的指针 |
| **更新** | `architecture/DUAL_CORE_SPLIT.md` 标注「已落地」+ 实测负载 |
| **改引用** | 代码里 4 处 `STYLE.md §x` 注释 → 指向 `docs/design/UI_DESIGN.md` |
| 备份 | `~/Project/m5ui-doc-backup-20261019/`（本次整理前的 `docs/` 与根 md，共 23 个文件） |

### 2026-10-17（上一轮）

- `ARCHITECTURE.md` → `guide/DEVELOPER.md` 附录
- `EPD_DRAWING_NOTES.md` → `display/EPD_TEST_MANUAL.md` §〇
- `输入法设计.md` → `design/UI_DESIGN.md` 附录 A
- 删除死文档：`HANDOFF.md`、`README.en.md`
- 备份：`~/Project/papers3-docs-backup-20261017/`

### 更早

- `~/Project/m5ui-doc-backup-20261009/`
