// tools/test_chapter_classify.cpp — 分章规则离线回归
//
// 把 .so 里那份 `app::chap::classify()`（apps/reader/main/chapter.h）**原样**拉出来，
// 对真书逐行跑一遍，输出格式对齐 `tools/scan_chapters.py --full`，用来逐条 diff。
//
// 为什么值得做：规则是三本书 3132 章核对过的，C++ 这版是照抄的 —— 抄错了
// 只能在设备上表现为"目录少了几章"，很难查。这里在 PC 上就能冻结住。
//
// 编译运行（见 tools/test_chapter_classify.sh）：
//   g++ -O2 -std=gnu++17 -I apps/reader/main tools/test_chapter_classify.cpp -o /tmp/tcc
//   /tmp/tcc books/*.txt > /tmp/cpp.txt
//   python3 tools/scan_chapters.py --full books/*.txt > /tmp/py.txt
//   diff /tmp/py.txt /tmp/cpp.txt
//
// ★ 只调 classify()（纯逻辑），不碰扫描器 —— 那些 inline 函数没被调用就不会被
//   生成，所以不需要 stub 任何 so_* 外部符号。（chapter.h 里只有声明。）

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include "../apps/reader/main/chapter.h"

static app::chap::Rules R;

static void base_name(const char* p, char* out, int cap) {
    const char* b = p;
    for (const char* q = p; *q; ++q) if (*q == '/') b = q + 1;
    int i = 0;
    for (; b[i] && i < cap - 1; ++i) out[i] = b[i];
    out[i] = 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "用法: %s BOOK...\n", argv[0]); return 2; }

    for (int a = 1; a < argc; ++a) {
        FILE* f = fopen(argv[a], "rb");
        if (!f) { fprintf(stderr, "打不开 %s\n", argv[a]); continue; }

        // 逐行读：与 Python `for raw in f` 同语义（按 '\n' 切，行含行尾）
        static char line[65536];
        static unsigned lv_n[8] = {0};
        for (int i = 0; i < 8; ++i) lv_n[i] = 0;
        static char title_pool[8192][96];
        static int  title_lv[8192];
        static unsigned title_off[8192];
        static int  title_bytes[8192];
        int nt = 0;

        uint32_t off = 0;
        uint32_t bytes = 0;
        while (fgets(line, sizeof(line), f)) {
            const int len = (int)strlen(line);          // 含行尾 '\n'（若有）
            const char* title = nullptr;
            int tlen = 0;
            const int lv = app::chap::classify(line, len, R, title, tlen);
            if (lv > 0) {
                if (lv < 8) lv_n[lv]++;
                if (nt < 8192) {
                    int cut = app::chap::u8n(title, tlen);      // 与 Python 一致：按字符截断到 34
                    if (cut > 34) cut = 34;
                    int b = 0;
                    for (int i = 0; i < cut; ++i) b += app::chap::u8l(title + b);
                    if (b > 95) b = 95;
                    memcpy(title_pool[nt], title, (size_t)b); title_pool[nt][b] = 0;
                    title_lv[nt] = lv; title_off[nt] = off; title_bytes[nt] = b;
                    nt++;
                }
            }
            off += (uint32_t)len;
            bytes = off;
        }
        fclose(f);

        char nm[128]; base_name(argv[a], nm, sizeof(nm));
        printf("====================================================================\n");
        printf("%s  %u.%uMB  卷%u 章%u 节%u 特殊%u  坏字节行0\n",
               nm, (unsigned)(bytes / 1048576), (unsigned)((bytes % 1048576) * 10 / 1048576),
               lv_n[1], lv_n[2], lv_n[3], lv_n[4]);
        for (int i = 0; i < nt; ++i)
            printf("   L%d off=%9u  %s\n", title_lv[i], title_off[i], title_pool[i]);
    }
    return 0;
}
