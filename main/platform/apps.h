#pragma once

// platform/apps.h — 扫描 /sdcard/apps/ 下的 .so app（launcher 用）
//
// 规则和 fonts/screensaver 目录一致：**只扫这一层，不递归**；只认 .so 扩展名。
//
// ★ 2026-10-18：每个 .so 可以自带元信息（图标位图 + 名称 + 描述），放在 `.m5meta` 节里。
//   这里**不加载 .so、不执行任何代码** —— 只读文件头 + 节表 + 那一个节，
//   按 so_meta_t 直接解释（见 apps/common/so_meta.h）。
//   读不到就退回文件名，不会因为某个 app 忘了写元信息就崩。

#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include "platform/sd.h"
#include "so_meta.h"          // apps/common/so_meta.h（CMakeLists 已加 include 路径）

namespace m5ui::apps {

inline constexpr const char* APPS_DIR = "/sdcard/apps";

struct AppFile {
    char     name[64];      // 文件名（含 .so 后缀）
    uint32_t size;

    // ── 从 .m5meta 解出来的（has_meta=0 → label 用文件名兜底，icon 全空）──
    bool     has_meta;
    char     label[32];
    char     desc[96];
    int16_t  icon_w, icon_h;
    uint8_t  icon_bpp;
    uint8_t  icon[SO_META_ICON_MAX];
};

inline bool is_so(const char* n) {
    size_t l = strlen(n);
    return l >= 3 && n[l-3] == '.' &&
           (n[l-2] | 0x20) == 's' && (n[l-1] | 0x20) == 'o';
}

// ── ELF32 结构（只取读节表需要的字段）──
struct Ehdr {
    uint8_t  ident[16];
    uint16_t type, machine;
    uint32_t version, entry, phoff, shoff, flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct Shdr {
    uint32_t name, type, flags, addr, offset, size, link, info, addralign, entsize;
} __attribute__((packed));

// 从 .so 文件里读 .m5meta（不加载、不执行）
// ★ 大缓冲全部 static：这几块加起来 7KB+，放栈上会把主任务（默认 3.5KB）直接爆掉 ——
//   实测就是开机 panic 重启。本函数只在开机扫目录时顺序调用，不需要可重入。
inline bool read_meta(const char* path, AppFile& a) {
    static Shdr      sh[64];
    static char      names[4096];
    static so_meta_t meta;
    FILE* f = fopen(path, "rb");
    if (!f) return false;

    Ehdr eh;
    bool ok = false;
    do {
        if (fread(&eh, sizeof(eh), 1, f) != 1) break;
        if (memcmp(eh.ident, "\x7f" "ELF", 4) != 0) break;
        if (eh.ident[4] != 1) break;                       // 只要 ELFCLASS32
        if (eh.shoff == 0 || eh.shnum == 0 || eh.shnum > 64) break;
        if (eh.shstrndx >= eh.shnum) break;

        if (fseek(f, (long)eh.shoff, SEEK_SET) != 0) break;
        const int n = eh.shnum;
        if (fread(sh, sizeof(Shdr), (size_t)n, f) != (size_t)n) break;

        const Shdr& strtab = sh[eh.shstrndx];
        if (strtab.size == 0 || strtab.size > sizeof(names)) break;
        if (fseek(f, (long)strtab.offset, SEEK_SET) != 0) break;
        if (fread(names, 1, strtab.size, f) != strtab.size) break;
        names[strtab.size - 1] = 0;

        for (int i = 0; i < n; ++i) {
            if (sh[i].name >= strtab.size) continue;
            if (strcmp(names + sh[i].name, ".m5meta") != 0) continue;
            if (sh[i].size < sizeof(so_meta_t) || sh[i].size > 8192) break;

            if (fseek(f, (long)sh[i].offset, SEEK_SET) != 0) break;
            if (fread(&meta, 1, sizeof(meta), f) != sizeof(meta)) break;
            if (meta.magic != SO_META_MAGIC || meta.version != SO_META_VERSION) break;
            if (meta.icon_w == 0 || meta.icon_h == 0 ||
                meta.icon_w > 64 || meta.icon_h > 64) break;
            if (meta.icon_bpp != 1 && meta.icon_bpp != 4) break;
            const uint32_t need = SO_ICON_BYTES(meta.icon_w, meta.icon_h, meta.icon_bpp);
            if (meta.icon_bytes != need || need > SO_META_ICON_MAX) break;

            a.has_meta = true;
            snprintf(a.label, sizeof(a.label), "%.*s", (int)sizeof(a.label) - 1, meta.name);
            snprintf(a.desc,  sizeof(a.desc),  "%.*s", (int)sizeof(a.desc) - 1,  meta.desc);
            a.icon_w = meta.icon_w; a.icon_h = meta.icon_h; a.icon_bpp = meta.icon_bpp;
            memcpy(a.icon, meta.icon, need);
            ok = true;
            break;
        }
    } while (false);

    fclose(f);
    return ok;
}

// 扫 /sdcard/apps/*.so，返回个数（≤ max）。
// 目录不存在 / SD 没挂 → 返回 0（不报错，launcher 自己显示"没有 app"）。
inline int scan(AppFile* out, int max) {
    if (!sd::mount() || !out || max <= 0) return 0;
    DIR* d = opendir(APPS_DIR);
    if (!d) return 0;
    int n = 0;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr && n < max) {
        if (e->d_name[0] == '.' || !is_so(e->d_name)) continue;
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", APPS_DIR, e->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        AppFile& a = out[n];
        memset(&a, 0, sizeof(a));
        snprintf(a.name, sizeof(a.name), "%.*s", (int)sizeof(a.name) - 1, e->d_name);
        a.size = (uint32_t)st.st_size;

        if (!read_meta(path, a)) {                       // 没有元信息 → 文件名兜底
            const int len = (int)strlen(a.name) - 3;
            snprintf(a.label, sizeof(a.label), "%.*s", len > 0 ? len : 0, a.name);
            a.desc[0] = 0;
            a.icon_w = a.icon_h = 0;
            a.icon_bpp = 1;
        }
        n++;
    }
    closedir(d);
    ESP_LOGI("APPS", "%s: %d 个 .so", APPS_DIR, n);
    return n;
}

} // namespace m5ui::apps
