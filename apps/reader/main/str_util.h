// apps/reader/main/str_util.h — .so 自带字符串工具
//
// ★ elf_loader 的 libc 白名单（esp_elf_symbol.c）里 string.h 只有：
//   strerror / memset / memcpy / strlen / strtod / strrchr / strchr / strcmp /
//   strtol / strcspn / strncat
//   —— **没有 strncmp、没有 strstr、没有 memcmp/snprintf/strcpy/strcat**。
//   用了就 relocation failed（errno=-88）、dlopen 失败、页面卡死。
//
// 缺的自己实现（纯逻辑，编译后零外部符号；别指望编译器替你调 memcmp，写完用
// `nm -u` 核一遍）。

#pragma once
#include <stddef.h>

namespace app {

// 等价 strncmp：返回负/0/正（只用于 ==0 判断也安全）
inline int my_strncmp(const char* a, const char* b, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[i];
        if (ca != cb) return (int)ca - (int)cb;
        if (ca == 0) return 0;
    }
    return 0;
}

// 等价 strstr：找 needle 首次出现，找不到返回 nullptr
inline const char* my_strstr(const char* h, const char* n) {
    if (!n[0]) return h;
    for (; *h; h++) {
        const char* p = h;
        const char* q = n;
        while (*p && *q && *p == *q) { p++; q++; }
        if (!*q) return h;
    }
    return nullptr;
}

} // namespace app
