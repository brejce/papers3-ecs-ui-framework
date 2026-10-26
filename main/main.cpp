/*
 * papers3 — ECS UI 框架 demo + 中文拼音输入法（IME）
 *
 * 目录就按职责分，谁是什么一眼看得出：
 *   main.cpp            入口：初始化 + 建页 + 主循环，就这些
 *   app.h               App：M5 初始化 / World / 主循环（原 papers3.h）
 *   ecs/                ECS 内核：world / components
 *   systems/            ECS 系统：flex / touch / render
 *   ui/                 UI 层（组件 + 设计令牌）
 *     theme.h           设计令牌（底/墨/描边/阴影的取值）
 *     widgets.h         基础部件工厂（button / input_frame / input_text / cursor / label / toggle / flex）
 *     icons.h           图元图标（字体里没有的箭头 / 退格 / A-a）
 *     keyboard.h        屏幕键盘组件（独立成型，输出端可插拔）
 *     candidate_bar.h   候选栏组件（一行候选格 + 翻页箭头）
 *     qr.h              二维码绘制（按模块画，version 从 1 往上试）
 *   pages/              页面（每页一个文件：只管本页存储 + 把组件接起来）
 *     manager.h         页面管理：多页共存，切页只切 Visible + 覆盖层/band
 *     launcher.h        首页：app 启动器（扫 /sdcard/apps 里的 .so）
 *     ime.h             输入法覆盖层（不是页面，.so 应用靠 so_ime_open 调它）
 *   text/               文本与输入法
 *     ttf.h             字体光栅化（flash mmap 的鸿蒙裁切版）
 *     ime.h             拼音输入法状态机（键盘的一个输出端）
 *     pinyin_store.h    索引存储：内置默认 / SD 文件 / 运行时词频
 *     pinyin_index.h    内置索引（tools/make_pinyin_index.py 从字体 cmap 生成）
 *     markdown.h        极简 Markdown 解析
 *   platform/           平台杂项
 *     sd.h              SD 卡挂载 + 找文件（供字体 / 词库 / 手机文件管理用）
 *     wifi_ap.h         开热点（AP；WiFi 模式从一开始就是 APSTA）
 *     wifi_sta.h        连外部热点（STA，比如手机热点）
 *     web_files.h       网页文件管理（目录列表 / 上传 / 下载 / 删 / 改名 / 建目录）
 *   third_party/        stb_truetype
 *
 * 没有继承，没有虚函数。
 * 按钮 = Rect+Text+Style+Touchable+OnClick；输入框 = 外框 + 文字 + 光标 三个独立实体。
 *
 * 构建：ESP-IDF
 *   idf.py build flash monitor
 */

#include <cstdio>
#include <cstring>
#include "esp_attr.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"   // 双核：ui_task 钉在 core1
#include "freertos/task.h"
#include "app.h"
#include "pages/manager.h"
#include "pages/ime.h"
#include "pages/launcher.h"
#include "platform/sd.h"
#include "platform/nvs.h"
#include "platform/wifi_ap.h"
#include "platform/wifi_sta.h"
#include "platform/web_files.h"
#include "platform/sercli.h"     // 串口命令行 + 文件传输（插线就能传，不依赖 WiFi）
#include "platform/sound.h"    // 触摸反馈音：开机按配置加载音量/音效
#include "systems/epd_lut.h"   // 波形档（开机装表）
#include "plugin.h"
#include "text/ime.h"

using namespace papers3;

// 上一次的复位原因。放 RTC 不掉电内存：**跨复位有效、断电丢失**，
// 且不写 flash/不磨 NVS —— 刚好用来回答「刚才是不是崩了重启」。
// 第一次上电是随机值，所以按范围夹一下。
RTC_NOINIT_ATTR static uint32_t g_prev_rst;

