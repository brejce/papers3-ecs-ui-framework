// main/platform/sh.h — 设备上的**真** shell（给 apps/terminal 用）
//
// 为什么在这儿：.so 里只有 so_api.h 白名单那几十个函数（没有 fopen/opendir/stat，
// 也没有 esp_*），而 `malloc` 还只能拿内部 RAM（本来就只剩 10~20KB）。
// 所以「能不能操作设备」这件事只能在固件里做，.so 只负责画界面和收键盘。
//
// 对外只有一个入口：exec(line, out, cap, &code, &flags)
//   ・输出是纯文本（行间 '\n'，不带 ANSI），按 cap 截断，截断点落在 UTF-8 字符边界
//   ・code: 0 成功 / 1 命令错 / 2 用法错 / 3 路径越界或只读 / 4 执行失败
//   ・flags: bit0 = 已排了重启（调用方推完屏就行，800ms 后自己会重启）
//
// 语义尽量贴近 Linux（用户口径「类似 linux 的吧」）：
//   rm -r 不二次确认、reboot 不二次确认，但 rm -r / 与 rm -r /sdcard 一律拒绝。
#pragma once

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdarg>
#include <cctype>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "platform/nvs.h"
#include "platform/wifi_sta.h"
#include "platform/wifi_ap.h"
#include "platform/web_files.h"
#include "platform/wifi_list.h"
#include "so_api.h"      // launch/apps 用 so_list_files；Out 用 so_snprintf 风格

// plugin.cpp 里实现（打开一个 /sdcard/apps/<name>.so）；sh.h 只管转发
extern "C" int so_app_launch_internal(const char* name);
// 输入法（宿主覆盖层）：诊断用，和 .so 调 so_ime_open 走同一条路
extern "C" int so_ime_open(const char* seed, void (*done)(const char*), void (*cancel)(void));

namespace papers3::sh {

static const char* TAG  = "SH";
static const char* ROOT = "/sdcard";

enum { RC_OK = 0, RC_CMD = 1, RC_USE = 2, RC_PATH = 3, RC_FAIL = 4 };
enum { FLAG_REBOOT = 1 };

// ══════════════════════════════════════════════════════════════
//  输出缓冲（UTF-8 安全截断）
// ══════════════════════════════════════════════════════════════
struct Out {
    char* b = nullptr;
    int   cap = 0;
    int   n = 0;
    bool  cut = false;

    void raw(const char* s, int len) {
        if (!b || cap <= 0) return;
        if (n + len >= cap - 1) {                     // 留一字节给 '\0'，且别切半个汉字
            int room = cap - 1 - n;
            while (room > 0 && (((unsigned char)s[room]) & 0xC0) == 0x80) room--;
            if (room > 0) { memcpy(b + n, s, room); n += room; }
            cut = true;
            b[n] = 0;
            return;
        }
        memcpy(b + n, s, len);
        n += len;
        b[n] = 0;
    }
    void s(const char* s) { if (s) raw(s, (int)strlen(s)); }
    void nl() { raw("\n", 1); }
    void line(const char* s = nullptr) { if (s) this->s(s); nl(); }
    void f(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char t[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t, sizeof(t), fmt, ap);
        va_end(ap);
        s(t);
    }
    void fl(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char t[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(t, sizeof(t), fmt, ap);
        va_end(ap);
        s(t);
        nl();
    }
};


// ══════════════════════════════════════════════════════════════
//  工作缓冲：**一律走 PSRAM**
//  ★ 这个项目内部 RAM 常年在 10~20KB，函数里写个大 static 数组会直接把
//    EPD 的 LUT 装表挤失败（实测：sh.h 里 20KB 静态 → "换表失败" → 开机画字时
//    readRect 空指针 panic，整机进恢复重启循环）。所以大缓冲只用 PSRAM。
// ══════════════════════════════════════════════════════════════
inline void* sh_scratch(size_t need) {
    static uint8_t* p = nullptr;
    static size_t cap = 0;
    if (need > cap) {
        if (p) heap_caps_free(p);
        p = (uint8_t*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        cap = p ? need : 0;
    }
    return p;
}

// ══════════════════════════════════════════════════════════════
//  路径：虚拟 cwd + 夹在 /sdcard 里
//    /apps → /sdcard/apps（敲起来短），".." 不许越过 /sdcard
// ══════════════════════════════════════════════════════════════
inline char g_cwd[192] = "/sdcard";

// 把 a/b 拼起来并规范化到 out；结果永远落在 /sdcard 里（chroot 语义）
inline bool norm(const char* in, char* out, size_t cap) {
    char tmp[384];
    if (!in || !in[0])           snprintf(tmp, sizeof(tmp), "%s", g_cwd);
    else if (in[0] == '/')       snprintf(tmp, sizeof(tmp), "%s", in);
    else                         snprintf(tmp, sizeof(tmp), "%s/%s", g_cwd, in);

    // 逐段处理：收 '/' 分段、丢 "."、".." 弹栈（栈空就不动 → 不能回到根之上）
    char work[384];
    int  o = 0;
    int  segstart[32];
    int  nseg = 0;
    work[0] = 0;
    const char* p = tmp;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* q = p;
        while (*q && *q != '/') q++;
        const int len = (int)(q - p);
        if (len == 1 && p[0] == '.') { p = q; continue; }
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            if (nseg > 0) { o = segstart[--nseg]; work[o] = 0; }
            p = q;
            continue;
        }
        if (o + len + 2 >= (int)sizeof(work) || nseg >= 31) break;
        segstart[nseg++] = o;                 // 这一段前面的 '/' 的位置
        work[o++] = '/';
        memcpy(work + o, p, len);
        o += len;
        work[o] = 0;
        p = q;
    }

