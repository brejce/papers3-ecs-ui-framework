// apps/common/so_api.h — 宿主导出给 .so 的接口声明（所有 .so app 共用）
//
// 这些符号是固件 plugin.cpp 里 g_customer_elfsyms 导出的，dlopen 重定位时解析。
// .so 侧只需要 extern 声明 + 类型，不能 include 任何 IDF 头（白名单没有）。
//
// 签名必须和固件 plugin.cpp 里的 extern "C" 定义完全一致。

#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ══════════════════════════════════════════════════════════════
// ★ API 版本握手（Flipper FAP 的 api_version 那套）
//
// 每个 .so **必须**导出下面这个函数，固件在 dlopen 之后、调 page_init 之前先核对：
//
//     extern "C" __attribute__((visibility("default")))
//     uint32_t so_app_api_version(void) { return SO_API_VERSION; }
//
// 不导出 / 主版本不匹配 / .so 比固件新 → 固件**明确拒绝加载**，
// 把原因写到串口**和屏幕上**（launcher 会显示一行错误），
// 而不是让它跑到一半崩成"闪退/白屏"——那种失败只看得见现象、看不见原因。
//
// 改这里的版本号时，也要同步改固件 main/plugin.h 的 API_VERSION_MAJOR/MINOR，
// 并把 apps/ 下所有 .so 重编一遍（旧 .so 会被拒绝，这是**有意**的）。
// ══════════════════════════════════════════════════════════════
#define SO_API_VERSION_MAJOR 1
// 1: 新增 so_font_builtin / so_battery_level / so_is_charging
// 2: 新增 so_cpu_load / so_mem_used_pct
// 6: 新增 so_fb_save4 / so_fb_restore4 / so_fb_bytes（4bpp 精确页缓存）
//    （MINOR 只对「app 比固件新」生效 → 老的 reader.so(0) 依然能加载，不必重编旧的）
// 10: 新增 so_ime_open（.so 打开固件输入法覆盖层，输 SSID/口令等）
// 11: 新增 so_wifi_scan/so_wifi_saved/so_wifi_connect/so_wifi_forget（多热点扫描/连接）
// 12: so_ime_open 加 cancel 回调（编辑条 × 关闭按钮）
// 13: 新增 so_sound_test（触摸反馈音试听）
// 14: 新增 so_sh_exec（设备上的真 shell：apps/terminal 用）
#define SO_API_VERSION_MINOR 14
#define SO_API_VERSION ((uint32_t)((SO_API_VERSION_MAJOR << 16) | SO_API_VERSION_MINOR))

// 可选的显示名（没有就用文件名）。launcher 上显示这个，不再显示 reader.so
//     extern "C" __attribute__((visibility("default")))
//     const char* so_app_name(void) { return "阅读"; }

// ── 内存（PSRAM；md/txt 解析的块数组用，.so 里的 malloc 只能退内部 RAM）──
void* so_psram_alloc(size_t n);
void  so_psram_free(void* p);

// ── 字符串格式化（.so 白名单没有 snprintf/sprintf）──
int so_snprintf(char* out, int cap, const char* fmt, ...);

// ── 文件 ──
int so_list_books(char (*names)[256], int max);   // 扫 /sdcard/books，返回个数
int so_read_book(const char* name, char* buf, int cap);  // 读一本书进 buf，返回字节数
// 从 off 处续读一段（开书后台加载用）。返回读到字节数；0 = 已到文件尾；<0 = 失败。
// 短读（< want）也说明到文件尾了。
int so_read_book_at(const char* name, char* buf, int off, int want);
int so_book_size(const char* name);              // 书的总字节数；<0 = 失败
// /sdcard/apps/ 下的通用小文件存取（name 只能是纯文件名，不含 '/'）。返回字节数；<0 = 失败。
int so_blob_load(const char* name, char* buf, int cap);
int so_blob_save(const char* name, const char* buf, int len);
int so_list_fonts(char (*names)[80], int max);    // 扫 /sdcard/fonts，返回个数

// ── 配置（/sdcard/apps/reader.cfg，.so 自己记字体设置）──
int so_cfg_load(char* buf, int cap);              // 读进 buf，返回字节数；<0 失败
int so_cfg_save(const char* buf, int len);        // 写 len 字节，返回写入数

