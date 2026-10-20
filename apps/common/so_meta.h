// apps/common/so_meta.h — .so 自带的元信息（图标位图 + 名称 + 描述）
//
// 设计要点：**固件不加载 .so**，而是直接把 .so 文件读进来，解析 ELF 节表找到
// `.m5meta` 节，按下面这个结构体解释。所以这里的东西必须是：
//   · 定长（固件按 sizeof 直接读）
//   · 自包含（不能有指针 —— 文件里的地址是链接期的，没重定位就是垃圾）
//   · 与地址无关（字符串 / 位图都内联在结构体里）
//
// app 侧用法（写在文件作用域，全局一次）：
//
//   #include "so_meta.h"
//   SO_APP_META("设置", "网络 · 显示 · 字体 · 电源", 32, 32, 1,
//               { 0x00, 0xFF, ... });      // 位图字节，行优先
//
// 位图格式：
//   bpp=1 → 每像素 1 bit，**MSB 在前**，位=1 画墨、0 留白（最常用，32×32 = 128 字节）
//   bpp=4 → 每像素 4 bit（高半字节在前），0=黑 … 15=白
  
#pragma once
#include <stdint.h>

#define SO_META_MAGIC    0x4D355549u   // 'M5UI'（小端文件里是 49 55 35 4D）
#define SO_META_VERSION  1
#define SO_META_ICON_MAX 512           // 位图最大字节数（32×32 4bpp = 512）

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t icon_w;
    uint16_t icon_h;
    uint8_t  icon_bpp;      // 1 或 4
    uint8_t  rsv;
    uint32_t icon_bytes;    // = SO_ICON_BYTES(w,h,bpp)
    char     name[32];      // UTF-8，NUL 结尾
    char     desc[96];      // UTF-8，NUL 结尾
    uint8_t  icon[SO_META_ICON_MAX];
} so_meta_t;

#define SO_ICON_BYTES(w, h, bpp) \
    ((((uint32_t)(w) * (uint32_t)(bpp) + 7u) / 8u) * (uint32_t)(h))

// retain：--gc-sections 不会把没人引用的 .m5meta 丢掉（GCC 11+ 的 SHF_GNU_RETAIN）
// ★ 两个坑：
//   1. 图标字节用可变参数传（写成 {...} 会被预处理器按逗号拆成上百个参数）
//   2. C++ 里 namespace 作用域的 const 默认是**内部链接** → --gc-sections 直接丢掉。
//      所以必须 extern 给它外部链接，链接器才当成导出符号、当成 GC 根保留下来。
#define SO_APP_META(NAME, DESC, W, H, BPP, ...)                                \
    __attribute__((section(".m5meta"), used, retain, visibility("default")))   \
    extern const so_meta_t so_app_meta;                                        \
    __attribute__((section(".m5meta"), used, retain, visibility("default")))   \
    const so_meta_t so_app_meta = {                                            \
        SO_META_MAGIC, SO_META_VERSION, (uint16_t)(W), (uint16_t)(H),           \
        (uint8_t)(BPP), 0, SO_ICON_BYTES(W, H, BPP), NAME, DESC,               \
        { __VA_ARGS__ }                                                        \
    }