    // /apps → /sdcard/apps；已经是 /sdcard 开头就原样；空 → /sdcard
    if (nseg == 0)                              snprintf(out, cap, "%s", ROOT);
    else if (strncmp(work, ROOT, strlen(ROOT)) == 0) snprintf(out, cap, "%s", work);
    else                                        snprintf(out, cap, "%s%s", ROOT, work);
    return true;
}

// 只读保护：
//   ① /sdcard/apps 这一层本身（`.so` 和每个 app 的私有目录都在里面）不许删/挪；
//   ② apps/ 下的 `.so`（别一 rm 把自己删了）。
// ★ 原来只认"以 .so 结尾的文件"，于是 `rm -r /sdcard/apps` 能绕过去，
//   而 rm_rec 递归时又逐个 unlink —— 一把就能把所有 app 删光。
inline bool readonly_path(const char* p) {
    if (!p) return false;
    if (strcmp(p, "/sdcard/apps") == 0 || strcmp(p, "/sdcard/apps/") == 0) return true;
    const size_t n = strlen(p);
    return n > 3 && strcmp(p + n - 3, ".so") == 0 && strncmp(p, "/sdcard/apps/", 13) == 0;
}

// ══════════════════════════════════════════════════════════════
//  小工具
// ══════════════════════════════════════════════════════════════
inline const char* basename_of(const char* p) {
    const char* s = strrchr(p, '/');
    return s ? s + 1 : p;
}

inline void human(char* out, int cap, long long n) {
    if (n < 1024)                snprintf(out, cap, "%lldB", n);
    else if (n < 1024LL * 1024)  snprintf(out, cap, "%.1fK", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024) snprintf(out, cap, "%.1fM", n / 1048576.0);
    else                         snprintf(out, cap, "%.2fG", n / 1073741824.0);
}

// 读文件的一段（不开 stdio 也得开，这里就是固件）
inline int read_span(const char* path, long off, int want, char* buf, int cap) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    if (off > 0) fseek(f, off, SEEK_SET);
    const int n = (int)fread(buf, 1, (size_t)((want < cap - 1) ? want : cap - 1), f);
    fclose(f);
    buf[(n > 0) ? n : 0] = 0;
    return n;
}

// ══════════════════════════════════════════════════════════════
//  命令：文件浏览
// ══════════════════════════════════════════════════════════════
inline int cmd_pwd(Out& o, int, char**) { o.line(g_cwd); return RC_OK; }

inline int cmd_cd(Out& o, int argc, char** argv) {
    char p[400];
    const char* t = (argc > 1) ? argv[1] : "/sdcard";
    if (!norm(t, p, sizeof(p))) { o.fl("cd: path outside /sdcard: %s", t); return RC_PATH; }
    struct stat st;
    if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) { o.fl("cd: no such directory: %s", t); return RC_FAIL; }
    snprintf(g_cwd, sizeof(g_cwd), "%.190s", p);
    return RC_OK;
}

struct Ent { char name[56]; bool dir; long size; time_t t; };   // 表本体放 PSRAM（见 sh_scratch）

inline int cmd_ls(Out& o, int argc, char** argv) {
    char p[400];
    const char* t = (argc > 1) ? argv[1] : ".";
    if (!norm(t, p, sizeof(p))) { o.fl("ls: path outside /sdcard: %s", t); return RC_PATH; }
    DIR* d = opendir(p);
    if (!d) { o.fl("ls: cannot open: %s", t); return RC_FAIL; }

    Ent* e = (Ent*)sh_scratch(sizeof(Ent) * 48);
    if (!e) { o.line("ls: no memory"); return RC_FAIL; }
    int n = 0;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr && n < 48) {
        if (de->d_name[0] == '.') continue;
        struct stat st;
        char full[480];
        snprintf(full, sizeof(full), "%.380s/%.70s", p, de->d_name);
        if (stat(full, &st) != 0) continue;
        snprintf(e[n].name, sizeof(e[n].name), "%.55s", de->d_name);
        e[n].dir  = S_ISDIR(st.st_mode);
        e[n].size = (long)st.st_size;
        e[n].t    = st.st_mtime;
        n++;
    }
    closedir(d);

    // 目录在前、名字升序（qsort 的比较函数要顶层，放下面）
    qsort(e, (size_t)n, sizeof(Ent), [](const void* a, const void* b) -> int {
        const Ent* x = (const Ent*)a;
        const Ent* y = (const Ent*)b;
        if (x->dir != y->dir) return x->dir ? -1 : 1;
        return strcmp(x->name, y->name);
    });

    for (int i = 0; i < n; i++) {
        struct tm tm;
        localtime_r(&e[i].t, &tm);
        char sz[16];
        if (e[i].dir) snprintf(sz, sizeof(sz), "<DIR>");
        else          human(sz, sizeof(sz), e[i].size);
        o.fl("%c %04d-%02d-%02d %02d:%02d %8s  %s",
             e[i].dir ? 'd' : '-', tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, sz, e[i].name);
    }
    uint64_t tot = 0, fr = 0;
    if (esp_vfs_fat_info(ROOT, &tot, &fr) == ESP_OK) {
        char a[16], b2[16];
        human(a, sizeof(a), (long long)tot);
        human(b2, sizeof(b2), (long long)fr);
        o.fl("-- %d entries · %s free of %s", n, b2, a);
    } else {
        o.fl("-- %d entries", n);
    }
    return RC_OK;
}

inline int cmd_cat(Out& o, int argc, char** argv) {
    if (argc < 2) { o.line("usage: cat <file>"); return RC_USE; }
    char p[400];
    if (!norm(argv[1], p, sizeof(p))) { o.fl("cat: path outside /sdcard: %s", argv[1]); return RC_PATH; }
    struct stat st;
    if (stat(p, &st) != 0 || S_ISDIR(st.st_mode)) { o.fl("cat: no such file: %s", argv[1]); return RC_FAIL; }

    char* buf = (char*)sh_scratch(4096);
    if (!buf) { o.line("cat: no memory"); return RC_FAIL; }
    const int n = read_span(p, 0, 4096, buf, 4096);
    if (n < 0) { o.fl("cat: read failed: %s", argv[1]); return RC_FAIL; }
    if (memchr(buf, 0, (size_t)n)) { o.fl("cat: binary file (%ld bytes)", (long)st.st_size); return RC_OK; }
    o.raw(buf, n);
    if (n > 0 && buf[n - 1] != '\n') o.nl();
    if ((long)st.st_size > n) o.fl("… (%ld more bytes)", (long)st.st_size - n);
    return RC_OK;
}

