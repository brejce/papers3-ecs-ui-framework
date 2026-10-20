#pragma once

namespace m5ui::plugin {

// ── API 版本（必须和 apps/common/so_api.h 的 SO_API_VERSION_MAJOR/MINOR 一致）──
// .so 侧导出 so_app_api_version() 报自己的版本；主版本不一致就拒绝加载。
inline constexpr uint16_t API_VERSION_MAJOR = 1;
inline constexpr uint16_t API_VERSION_MINOR = 13;   // 2: +so_cpu_load/so_mem_used_pct · 6: +4bpp 页快照 · 7: +so_read_book_at · 8: +so_book_size/so_blob_load/so_blob_save · 9: +so_cfg_*/so_net_*/so_list_files · 10: +so_ime_open · 11: +so_wifi_scan/so_wifi_saved/so_wifi_connect/so_wifi_forget · 12: so_ime_open 加 cancel 回调 · 13: +so_sound_test

// 从 SD 卡加载 hello_page.so 并调用 page_init()（.so 插件页验证）
void test();

// 加载 /sdcard/apps/<name>（相对路径，含 .so）并调用 page_init()（launcher 打开 app 用）
bool load_app(const char* name);

// 最近一次 load_app 失败的原因（中文，一行；空串 = 没失败过）。
// launcher 用它把错误显示在屏幕上 —— 加载失败只打串口等于没报错。
const char* last_error();

// app 每帧回调（主循环调）：.so 若导出了 page_tick 就调它（交互用）
void tick();

// 退出 app（so_exit 调）：停掉每帧回调（不 dlclose，.so 常驻）
void close();

// 是否有 .so app 正在运行（电源管理用：独占时别熄屏 —— 唤醒没东西可重画 → 白屏）
bool active();

}   // namespace m5ui::plugin

// ── 下面的函数定义在 plugin.cpp 里、位于 namespace **之外** ──

// 页快照"快版"搬运的判定入口（开机调一次）：
// 读 NVS 里存过的结论 → 有就直接用（不重跑自检）；没有就跑一次完整自检并存盘。
// 自检内容：全屏/子区域/越界共 4 个区域，原版 vs 快版逐字节 memcmp。不一致则退回原版。
void fb_check_init();