static const char* rst_name(int r) {
    switch (r) {                                  // 值取自 esp_system.h 的 esp_reset_reason_t
        case ESP_RST_POWERON:    return "上电";
        case ESP_RST_EXT:        return "外部引脚";
        case ESP_RST_SW:         return "软件复位";
        case ESP_RST_PANIC:      return "*** 异常/panic（崩溃）";
        case ESP_RST_INT_WDT:    return "中断看门狗";
        case ESP_RST_TASK_WDT:   return "任务看门狗";
        case ESP_RST_WDT:        return "看门狗";
        case ESP_RST_DEEPSLEEP:  return "深睡唤醒";
        case ESP_RST_BROWNOUT:   return "*** 掉压 brownout";
        case ESP_RST_SDIO:       return "SDIO";
        case ESP_RST_USB:        return "USB（连串口触发的，不是故障）";
        case ESP_RST_JTAG:       return "JTAG";
        case ESP_RST_EFUSE:      return "efuse 错误";
        case ESP_RST_PWR_GLITCH: return "电源毛刺";
        case ESP_RST_CPU_LOCKUP: return "*** CPU 锁死";
        default:                 return "未知";
    }
}

// ═══ core1：UI 任务 —— 只跑 UI（ECS / 渲染 / 触摸 / .so），独占一个核 ═══
// 全项目唯一读 GT911 的地方就在这里（app.update → M5.update，加上 .so 的 so_touch/so_wait_display）。
// 别在别的任务里读 M5.Touch —— GT911 的点是「读一次就被消费」，两个读者会互相抢。
static void ui_task(void* arg) {
    App& app = *(App*)arg;
    ESP_LOGI("MAIN", "ui_task 起在 core %d", xPortGetCoreID());
    for (;;) {
        const int64_t t0 = esp_timer_get_time();
        systems::g_push_n = 0;
        systems::g_push_us = 0;
        systems::g_push_px = 0;
        app.update();
        const int64_t dt = (esp_timer_get_time() - t0) / 1000;
        // 平常只报慢帧；trace 窗口内把有推送/有耗时的帧也报出来（空转帧不刷屏）
        if (dt > 120 || (esp_timer_get_time() < systems::g_trace_until && (systems::g_push_n || dt > 20))) {
            ESP_LOGW("PERF", "帧 %lldms · 推屏 %u 次 %lldms · 面积 %lldK px（整屏=506K）",
                     dt, (unsigned)systems::g_push_n,
                     (long long)(systems::g_push_us / 1000),
                     (long long)(systems::g_push_px / 1000));
        }
        pages::flush_band();        // 覆盖层/还原：渲染跑完一趟后用 QUALITY 推那条带
        pages::launcher::tick();    // 顶栏时钟：分钟跳了才重画
        plugin::tick();             // .so app 每帧回调（page_tick）
        M5.delay(1);
    }
}