inline int head_tail(Out& o, int argc, char** argv, bool tail) {
    int nline = 10;
    const char* file = nullptr;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            nline = atoi(argv[++i]);
            if (nline < 1) nline = 1;
            if (nline > 200) nline = 200;
        } else file = argv[i];
    }
    if (!file) { o.fl("usage: %s [-n N] <file>", tail ? "tail" : "head"); return RC_USE; }
    char p[400];
    norm(file, p, sizeof(p));
    struct stat st;
    if (stat(p, &st) != 0 || S_ISDIR(st.st_mode)) { o.fl("%s: no such file: %s", tail ? "tail" : "head", file); return RC_FAIL; }

    char* buf = (char*)sh_scratch(8192);
    if (!buf) { o.line("head/tail: no memory"); return RC_FAIL; }
    const long size = (long)st.st_size;
    const long off = (tail && size > 4096) ? size - 4096 : 0;
    const int n = read_span(p, off, 4096, buf, 8192);
    if (n <= 0) return RC_OK;

    int i = 0;
    if (tail && off > 0) {                       // 尾读的第一行可能是半行，丢掉
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
    }
    int starts[220];
    int cnt = 0;
    while (i < n && cnt < 220) {
        starts[cnt++] = i;
        while (i < n && buf[i] != '\n') i++;
        if (i < n) i++;
    }
    if (cnt == 0) return RC_OK;
    const int from = tail ? ((cnt > nline) ? cnt - nline : 0) : 0;
    const int to   = tail ? cnt : ((cnt > nline) ? nline : cnt);
    const int a = starts[from];
    const int b = (to < cnt) ? starts[to] : n;
    if (b > a) {
        o.raw(buf + a, b - a);
        if (buf[b - 1] != '\n') o.nl();
    }
    return RC_OK;
}
inline int cmd_head(Out& o, int argc, char** argv) { return head_tail(o, argc, argv, false); }
inline int cmd_tail(Out& o, int argc, char** argv) { return head_tail(o, argc, argv, true); }

inline int cmd_wc(Out& o, int argc, char** argv) {
    if (argc < 2) { o.line("usage: wc <file>"); return RC_USE; }
    char p[400];
    if (!norm(argv[1], p, sizeof(p))) { o.fl("wc: path outside /sdcard"); return RC_PATH; }
    FILE* f = fopen(p, "rb");
    if (!f) { o.fl("wc: no such file: %s", argv[1]); return RC_FAIL; }
    long bytes = 0, lines = 0;
    char* buf = (char*)sh_scratch(2048);
    if (!buf) { o.line("wc: no memory"); return RC_FAIL; }
    size_t n;
    while ((n = fread(buf, 1, 2048, f)) > 0) {
        bytes += (long)n;
        for (size_t i = 0; i < n; i++) if (buf[i] == '\n') lines++;
    }
    fclose(f);
    o.fl("%ld lines, %ld bytes", lines, bytes);
    return RC_OK;
}

inline int cmd_df(Out& o, int, char**) {
    uint64_t tot = 0, fr = 0;
    if (esp_vfs_fat_info(ROOT, &tot, &fr) != ESP_OK) { o.line("df: not mounted"); return RC_FAIL; }
    char a[16], b[16], u[16];
    human(a, sizeof(a), (long long)tot);
    human(b, sizeof(b), (long long)fr);
    human(u, sizeof(u), (long long)(tot - fr));
    o.fl("/sdcard  %s total · %s used · %s free (%d%%)",
         a, u, b, tot ? (int)((tot - fr) * 100 / tot) : 0);
    return RC_OK;
}

// ══════════════════════════════════════════════════════════════
//  命令：文件写
// ══════════════════════════════════════════════════════════════
inline bool wguard(Out& o, const char* p, const char* who) {
    if (readonly_path(p)) { o.fl("%s: read-only (apps/*.so): %s", who, p); return false; }
    return true;
}

inline int cmd_mkdir(Out& o, int argc, char** argv) {
    if (argc < 2) { o.line("usage: mkdir <dir>"); return RC_USE; }
    char p[400];
    if (!norm(argv[1], p, sizeof(p))) { o.fl("mkdir: path outside /sdcard"); return RC_PATH; }
    if (mkdir(p, 0777) != 0) { o.fl("mkdir: failed: %s", argv[1]); return RC_FAIL; }
    return RC_OK;
}

inline int rm_rec(const char* p) {
    DIR* d = opendir(p);
    if (!d) return unlink(p);
    struct dirent* de;
    bool blocked = false;
    while ((de = readdir(d)) != nullptr) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char full[480];
        snprintf(full, sizeof(full), "%s/%s", p, de->d_name);
        if (readonly_path(full)) { blocked = true; continue; }   // apps/*.so 递归里也不许删
        struct stat st;
        if (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) rm_rec(full);
        else unlink(full);
    }
    closedir(d);
    const int rc = rmdir(p);
    return (rc == 0 && !blocked) ? 0 : -1;
}

inline int cmd_rm(Out& o, int argc, char** argv) {
    bool rec = false;
    const char* t = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf") || !strcmp(argv[i], "-R")) rec = true;
        else t = argv[i];
    }
    if (!t) { o.line("usage: rm [-r] <path>"); return RC_USE; }
    char p[400];
    if (!norm(t, p, sizeof(p))) { o.fl("rm: path outside /sdcard: %s", t); return RC_PATH; }
    if (!strcmp(p, ROOT) || !strcmp(p, "/")) { o.fl("rm: refusing to remove root: %s", p); return RC_PATH; }
    if (!wguard(o, p, "rm")) return RC_PATH;

    struct stat st;
    if (stat(p, &st) != 0) { o.fl("rm: no such file: %s", t); return RC_FAIL; }
    if (S_ISDIR(st.st_mode)) {
        if (!rec) { o.fl("rm: %s is a directory (use -r)", t); return RC_USE; }
        if (rm_rec(p) != 0) { o.fl("rm: failed: %s", t); return RC_FAIL; }
    } else if (unlink(p) != 0) {
        o.fl("rm: failed: %s", t);
        return RC_FAIL;
    }
    return RC_OK;
}

