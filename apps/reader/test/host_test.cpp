// apps/reader/test/host_test.cpp — 在电脑上验证排版引擎（窗口 + 分页 + 目录跳章）
//
// 跑法：apps/reader/test/run.sh [书文件名...]（默认三本真书）
// 验三件事：
//   ① 全书顺序翻页 → 所有页面画出来的文字**去掉空白后**必须与原文件逐一对应（不漏行、不重行）
//   ② 从中间页起点续读（= 历史阅读 / 目录跳章的动作）→ 接着的文本必须接得上
//   ③ 分章索引里前 N 章逐个跳 → 页面第一段文字必须就是那一章的标题
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <string>
#include <vector>
#include <algorithm>
#include "../main/layout.h"

extern void collect_reset();
extern void collect_stop();
extern const char* collect_buf();
extern long collect_len();
extern void stub_set_book_dir(const char* d);
extern void* fake_font(float px);

using namespace app;

static int g_fail = 0;
static FILE* g_dump = nullptr;      // PAGE_DUMP=1 → build/pages.txt（逐页：序号 偏移 行号 字数 前 60 字）


// 去掉 ASCII 空白（折行会在行尾/行首吃掉空格，比对时两边都去）
static std::string squash(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); ) {
        const unsigned char c = (unsigned char)s[i];
        if (c > 0x20) {
            if (i + 3 <= s.size() && c == 0xE3 && (unsigned char)s[i+1] == 0x80 && (unsigned char)s[i+2] == 0x80) {
                i += 3; continue;                 // 全角空格：分章标题会被 trim 掉，不算内容
            }
            o.push_back((char)c); i++;
        } else i++;
    }
    return o;
}

static std::string read_file(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { printf("  ✗ 打不开 %s\n", path.c_str()); return std::string(); }
    std::string s;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    return s;
}

// 定位第一处不同，打印上下文（「看起来一样但后面差一个字」这类最难查）
static void report_diff(const std::string& ref, const std::string& got) {
    const size_t lim = std::min(ref.size(), got.size());
    size_t i = 0;
    while (i < lim && ref[i] == got[i]) i++;
    if (i == lim && ref.size() == got.size()) return;
    g_fail++;
    printf("  ✗ 文本不一致：ref %zu 字 / got %zu 字，首个不同在第 %zu 字\n",
           ref.size(), got.size(), i);
    const size_t a = i > 60 ? i - 60 : 0;
    printf("     ref[%zu..]: %.170s\n", a, ref.substr(a, 120).c_str());
    printf("     got[%zu..]: %.170s\n", a, got.substr(a, 120).c_str());
}

static void set_book(const std::string& name, int size) {
    ui::scopy(rd::g_name, name.c_str(), sizeof(rd::g_name));
    rd::g_fsize = (uint32_t)size;
    rd::g_md = false;
    rd::g_win_off = 0; rd::g_win_len = 0; rd::g_win_eof = true;
}

static void test_sequential(const std::string& name, const std::string& dir, int sz) {
    printf("① 全书顺序翻页\n");
    set_book(name, sz);
    rd::pages_reset(0, 0);
    if (!rd::win_load_at(0)) { printf("  ✗ 窗口装载失败\n"); g_fail++; return; }

    std::string all;
    int pages = 0;
    uint32_t last_off = 0;
    bool mono = true;
    const auto t0 = std::chrono::steady_clock::now();
    while (true) {
        rd::ensure_page(pages);
        if (rd::g_ps_n <= pages) break;                 // 到底了
        collect_reset();
        rd::paint_page(pages);
        collect_stop();
        if (g_dump) fprintf(g_dump, "%d\t%u\t%d\t%d\t%.9000s\n", pages, rd::g_ps[pages].off,
                            rd::g_ps[pages].li, (int)collect_len(), collect_buf());
        all += collect_buf();
        if (rd::g_ps[pages].off < last_off) mono = false;
        last_off = rd::g_ps[pages].off;
        pages++;
        if (pages > 300000) { printf("  ✗ 页数失控（>30 万）→ 分页不收敛\n"); g_fail++; break; }
    }
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    // 调试：把最后几页用到的窗口与块表摊出来（排查"页面里出现的偏移到底是哪来的"）
    if (getenv("DUMP_WIN")) {
        printf("   [窗口] off=%u len=%d eof=%d 块数=%d\n",
               rd::g_win_off, rd::g_win_len, (int)rd::g_win_eof, rd::g_doc.n);
        for (int i = rd::g_doc.n - 8; i < rd::g_doc.n; i++) {
            if (i < 0) continue;
            printf("   [块] bi=%d off=%u kind=%d text=%.40s\n",
                   i, rd::g_win_off + rd::g_doc.blk[i].off, rd::g_doc.blk[i].kind,
                   rd::g_doc.blk[i].text ? rd::g_doc.blk[i].text : "(null)");
        }
        for (int i = rd::g_ps_n - 4; i < rd::g_ps_n; i++)
            if (i >= 0) printf("   [页] %d off=%u li=%d\n", i, rd::g_ps[i].off, rd::g_ps[i].li);
    }

    const std::string ref = squash(read_file(dir + "/" + name));
    const std::string got = squash(all);
    printf("   页数 %d · 排完 %lldms · 页起点单调 %s · 文字 ref %zu / got %zu 字\n",
           pages, ms, mono ? "OK" : "✗", ref.size(), got.size());
    if (!mono) g_fail++;
    report_diff(ref, got);
    if (ref == got) printf("   ✓ 整本文字逐字对上（无漏行 / 无重行）\n");
}