// ── 字体（句柄 = FontCtx*，多实例）──
void*  so_font_load(const char* name, float px);  // 按文件名加载（宿主拼 /sdcard/fonts/<name>）
void*  so_font_builtin(float px);                 // 内置字体（flash mmap，零 PSRAM、毫秒级）
void   so_font_free(void* f);
void   so_font_set_size(void* f, float px);
int    so_font_measure(void* f, const char* s, int32_t* out_w, int32_t* out_h);
int32_t so_font_measure_prefix(void* f, const char* s, int32_t char_count); // 量前 N 字符宽
int    so_font_draw(void* f, int32_t x, int32_t y, const char* s, uint8_t ink, int32_t ls);
int    so_font_ink_bounds(void* f, int32_t cp, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1);   // 单字形真墨迹框
int    so_font_text_ink(void* f, const char* s, int32_t ls, int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1);
       // ↑ 整串文字的真墨迹框（所有字形并集，相对文字原点）。**擦旧值+画新值**时把两个框并起来才是精确擦除范围
       // ink=亮度档（0 纯黑 ~ 255 白），ls=字间距像素；返回宽度
int    so_font_line_height(void* f);
int    so_font_has(void* f, int32_t cp);
float  so_font_size(void* f);                      // 读当前字号

// ── 电池（宿主轮询 + 缓存；.so 只读值，样式自己画）──
int so_battery_level(void);   // 0..100，-1 = 读不到
int so_cpu_load(void);        // CPU 占用率 0..100，-1 = 读不到（第一次调用只采样）
int so_mem_used_pct(void);      // 默认分配器视角（内部 SRAM + PSRAM）已用百分比，-1=不可用
// 内存明细（KB）：想分开显示内部/PSRAM 时用。which: 0=内部SRAM 1=PSRAM
int so_mem_size_kb(int which);  // 总量
int so_mem_free_kb(int which);  // 剩余
int so_clock_hhmm(void);      // 时间 HH*100+MM，-1 = 时间不可信（RTC 无值且未联网校准）→ 显示占位符
int so_clock_valid(void);     // 1 = 时间可信（RTC 读到过 或 网络校准过）
int so_is_charging(void);     // 0 放电 / 1 充电 / 2 未知

// ── 绘制 / 推屏 ──
int so_fill_rect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color);
int so_set_epd_mode(int mode);   // 1=QUALITY 2=TEXT 3=FAST 4=FASTEST
int so_display(int32_t x, int32_t y, int32_t w, int32_t h);
int so_wait_display(void);

// ── 帧缓冲快照 ──
// 下面这对是 **8bit 有损**的（readRect/pushImage 往返后只有灰阶 0/8/15 原样回来，
// 1-7 和 9-14 各裂成相邻两级）—— 只在"临时遮一下再还原"这种场景能用。
int so_fb_save(uint8_t* dst, int32_t x, int32_t y, int32_t w, int32_t h);
int so_fb_restore(const uint8_t* src, int32_t x, int32_t y, int32_t w, int32_t h);

// ── ★ 帧缓冲快照 · 4bpp 精确版（页缓存用这个）──
//   逐 nibble 直搬 _buf，**逐位无损** —— 反复 restore 也不会越看越花。
//   缓冲大小先问 so_fb_bytes(w,h)，别自己算 (w+1)/2*h。
//   ★ 恢复之后必须再 so_display(同一区域) 才会推上屏：copy_region_in 直接写 _buf，
//     不走 display 的脏矩形跟踪。
int so_fb_save4(uint8_t* dst, int32_t x, int32_t y, int32_t w, int32_t h);
int so_fb_restore4(const uint8_t* src, int32_t x, int32_t y, int32_t w, int32_t h);
int so_fb_bytes(int32_t w, int32_t h);      // 一块 w×h 的 4bpp 区域要多少字节

// ── 触摸（.so 的 page_tick 里轮询；宿主每帧调 .so 的 page_tick）──
int so_touch(int32_t* x, int32_t* y);   // 返回 1=有触点（*x/*y 填坐标），0=无
// ★ 只看"有没有手指"，**不消耗** so_touch 的补报序列。
//   除 page_tick（做按下沿判定）以外的轮询一律用这个 —— 比如预取绘制里的
//   "摸到手指就收手"。用错了会把给 page_tick 准备的按下补报吃掉，
//   症状是"点击坐标在不该消费的时候被消费"（点一下结果被别处用掉）。
int so_touch_down(void);

// ── 批处理（写一批 → 一次性推脏区）────────────────────────────────
// 用法：so_begin_write(); …随便画（so_fill_rect/so_font_draw，写的是帧缓冲）…; so_end_write();
// so_end_write 会把这一批累计写过的像素**并成一个矩形推一次**（Panel_EPD 的 _range_mod），
// 所以一轮里改多个格子（百分比 + 进度条 + 内存百分比…）只走一次波形。
// 没写任何像素 → 一次都不推（返回 0）。
// ★ 一轮里尽量把"先全部清除（FAST）再全部绘制（TEXT）"排好 —— 模式切换少一半。
void so_begin_write(void);
int  so_end_write(void);