inline int cmd_mv(Out& o, int argc, char** argv) {
    if (argc < 3) { o.line("usage: mv <src> <dst>"); return RC_USE; }
    char a[400], b[400];
    if (!norm(argv[1], a, sizeof(a)) || !norm(argv[2], b, sizeof(b))) { o.line("mv: path outside /sdcard"); return RC_PATH; }
    if (!wguard(o, a, "mv") || !wguard(o, b, "mv")) return RC_PATH;
    if (rename(a, b) != 0) { o.fl("mv: failed: %s -> %s", argv[1], argv[2]); return RC_FAIL; }
    return RC_OK;
}

inline int cmd_cp(Out& o, int argc, char** argv) {
    if (argc < 3) { o.line("usage: cp <src> <dst>"); return RC_USE; }
    char a[400], b[400];
    if (!norm(argv[1], a, sizeof(a)) || !norm(argv[2], b, sizeof(b))) { o.line("cp: path outside /sdcard"); return RC_PATH; }
    if (!wguard(o, b, "cp")) return RC_PATH;
    struct stat st;
    if (stat(a, &st) != 0 || S_ISDIR(st.st_mode)) { o.fl("cp: no such file: %s", argv[1]); return RC_FAIL; }
    if (st.st_size > 8 * 1024 * 1024) { o.fl("cp: too big (%ld bytes), limit 8MB", (long)st.st_size); return RC_FAIL; }
    FILE* in = fopen(a, "rb");
    if (!in) { o.fl("cp: cannot open: %s", argv[1]); return RC_FAIL; }
    FILE* out = fopen(b, "wb");
    if (!out) { fclose(in); o.fl("cp: cannot create: %s", argv[2]); return RC_FAIL; }
    char* buf = (char*)sh_scratch(4096);
    if (!buf) { o.line("cp: no memory"); return RC_FAIL; }
    size_t n;
    long total = 0;
    while ((n = fread(buf, 1, 4096, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); o.line("cp: write failed"); return RC_FAIL; }
        total += (long)n;
    }
    fclose(in);
    fclose(out);
    o.fl("copied %ld bytes", total);
    return RC_OK;
}

inline int cmd_write(Out& o, int argc, char** argv, bool append) {
    if (argc < 2) { o.fl("usage: %s <file> [text...]", append ? "append" : "write"); return RC_USE; }
    char p[400];
    if (!norm(argv[1], p, sizeof(p))) { o.line("write: path outside /sdcard"); return RC_PATH; }
    if (!wguard(o, p, append ? "append" : "write")) return RC_PATH;
    FILE* f = fopen(p, append ? "ab" : "wb");
    if (!f) { o.fl("write: cannot open: %s", argv[1]); return RC_FAIL; }
    for (int i = 2; i < argc; i++) {
        if (i > 2) fputc(' ', f);
        fputs(argv[i], f);
    }
    fputc('\n', f);
    fclose(f);
    o.fl("wrote %s", p);
    return RC_OK;
}

// ══════════════════════════════════════════════════════════════
//  命令：系统观察
// ══════════════════════════════════════════════════════════════
inline int cmd_free(Out& o, int, char**) {
    const size_t i_tot = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t i_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t i_big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    const size_t p_tot = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t p_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    o.fl("              total      free     free%%   largest");
    o.fl("internal   %7.1fK  %7.1fK   %4d%%  %6.1fK", i_tot / 1024.0, i_free / 1024.0,
         i_tot ? (int)(i_free * 100 / i_tot) : 0, i_big / 1024.0);
    o.fl("psram      %7.1fK  %7.1fK   %4d%%        -",
         p_tot / 1024.0, p_free / 1024.0, p_tot ? (int)(p_free * 100 / p_tot) : 0);
    return RC_OK;
}

inline int cmd_ps(Out& o, int, char**) {
    static TaskStatus_t st[24];
    const UBaseType_t n = uxTaskGetSystemState(st, 24, nullptr);
    // ★ 装不下时 FreeRTOS **一个都不填、返回 0**（不是返回总数）→ 别静默印"0 tasks"。
    if (n == 0) {
        o.fl("ps: 任务数超过表长(%d)，把 st[] 调大", (int)24);
        o.fl("-- %u tasks", (unsigned)uxTaskGetNumberOfTasks());
        return RC_FAIL;
    }
    o.fl("%-16s %-3s %-4s %-6s %s", "name", "pri", "core", "stack", "state");
    for (UBaseType_t i = 0; i < n; i++) {
        o.fl("%-16.16s %-3u %-4d %-6u %s", st[i].pcTaskName, (unsigned)st[i].uxCurrentPriority,
             (int)st[i].xCoreID, (unsigned)st[i].usStackHighWaterMark,
             st[i].eCurrentState == eRunning ? "run" : st[i].eCurrentState == eReady ? "ready" :
             st[i].eCurrentState == eBlocked ? "block" : st[i].eCurrentState == eSuspended ? "susp" : "del");
    }
    o.fl("-- %u tasks", (unsigned)n);
    return RC_OK;
}

inline int cmd_uptime(Out& o, int, char**) {
    const long long ms = esp_timer_get_time() / 1000;
    o.fl("up %lldh %02lldm %02llds", ms / 3600000, (ms / 60000) % 60, (ms / 1000) % 60);
    return RC_OK;
}

inline int cmd_date(Out& o, int, char**) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    char b[64];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tm);
    o.fl("%s %s", b, (tm.tm_year + 1900 < 2024) ? "(RTC 未校准)" : "CST");
    return RC_OK;
}

inline int cmd_ver(Out& o, int, char**) {
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    o.fl("papers3 · built %s %s", __DATE__, __TIME__);
    o.fl("chip esp32s3 · %d core(s) · rev %d · idf %s", ci.cores, ci.revision, IDF_VER);
    return RC_OK;
}