extern "C" void app_main() {
    static App app;          // static：App 里有 ecs::World（约 1.3KB），放 .bss 不占主任务栈
    pages::g_ctx.app = &app;

    {
        const int cur = (int)esp_reset_reason();
        const uint32_t prev = (g_prev_rst <= 15) ? g_prev_rst : 99;   // 上电首帧是随机值
        ESP_LOGI("MAIN", "复位: 本次=%d(%s)  上次=%u(%s)",
                 cur, rst_name(cur), (unsigned)prev,
                 prev == 99 ? "（上电首帧，无记录）" : rst_name((int)prev));
        g_prev_rst = (uint32_t)cur;
    }

    app.begin();

    // ── 1) 读设置（NVS）。字体/字号由它决定 ──
    cfg::init();             // 幂等；wifi_ap.h 里也会调，谁先都行
    cfg::load();

    // ── 1b) 波形档：装自定义 LUT 表 ──
    //   「默认配置」开（wave_def=1）→ 不换表，档位由部件按内容自选（用原厂表）。
    //   「默认配置」关（wave_def=0）→ 全局强制 wave_sel 那一档，并装上对应自定义表。
    //   档位：1=gc13 2=text12 3=fast 4=fastest 5=bw4（见 systems/epd_lut.h）
    //   ★ 这里能安全换表的前提：app.begin() 里的首屏 QUALITY 已经 waitDisplay 过，
    //     此刻没有排队中的 display（Panel_EPD::reload_lut_table 不能在推屏途中调）。
    {
        // ★ 画字用 gc13（2026-10-15 拍板）：quality 槽**总是**装 lut_quality13。
        //   依据 docs/display/EPD_LUT_MANUAL.md §6.4 —— 文字是 TTF 抗锯齿灰度，
        //   fast/fastest 档在写帧缓冲时就量化成 1bit+Bayer 抖动（字变网点），
        //   只有 quality/text 槽出平滑灰；而 §0 说 GC13 = quality 族砍掉 16 行 hold，
        //   整屏 831ms（GL16 是 1448ms）且**唯一无残留** —— 又快又干净的那个落点。
        //   所以这一份是"画字档"的基线，与下面的全局档各占各的槽，互不冲突
        //   （gc13=quality，text12=text，fast/bw4=fast，fastest=fastest）。
        //   ★ 这里能安全换表的前提：app.begin() 里的首屏 QUALITY 已经 waitDisplay 过，
        //     此刻没有排队中的 display（reload_lut_table 不能在推屏途中调）。
        bool ok = epd::apply_lut(epd::TIER_GC13);
        if (!ok) ESP_LOGE("MAIN", "gc13 装表失败 —— 画字会退回原厂 GC16（更慢）");

        // 「默认配置」关（wave_def=0）→ 再装用户选的那一档（它的槽与 quality 不冲突）
        if (!cfg::g_cfg.wave_def && cfg::g_cfg.wave_sel != epd::TIER_GC13)
            ok = epd::apply_lut(cfg::g_cfg.wave_sel) && ok;

        ESP_LOGI("MAIN", "波形: 画字=gc13；全局档=%s（wavdef=%u wavsel=%u）%s",
                 cfg::g_cfg.wave_def ? "默认配置/部件自选"
                                     : epd::tier_name(cfg::g_cfg.wave_sel),
                 cfg::g_cfg.wave_def, cfg::g_cfg.wave_sel,
                 ok ? "" : " ← 部分装表失败，该槽保持原厂表");
    }

    // ── 2) 先映射 flash 里的默认字体 —— 它是**兜底**，SD 字体加载失败就靠它 ──
    bool ttf_ok = ttf::load("fonts", (float)cfg::g_cfg.size);
    // ── 3) 按 NVS 里的选择加载字体。★ 不自动挑 —— 没选过就用内置 ──
    bool sd_ok = sd::mount();
    if (sd_ok) sercli::start();      // 串口传文件：SD 挂上就能用，WiFi 断着也没关系
    if (sd_ok) sd::ensure_font_dir();
    if (sd_ok && cfg::g_cfg.font[0]) {
        char path[320];
        cfg::font_path(path, sizeof(path));
        ESP_LOGI("MAIN", "NVS 指定的字体: %s", path);
        if (ttf::load_from_sd(path, (float)cfg::g_cfg.size)) {
            ttf_ok = true;
        } else {
            // 卡拔了 / 文件删了 / CFF / 超 PSRAM —— 回退内置，并把 NVS 里的选择清掉。
            // ★ 不清的话每次开机都会失败一次、白磨一次 NVS。
            ESP_LOGW("MAIN", "NVS 里的字体不可用 → 回退内置并清掉记录");
            cfg::clear_font();
            ttf_ok = ttf::load("fonts", (float)cfg::g_cfg.size);
        }
    } else {
        ESP_LOGI("MAIN", "没有选过字体 —— 用内置（不是自动挑）");
    }

    // ── 3b) 触摸反馈音：音量 + 音效文件（SD 就绪之后再加载）──
    papers3::sound::reload();

    if (ttf_ok) {
        ttf::set_size((float)cfg::g_cfg.size);
        ESP_LOGI("MAIN", "TTF ready: line_height=%d source=%s size=%u",
                 ttf::line_height(), ttf::source(), cfg::g_cfg.size);
    }

    // ── 4) 拼音索引：SD 上有就用 SD 的（可升级：换文件即换词库），没有就用内置 ──
    if (sd_ok) {
        ime::table_load(ime::g_table);
        if (!ime::g_table.from_sd)
            ime::table_save(ime::g_table);   // 首次：把内置默认表落盘，给用户一个可改的起点
    } else {
        ime::table_builtin(ime::g_table);    // 没卡也照样能用
    }
    ESP_LOGI("MAIN", "pinyin index: %d syllables (%s)",
             ime::g_table.count, ime::g_table.from_sd ? "SD" : "builtin");

    // ── 页快照快版判定 ──
    // 开机做一次（结论存 NVS，第二次开机起直接读，不重跑）：
    //   全屏 out 原版 vs 快版逐字节比对 + 4 个区域（含越界）验证。
    // 放在这里而不是"第一次用到页缓存时" —— 免得卡在用户点开书那一下。
    fb_check_init();

    // ★ core1 触摸采样任务已撤销 ——
    //   GT911 的点数据是"读一次就被消费"的（驱动把 0x814E 清零后值才失效），
    //   两个任务各读一次会互相抢：任务读走 → 主循环 M5.update() 读到空 → 固件页面按不动；
    //   主循环读走 → so_touch 读到空 → 阅读页三分区有时收不到触摸。还会互相锁存出幻触。
    //   Touch_Class 本身就有共享缓存（update() 4ms 一次，所有人读缓存），
    //   正确做法就是**单一读者 + 读缓存**，不要在插件里读原始点。
    // plugin::touch_task_start();

    // ── 建页（一次性建好，之后切页只切 Visible）──
    auto& w = app.world;

    pages::begin(pages::g_launcher);
    pages::launcher::build(w);         // 首页：app 启动器

    // 输入法 = 覆盖层能力（不是页面）：建实体 + 登记 g_open_overlay
    pages::ime_page::build(w);
    pages::ime_page::refresh_all();    // 把候选栏/拼音条/状态栏摆成初始态

    pages::focus(pages::g_launcher);  // 当前页 = launcher（首页）

    ESP_LOGI("MAIN", "entities: launcher=%d total=%d",
             pages::g_launcher.n, (int)w.count());

    // ── 双核分工（2026-10-18）：UI 独占 core1，core0 只留「不怕抖」的 ──
    //   core1：ui 任务 = ECS + 渲染 + 触摸采样 + .so page_tick（独占，只有 ipc1/IDLE1 陪跑）
    //   core0：wifi(23) + sys_evt(20，IDF 硬编码 core0) + esp_timer(22) + lwIP + httpd + epd 推屏
    //   为什么这样分：sys_evt 挪不走，WiFi 与 UI 同核就永远抢 UI；界面被网络拖慢比推屏慢一点难受得多。
    //   触摸仍是**单一读者**（全在这个 ui 任务里），不需要跨核事件队列。
    xTaskCreatePinnedToCore(ui_task, "ui", 12288, &app, 5, nullptr, 1);
    ESP_LOGI("MAIN", "双核启动：ui=core1(prio5) / wifi+sys_evt+epd=core0");

    // core0 主任务：只剩 WiFi 编排（做一次），之后空转
    {
        // ★ 三个独立开关（连接页），各自记忆（NVS）：热点(ap) / WiFi(wifi) / 网页(web)
        //   —— 互不影响，开机按各自的记忆来。
        const bool want_ap   = cfg::g_cfg.ap;
        const bool want_wifi = cfg::g_cfg.wifi && cfg::g_cfg.wifi_ssid[0];
        const bool want_web  = cfg::g_cfg.web;

        // 1) 热点：要 → 起驱动 + 开 AP；不要但 WiFi 要 → 只起 STA 形态的驱动
        if (want_ap) {
            ESP_LOGI("MAIN", "热点开关=开 → 起设备热点");
            wifi_ap::start(true);
        } else if (want_wifi) {
            ESP_LOGI("MAIN", "热点=关 / WiFi=开 → 只起 STA（别人搜不到设备热点）");
            wifi_ap::start(false);
        } else {
            ESP_LOGI("MAIN", "热点、WiFi 都关 → WiFi 驱动不起");
        }

        // 2) WiFi：存过热点就连（跟热点开关无关）
        if (want_wifi) {
            ESP_LOGI("MAIN", "WiFi开关=开 → 自动连 '%s'…", cfg::g_cfg.wifi_ssid);
            wifi_sta::connect(cfg::g_cfg.wifi_ssid, cfg::g_cfg.wifi_pass);
        } else if (cfg::g_cfg.wifi_ssid[0]) {
            ESP_LOGI("MAIN", "WiFi开关=关 → 不自动连（SSID/密码还留着）");
        }

        // 3) 网页文件服务：独立开关，跟热点/WiFi 无关（起了之后 AP/STA 都能访问）
        if (want_web) {
            ESP_LOGI("MAIN", "网页开关=开 → 起 httpd（AP/STA 任一条通就能用）");
            web_files::start();
        } else {
            ESP_LOGI("MAIN", "网页开关=关 → 不起 httpd");
        }
    }

    // ★ core0 循环：网页服务自愈放在**这里**，不放 ui_task。
    //   自愈动作是 httpd_stop()+start()，它会等 httpd 任务退出；放 core1 的话
    //   正好在上传/下载时会把整个 UI 卡住（触摸、渲染全停）。
    for (;;) {
        web_files::tick();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