static void test_resume(const std::string& name, const std::string& dir, int sz, int page_from) {
    printf("② 从中间页（第 %d 页）起点续读\n", page_from + 1);
    set_book(name, sz);
    rd::pages_reset(0, 0);
    rd::win_load_at(0);
    while (rd::g_ps_n <= page_from && !rd::g_ps_end) rd::ensure_page(rd::g_ps_n);
    if (rd::g_ps_n <= page_from) { printf("   本书不够 %d 页，跳过\n", page_from); return; }
    // 只比「块首页」（li=0）：块中间那一页本来就从半句开始，拿文件偏移对齐必然对不上
    while (rd::g_ps[page_from].li != 0) {
        page_from++;
        while (rd::g_ps_n <= page_from && !rd::g_ps_end) rd::ensure_page(rd::g_ps_n);
        if (rd::g_ps_n <= page_from) break;
    }

    const rd::PageStart ps = rd::g_ps[page_from];
    rd::pages_reset(ps.off, ps.li);                     // ← 历史阅读 / 跳章就是这么干的
    std::string got;
    for (int k = 0; k < 30; k++) {
        rd::ensure_page(k);
        if (rd::g_ps_n <= k) break;
        collect_reset();
        rd::paint_page(k);
        collect_stop();
        got += collect_buf();
    }
    const std::string whole = read_file(dir + "/" + name);
    if ((size_t)ps.off >= whole.size()) { printf("  ✗ 偏移越界\n"); g_fail++; return; }
    const std::string tail = squash(std::string(whole, ps.off));
    const std::string a = tail.substr(0, std::min((size_t)4000, tail.size()));
    const std::string b = squash(got).substr(0, std::min((size_t)4000, squash(got).size()));
    if (a == b) printf("   ✓ 续读接得上（比对前 %zu 字，起点偏移 %u）\n", a.size(), ps.off);
    else        report_diff(a, b);
}

static void test_toc_jump(const std::string& name, const std::string& dir, int sz) {
    printf("③ 目录跳章（分章索引抽查 20 条）\n");
    chap::start(name.c_str(), sz);                      // 起扫描（走和真机同一套规则）
    int guard = 0;
    while (chap::running() && guard++ < 400000) chap::step();
    const int n = chap::count();
    printf("   索引 %d 项（卷%d 章%d 节%d 特殊%d）\n",
           n, chap::lv(1), chap::lv(2), chap::lv(3), chap::lv(4));
    if (n <= 0) { printf("   （这本判定为不分章，跳过）\n"); return; }
    (void)dir;

    const int step = n > 200 ? n / 20 : 1;
    int bad = 0, checked = 0;
    for (int i = 0; i < n && checked < 20; i += step, checked++) {
        const int lv = chap::title_level(i);
        char t[128] = {0};
        chap::read_title(i, t, sizeof(t) - 1);
        const std::string want = squash(std::string(t));
        if (want.empty()) continue;
        set_book(name, sz);
        rd::pages_reset(chap::list()[i].off, 0);        // ← 目录里点一条就是这么干的
        collect_reset();
        rd::paint_page(0);
        collect_stop();
        const std::string head = squash(collect_buf());
        const std::string got = head.substr(0, want.size());
        if (got != want) {
            bad++;
            printf("   ✗ 第 %d 条（L%d）跳错：想要「%s」，页首是「%.60s」\n",
                   i + 1, lv, want.c_str(), head.substr(0, 60).c_str());
        }
    }
    if (bad == 0) printf("   ✓ 抽查 %d 章全部跳到章首（页首文字 = 该章标题）\n", checked);
    else          g_fail += bad;
}

int main(int argc, char** argv) {
    const char* dir = getenv("BOOK_DIR") ? getenv("BOOK_DIR") : "books";
    stub_set_book_dir(dir);
    ui::g_font = fake_font((float)rd::g_size);
    rd::g_repaint_hook = []() { collect_reset(); };   // 重画整页 → 收集器重新开始
    if (getenv("PAGE_DUMP")) g_dump = fopen("build/pages.txt", "w");

    std::vector<std::string> books;
    for (int i = 1; i < argc; i++) books.push_back(argv[i]);
    if (books.empty()) books = { "玄幻世界捡垃圾x.txt" };

    for (auto& b : books) {
        printf("═══════ %s ═══════\n", b.c_str());
        const int sz = so_book_size(b.c_str());
        if (sz <= 0) { printf("  ✗ 读不到文件（BOOK_DIR=%s）\n", dir); g_fail++; continue; }
        test_sequential(b, dir, sz);
        test_resume(b, dir, sz, 500);
        test_toc_jump(b, dir, sz);
    }
    printf("\n%s（失败项 %d）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