// log [app名或路径] [行数]
// ★ 默认读**当前 app 私有目录**的 log.txt（2026-10-20 起日志写 <appdir>/log.txt），
//   没有再退回老的 /sdcard/apps/reader.log。原来硬编码老路径 → 迁移后这条命令永远
//   报 "no reader.log"。
inline int cmd_log(Out& o, int argc, char** argv) {
    int nline = 20;
    const char* want = nullptr;
    for (int i = 1; i < argc; i++) {
        const char c0 = argv[i][0];
        if (c0 >= '0' && c0 <= '9') nline = atoi(argv[i]);
        else want = argv[i];
    }
    if (nline < 1) nline = 1;
    if (nline > 100) nline = 100;

    char path[400];
    path[0] = 0;
    if (want) {
        if (strchr(want, '/')) norm(want, path, sizeof(path));            // 给的是路径
        else snprintf(path, sizeof(path), "/sdcard/apps/%.80s/log.txt", want);  // 给的是 app 名
    } else {
        char ad[120];
        if (so_app_dir(ad, sizeof(ad)) > 0) {
            struct stat st0;
            if (stat(ad, &st0) == 0) snprintf(path, sizeof(path), "%.100s/log.txt", ad);
        }
        if (!path[0]) snprintf(path, sizeof(path), "/sdcard/apps/reader/log.txt");
    }

    struct stat st;
    if (stat(path, &st) != 0) { o.fl("log: no such file: %s", path); return RC_FAIL; }
    char* buf = (char*)sh_scratch(4096);
    if (!buf) { o.line("log: no memory"); return RC_FAIL; }
    long off = (long)st.st_size - 4096;
    if (off < 0) off = 0;
    const int n = read_span(path, off, 4096, buf, 4096);
    if (n <= 0) { o.line("log: empty"); return RC_OK; }
    int total = 0;
    for (int i = 0; i < n; i++) if (buf[i] == '\n') total++;
    int skip = (total > nline) ? total - nline : 0;
    int from = 0, seen = 0;
    for (; from < n; from++) { if (buf[from] == '\n' && ++seen == skip) { from++; break; } }
    o.raw(buf + from, n - from);
    if (buf[n - 1] != '\n') o.nl();
    return RC_OK;
}

// ══════════════════════════════════════════════════════════════
//  命令：网络 / 配置 / 电源
// ══════════════════════════════════════════════════════════════
inline const char* net_state_str(int s) {
    return s == 0 ? "idle" : s == 1 ? "connecting" : s == 2 ? "connected" : "failed";
}

inline int cmd_ip(Out& o, int, char**) {
    char ap[8];
    snprintf(ap, sizeof(ap), "%s", wifi_ap::g_running ? "on" : "off");
    o.fl("sta  %s (%s)", wifi_sta::g_ip[0] ? wifi_sta::g_ip : "-", net_state_str(wifi_sta::g_state));
    o.fl("ap   %s", ap);
    o.fl("web  %s", web_files::running() ? "on" : "off");
    return RC_OK;
}

inline int cmd_wifi(Out& o, int argc, char** argv) {
    const char* sub = (argc > 1) ? argv[1] : nullptr;
    if (!sub) {
        o.fl("state %s", net_state_str(wifi_sta::g_state));
        o.fl("ssid  %s", wifi_sta::g_ssid[0] ? wifi_sta::g_ssid : "-");
        o.fl("ip    %s", wifi_sta::g_ip[0] ? wifi_sta::g_ip : "-");
        o.fl("ap    %s", wifi_ap::g_running ? "on" : "off");
        return RC_OK;
    }
    if (!strcmp(sub, "scan")) {
        wifi_scan_config_t cfg{};
        if (esp_wifi_scan_start(&cfg, true) != ESP_OK) { o.line("wifi: scan failed"); return RC_FAIL; }
        uint16_t num = 8;
        static wifi_ap_record_t rec[8];
        if (esp_wifi_scan_get_ap_records(&num, rec) != ESP_OK) { o.line("wifi: no result"); return RC_FAIL; }
        for (int i = 0; i < num; i++) o.fl("%4d dBm  %s", rec[i].rssi, (const char*)rec[i].ssid);
        o.fl("-- %u ap(s)", (unsigned)num);
        return RC_OK;
    }
    if (!strcmp(sub, "on") || !strcmp(sub, "off")) {
        const bool on = !strcmp(sub, "on");
        if (!wifi_ap::set_ap(on)) { o.fl("wifi: cannot turn %s", on ? "on" : "off"); return RC_FAIL; }
        cfg::g_cfg.ap = on ? 1 : 0;
        o.fl("ap %s", on ? "on" : "off");
        return RC_OK;
    }
    if (!strcmp(sub, "conn")) {
        if (argc < 3) { o.line("usage: wifi conn <ssid> [pass]"); return RC_USE; }
        const char* pass = (argc > 3) ? argv[3] : "";
        if (!wifi_sta::connect(argv[2], pass)) { o.fl("wifi: cannot connect %s", argv[2]); return RC_FAIL; }
        o.fl("connecting to %s…（2~5 秒后用 wifi 看结果）", argv[2]);
        return RC_OK;
    }
    o.fl("wifi: unknown subcommand: %s", sub);
    return RC_CMD;
}

inline void cfg_dump(Out& o) {
    auto& c = cfg::g_cfg;
    o.fl("size=%d ink=%d bw=%d idle=%d ap=%d web=%d wifi=%d", c.size, c.ink, c.bw, c.idle, c.ap, c.web, c.wifi);
    o.fl("wave_def=%d wave_sel=%d ss=%d dp=%d snd=%d snd_vol=%d",
         c.wave_def, c.wave_sel, c.ss, c.dp, c.snd, c.snd_vol);
    o.fl("font=%s", c.font[0] ? c.font : "(builtin)");
    o.fl("wifi_ssid=%s", c.wifi_ssid[0] ? c.wifi_ssid : "(none)");
    o.fl("snd_file=%s", c.snd_file[0] ? c.snd_file : "(beep)");
    o.fl("scr_img=%s", c.scr_img[0] ? c.scr_img : "(none)");
}