// ── 计时 / 日志（.so 自测各阶段耗时；.so 里没有 esp_timer 也没有 ESP_LOGx）──
int64_t so_now_ms(void);
void    so_log(const char* msg);           // 串口打日志
void    so_log_file(const char* msg);      // 追加到 /sdcard/apps/reader.log（nullptr=清空）

// ── 退出回 launcher ──
void so_exit(void);

// ── 固件设置（NVS "m5ui"）── .so 改固件配置唯一的路
//   ★ set_* 只改内存（NVS 写会磨损 flash）；一批改完调一次 so_fwcfg_save() 落盘，
//     再调 so_fwcfg_apply() 让改动当场生效（重载字体 / 装波形表）。
//     size / ink / bw / wave_def 在固件里是每次绘制现读 → 改完自然生效。
//   整数键：size ink bw idle ap web wifi wave_def wave_sel ss dp
//   字符串键：font wifi_ssid wifi_pass scr_img
//   （"ink" 墨色档已废弃，不再出现在设置 UI，读写只为兼容 NVS 旧值）
int  so_fwcfg_get_int(const char* key, int def);
int  so_fwcfg_set_int(const char* key, int v);          // 越界自动夹回合理范围，返回实际值
int  so_fwcfg_get_str(const char* key, char* out, int cap);
int  so_fwcfg_set_str(const char* key, const char* v);
int  so_fwcfg_save(void);                                // 一次落盘；0 = 成功
void so_fwcfg_apply(void);                               // 让改动当场生效
int  so_fwcfg_reset(void);                               // 全部回默认值并落盘

// ── 网络：状态 + 开关 ──
int  so_net_state(void);      // 0 空闲 1 连接中 2 已连 3 失败
int  so_net_ap_on(void);      // 设备热点开着吗
int  so_net_web_on(void);     // 网页文件服务开着吗
int  so_net_set_ap(int on);   // 真开/关热点（会拉起/放下 WiFi 驱动）；返回新状态
int  so_net_set_web(int on);  // 真开/关文件服务；返回新状态
int  so_net_ip(char* out, int cap);     // 已连时填 IP，否则空串
int  so_net_ssid(char* out, int cap);   // 连过/正在连的热点名
int  so_net_connect(void);    // 用固件里存的 SSID/口令连一次（没存过 → -1）
void so_net_disconnect(void);
void so_net_forget(void);     // 忘掉已存热点（清 SSID/口令并落盘）

// ── 多热点（v11）──
int  so_wifi_scan(char* names, int cap, int max, int8_t* rssi); // 扫附近热点：SSID 逐行填进 names，rssi 填信号(dBm)，返回条数
int  so_wifi_saved(char* names, int cap, int max);        // 列出已存热点 SSID
int  so_wifi_connect(const char* ssid, const char* pass); // 已存→用存的口令连；新的→存下再连（pass 空 = 开放网络）；0=开始连 -1=失败
int  so_wifi_forget(const char* ssid);                    // 忘掉一个已存热点

// ── 输入法（.so 需要文本输入时用，比如设置页输 SSID/口令）──
// seed = 预填文本（可 null）；done = 完成回调，收到**固件内部缓冲区**的最终文本，
//   回调里要立刻拷走（下一次打开输入法会覆盖）；cancel = 点「×」取消时回调（可 null）。
//   返回 0 = 已打开，-1 = 输入法不可用。
int  so_ime_open(const char* seed, void (*done)(const char*), void (*cancel)(void));

// ── 触摸反馈音（设置页「试听」用）──
// 重新加载 cfg 里的音效文件并立刻播一遍；不看 snd 开关（试听就是要听见）。
// 反馈音本身由固件在每次「按下沿」自动播放，.so 不需要管。
int  so_sound_test(void);

// ── 设备上的真 shell（apps/terminal）──
// 执行一条命令行，把文本输出写进 out（以 '\0' 结尾，行间 '\n'，不带 ANSI 转义）。
//   line : 一整行（不含换行），最长 200 字节
//   cap  : 输出缓冲，建议 3072~4096
//   返回 : 写出的字节数（>=0）；-1 = 固件不支持；-2 = 参数非法
//   *code: 退出码（0 成功 / 1 命令错 / 2 用法错 / 3 路径越界或只读 / 4 执行失败）
//   *flags: bit0 = 已排重启（推完屏就不用管了，800ms 后自己重启）
// 命令实现在 main/platform/sh.h（虚根 /sdcard，/apps 会补成 /sdcard/apps）
int  so_sh_exec(const char* line, char* out, int cap, int* code, int* flags);

// ── 通用目录列举（.so 里没有文件系统 API，只能请宿主列）──
//   names = max 行 × cap 字节的连续缓冲；ext 只留该后缀（nullptr/""=全要）；返回条数
int  so_list_files(const char* dir, const char* ext, char* names, int cap, int max);

#ifdef __cplusplus
}
#endif