inline int cmd_cfg(Out& o, int argc, char** argv) {
    const char* sub = (argc > 1) ? argv[1] : nullptr;
    if (!sub) { cfg_dump(o); return RC_OK; }
    if (!strcmp(sub, "get")) {
        if (argc < 3) { o.line("usage: cfg get <key>"); return RC_USE; }
        const char* k = argv[2];
        // 复用 plugin.cpp 的键表太长，这里直接读结构体（键名与 NVS 一致）
        auto& c = cfg::g_cfg;
        if (!strcmp(k, "size")) o.fl("%d", c.size);
        else if (!strcmp(k, "ink")) o.fl("%d", c.ink);
        else if (!strcmp(k, "bw")) o.fl("%d", c.bw);
        else if (!strcmp(k, "idle")) o.fl("%d", c.idle);
        else if (!strcmp(k, "ap")) o.fl("%d", c.ap);
        else if (!strcmp(k, "web")) o.fl("%d", c.web);
        else if (!strcmp(k, "wifi")) o.fl("%d", c.wifi);
        else if (!strcmp(k, "wave_def")) o.fl("%d", c.wave_def);
        else if (!strcmp(k, "wave_sel")) o.fl("%d", c.wave_sel);
        else if (!strcmp(k, "ss")) o.fl("%d", c.ss);
        else if (!strcmp(k, "dp")) o.fl("%d", c.dp);
        else if (!strcmp(k, "snd")) o.fl("%d", c.snd);
        else if (!strcmp(k, "snd_vol")) o.fl("%d", c.snd_vol);
        else if (!strcmp(k, "font")) o.fl("%s", c.font);
        else if (!strcmp(k, "wifi_ssid")) o.fl("%s", c.wifi_ssid);
        else if (!strcmp(k, "snd_file")) o.fl("%s", c.snd_file);
        else if (!strcmp(k, "scr_img")) o.fl("%s", c.scr_img);
        else { o.fl("cfg: unknown key: %s", k); return RC_CMD; }
        return RC_OK;
    }
    if (!strcmp(sub, "set")) {
        if (argc < 4) { o.line("usage: cfg set <key> <value>"); return RC_USE; }
        const char* k = argv[2];
        const char* v = argv[3];
        auto& c = cfg::g_cfg;
        const int iv = atoi(v);
        if (!strcmp(k, "size")) c.size = (uint16_t)(iv < 16 ? 16 : iv > 48 ? 48 : iv);
        else if (!strcmp(k, "ink")) c.ink = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
        else if (!strcmp(k, "bw")) c.bw = (uint8_t)(iv < 1 ? 1 : iv > 8 ? 8 : iv);
        else if (!strcmp(k, "idle")) c.idle = (uint16_t)(iv < 1 ? 1 : iv > 120 ? 120 : iv);
        else if (!strcmp(k, "ap")) c.ap = iv ? 1 : 0;
        else if (!strcmp(k, "web")) c.web = iv ? 1 : 0;
        else if (!strcmp(k, "wifi")) c.wifi = iv ? 1 : 0;
        else if (!strcmp(k, "wave_def")) c.wave_def = iv ? 1 : 0;
        else if (!strcmp(k, "wave_sel")) c.wave_sel = (uint8_t)(iv < 1 ? 1 : iv > 5 ? 5 : iv);
        else if (!strcmp(k, "ss")) c.ss = iv ? 1 : 0;
        else if (!strcmp(k, "dp")) c.dp = iv ? 1 : 0;
        else if (!strcmp(k, "snd")) c.snd = iv ? 1 : 0;
        else if (!strcmp(k, "snd_vol")) c.snd_vol = (uint8_t)(iv < 0 ? 0 : iv > 100 ? 100 : iv);
        else if (!strcmp(k, "font")) snprintf(c.font, sizeof(c.font), "%s", v);
        else if (!strcmp(k, "wifi_ssid")) snprintf(c.wifi_ssid, sizeof(c.wifi_ssid), "%s", v);
        else if (!strcmp(k, "snd_file")) snprintf(c.snd_file, sizeof(c.snd_file), "%s", v);
        else if (!strcmp(k, "scr_img")) snprintf(c.scr_img, sizeof(c.scr_img), "%s", v);
        else { o.fl("cfg: unknown key: %s", k); return RC_CMD; }
        o.fl("%s = %s  (敲 cfg save 落盘)", k, v);
        return RC_OK;
    }
    if (!strcmp(sub, "save")) {
        if (!cfg::save()) { o.line("cfg: save failed"); return RC_FAIL; }
        o.line("cfg: saved to NVS");
        return RC_OK;
    }
    if (!strcmp(sub, "reset")) {
        cfg::g_cfg = cfg::Config{};
        cfg::save();
        o.line("cfg: factory defaults（重启后生效）");
        return RC_OK;
    }
    o.fl("cfg: unknown subcommand: %s", sub);
    return RC_CMD;
}

inline void reboot_cb(void*) { esp_restart(); }

inline int cmd_apps(Out& o, int, char**) {
    char names[8][64];
    const int n = so_list_files("/sdcard/apps", ".so", (char*)names, 64, 8);
    for (int i = 0; i < n; i++) o.fl("  %s", names[i]);
    o.fl("-- %d app(s)", n);
    return RC_OK;
}

inline int cmd_launch(Out& o, int argc, char** argv) {
    if (argc < 2) { o.line("usage: launch <name>   （不带 .so，敲 apps 看有哪些）"); return RC_USE; }
    char f[96];
    so_snprintf(f, sizeof(f), "%s%s", argv[1], (strstr(argv[1], ".so") ? "" : ".so"));
    o.fl("打开 %s …", f);
    const int r = so_app_launch_internal(f);
    if (r != 0) o.fl("失败（r=%d，看串口）", r);
    return r == 0 ? RC_OK : RC_FAIL;
}

// 关掉当前 .so app 回启动器（等价于点状态栏左上角「< EXIT」）：
// 现场／远程都能用，也给退出路径的耗时探针留个入口。
inline int cmd_close(Out& o, int argc, char** argv) {
    (void)argc; (void)argv;
    o.line("关闭当前 app …");
    so_exit();                 // 和状态栏「< EXIT」完全同一条路径（含耗时探针）
    return RC_OK;
}

// 把当前帧缓冲整屏导出成 PGM（P5，540×960，8bit 灰度）—— 远程排查绘制问题用：
//   shot [path]        默认 /sdcard/shot.pgm，然后用 sercli get 拿回来看
inline int cmd_shot(Out& o, int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "/sdcard/shot.pgm";
    FILE* f = fopen(path, "wb");
    if (!f) { o.fl("打不开 %s", path); return RC_FAIL; }
    fprintf(f, "P5\n540 960\n255\n");
    static uint8_t line[540];
    lgfx::v1::pixelcopy_t pc(nullptr, lgfx::v1::grayscale_t::depth,
                             lgfx::v1::grayscale_t::depth, false, nullptr);
    M5.Display.startWrite();
    for (int y = 0; y < 960; ++y) {
        M5.Display.getPanel()->readRect(0, y, 540, 1, line, &pc);
        fwrite(line, 1, 540, f);
    }
    M5.Display.endWrite();
    fclose(f);
    o.fl("%s 已写（540×960 PGM）", path);
    return RC_OK;
}

static void ime_done_cb(const char* t) { ESP_LOGI("SH", "IME 完成: %s", t ? t : ""); }
static void ime_cancel_cb()            { ESP_LOGI("SH", "IME 取消"); }

#include "text/ttf.h"
// 把键盘键面用到的字形**用设备自己的光栅器烤一遍**，导出成文本（PC 侧再转成 C 数组）：
//   bakekb [px]        → /sdcard/kb_glyphs.txt
// 记录格式（每字形一段）：
//   cp <hex> w <w> h <h> xo <xo> yo <yo> adv <adv> n <bytes>
//   <hex 字节（每行 32 个）...>
inline int cmd_bakekb(Out& o, int argc, char** argv) {
    int px = 34;                                   // = widgets::KB_FONT_PX
    if (argc >= 2) { int v = atoi(argv[1]); if (v >= 12 && v <= 64) px = v; }
    FILE* f = fopen("/sdcard/kb_glyphs.txt", "w");
    if (!f) { o.line("打不开 /sdcard/kb_glyphs.txt"); return RC_FAIL; }
    const float keep = papers3::ttf::size_px();
    papers3::ttf::set_size((float)px);
    fprintf(f, "# px %d\n", px);
    int n_g = 0;
    // ASCII 全段 + 键面用到的两个箭头（0x2190 ←、0x2192 →）
    const int32_t extra[] = {0x2190, 0x2192};
    for (int k = 0; k < 97; ++k) {
        const int32_t cp = (k < 95) ? (0x20 + k) : extra[k - 95];
        auto g = papers3::ttf::rasterize(cp);
        fprintf(f, "cp %x w %d h %d xo %d yo %d adv %d n %d\n",
                (unsigned)cp, g.w, g.h, g.xoff, g.yoff, g.advance, g.w * g.h);
        for (int i = 0; i < g.w * g.h; ++i) {
            fprintf(f, "%02x%s", g.gray ? g.gray[i] : 0, ((i + 1) % 32 == 0) ? "\n" : " ");
        }
        if ((g.w * g.h) % 32) fprintf(f, "\n");
        papers3::ttf::free_glyph(g);
        n_g++;
    }
    papers3::ttf::set_size(keep);
    fclose(f);
    o.fl("烤好 %d 个字形（%dpx）→ /sdcard/kb_glyphs.txt", n_g, px);
    return RC_OK;
}

inline int cmd_ime(Out& o, int, char**) {
    const int r = so_ime_open("", ime_done_cb, ime_cancel_cb);
    o.fl("so_ime_open → %d（0 = 已打开）", r);
    return r == 0 ? RC_OK : RC_FAIL;
}

inline int cmd_reboot(Out& o, int*, int* flags) {
    o.line("rebooting…");
    if (flags) *flags |= FLAG_REBOOT;
    // 800ms 后真重启：留出推屏时间，别让屏幕停在半句话上
    esp_timer_handle_t t = nullptr;
    esp_timer_create_args_t a{};
    a.callback = reboot_cb;
    a.name = "sh_reboot";
    if (esp_timer_create(&a, &t) == ESP_OK) esp_timer_start_once(t, 800 * 1000);
    return RC_OK;
}

// ══════════════════════════════════════════════════════════════
//  命令表
// ══════════════════════════════════════════════════════════════
struct Cmd {
    const char* name;
    const char* usage;
    const char* help;
    int (*fn)(Out&, int, char**);
};

// reboot 的签名多两个指针，单独包一层
inline int cmd_reboot_wrap(Out& o, int argc, char** argv) {
    (void)argc; (void)argv;
    return cmd_reboot(o, nullptr, nullptr);
}

inline const Cmd kCmds[] = {
    {"pwd",     "pwd",                      "print working directory",              cmd_pwd},
    {"cd",      "cd [dir]",                 "change directory",                     cmd_cd},
    {"ls",      "ls [dir]",                 "list a directory (size + time)",        cmd_ls},
    {"cat",     "cat <file>",               "print a text file (max 4KB)",          cmd_cat},
    {"head",    "head [-n N] <file>",        "first N lines (default 10)",           cmd_head},
    {"tail",    "tail [-n N] <file>",        "last N lines (default 10)",            cmd_tail},
    {"wc",      "wc <file>",                "lines / bytes",                        cmd_wc},
    {"df",      "df",                       "SD card space",                        cmd_df},
    {"mkdir",   "mkdir <dir>",              "create a directory",                   cmd_mkdir},
    {"rm",      "rm [-r] <path>",           "remove a file / directory (-r)",       cmd_rm},
    {"mv",      "mv <src> <dst>",           "rename / move",                        cmd_mv},
    {"cp",      "cp <src> <dst>",           "copy a file (max 8MB)",                cmd_cp},
    {"write",   "write <file> [text...]",   "overwrite a text file",                [](Out& o, int c, char** v) { return cmd_write(o, c, v, false); }},
    {"append",  "append <file> [text...]",  "append a line to a text file",         [](Out& o, int c, char** v) { return cmd_write(o, c, v, true); }},
    {"free",    "free",                     "internal / PSRAM memory",              cmd_free},
    {"ps",      "ps",                       "task list",                            cmd_ps},
    {"uptime",  "uptime",                   "time since boot",                      cmd_uptime},
    {"date",    "date",                     "current time",                         cmd_date},
    {"ver",     "ver",                      "firmware / chip info",                 cmd_ver},
    {"log",     "log [n]",                  "tail /sdcard/apps/reader.log",         cmd_log},
    {"wifi",    "wifi [scan|on|off|conn <ssid> [pass]]", "wifi status / scan / ap / connect", cmd_wifi},
    {"ip",      "ip",                       "network status",                       cmd_ip},
    {"cfg",     "cfg [get|set|save|reset] ...", "firmware config",                  cmd_cfg},
    {"reboot",  "reboot",                   "restart the device",                   cmd_reboot_wrap},
    {"apps",    "apps",                     "list /sdcard/apps/*.so",                cmd_apps},
    {"ime",     "ime",                      "诊断：直接拉起输入法覆盖层",             cmd_ime},
    {"bakekb",  "bakekb [px]",              "烤键盘字形导出（给 PC 转 C 数组）",       cmd_bakekb},
    {"launch",  "launch <name>",            "open an app (name without .so)",        cmd_launch},
    {"close",   "close",                    "close current app → launcher",          cmd_close},
    {"shot",    "shot [path]",              "导出整屏帧缓冲（PGM，给 sercli get）",     cmd_shot},
};

inline const Cmd* find_cmd(const char* name) {
    for (auto& c : kCmds) if (!strcmp(c.name, name)) return &c;
    return nullptr;
}

// ══════════════════════════════════════════════════════════════
//  解析 + 分派
// ══════════════════════════════════════════════════════════════
inline int tokenize(char* s, char* argv[], int max) {
    int n = 0;
    char* p = s;
    while (*p && n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (*p == '"') {                        // "带空格的参数"
            p++;
            argv[n++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') { *p = 0; p++; }
        } else {
            argv[n++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) { *p = 0; p++; }
        }
    }
    return n;
}

inline void usage(Out& o, const char* line) {
    char t[256];
    snprintf(t, sizeof(t), "%s", line);
    char* argv[8];
    const int n = tokenize(t, argv, 8);
    if (n <= 0) { o.line("sh: empty command"); return; }
    const Cmd* c = find_cmd(argv[0]);
    if (!c) { o.fl("sh: unknown command: %s  (敲 help)", argv[0]); return; }
    o.fl("usage: %s", c->usage);
    o.fl("       %s", c->help);
}

inline void print_help(Out& o) {
    o.line("papers3 shell · 命令一览（都是真的在这台机器上跑）");
    o.line("");
    o.line("文件   pwd cd ls cat head tail wc df mkdir rm mv cp write append");
    o.line("系统   free ps uptime date cpu ver log");
    o.line("网络   wifi [scan|on|off|conn]  ip");
    o.line("配置   cfg [get <k>|set <k> <v>|save|reset]");
    o.line("应用   apps  launch <name>");
    o.line("电源   reboot");
    o.line("界面   help clear exit history");
    o.line("");
    o.line("/apps 这类简写会自动补成 /sdcard/apps；写操作只在 /sdcard 里，apps/*.so 只读");
}

// ══════════════════════════════════════════════════════════════
//  跨核互斥
//  ★ sh 是**有状态**的：g_cwd + sh_scratch() 单例缓冲。而两个核都会调进来 ——
//    core1 = 终端 app（.so 的 so_sh_exec），core0 = sercli 串口命令行任务。
//    没有互斥时两边同时敲命令会互相踩（cp/cat 读到别人的缓冲、cd 改错目录）。
//    拿不到锁就明确失败（3s 超时），不要把 UI 核挂在那儿等。
// ══════════════════════════════════════════════════════════════
inline SemaphoreHandle_t lock() {
    static SemaphoreHandle_t m = xSemaphoreCreateMutex();
    return m;
}
struct LockGuard {
    bool held = false;
    LockGuard() {
        SemaphoreHandle_t m = lock();
        if (m) held = (xSemaphoreTake(m, pdMS_TO_TICKS(3000)) == pdTRUE);
    }
    ~LockGuard() { if (held) xSemaphoreGive(lock()); }
};

// 入口
inline int exec(const char* line_in, char* outbuf, int cap, int* code, int* flags) {
    if (code) *code = RC_OK;
    if (flags) *flags = 0;
    Out o;
    o.b = outbuf;
    o.cap = cap;
    if (outbuf && cap > 0) outbuf[0] = 0;

    LockGuard lk;
    if (!lk.held) { o.line("sh: busy（另一个命令正在跑，稍后重试）"); if (code) *code = RC_FAIL; return o.n; }

    char line[512];
    snprintf(line, sizeof(line), "%s", line_in ? line_in : "");
    // 去首尾空白
    char* s = line;
    while (*s == ' ' || *s == '\t') s++;
    int L = (int)strlen(s);
    while (L > 0 && (s[L - 1] == ' ' || s[L - 1] == '\t' || s[L - 1] == '\r' || s[L - 1] == '\n')) s[--L] = 0;
    if (!L) return o.n;

    char* argv[12];
    const int argc = tokenize(s, argv, 12);
    if (argc <= 0) return o.n;

    // reboot 要拿到 flags 指针，单独处理
    if (!strcmp(argv[0], "reboot")) {
        if (code) *code = cmd_reboot(o, nullptr, flags);
        return o.n;
    }

    const Cmd* c = find_cmd(argv[0]);
    if (!c) { usage(o, s); if (code) *code = RC_CMD; return o.n; }

    const int rc = c->fn(o, argc, argv);
    if (code) *code = rc;
    if (o.cut) o.fl("… (输出被截断，cap=%d)", cap);
    return o.n;
}

} // namespace papers3::sh
