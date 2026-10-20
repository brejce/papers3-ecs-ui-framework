// apps/reader/test/jump.cpp — 现场诊断：把"目录跳章"这一步单独复现
//
//   ./build/jump 大明1973.txt 224648         # 跳到这个文件偏移，打印块表 + 第一页画出来的文字
//   ./build/jump 大明1973.txt 224648 --pct   # 顺带打印页码/百分比
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include "../main/layout.h"

extern void collect_reset();
extern void collect_stop();
extern const char* collect_buf();
extern long collect_len();
extern void stub_set_book_dir(const char* d);
extern void* fake_font(float px);

using namespace app;

int main(int argc, char** argv) {
    if (argc < 3) { printf("用法: jump <书> <文件偏移>\n"); return 1; }
    const char* dir = getenv("BOOK_DIR") ? getenv("BOOK_DIR") : "books";
    stub_set_book_dir(dir);
    ui::g_font = fake_font((float)rd::g_size);

    const std::string book = argv[1];
    const uint32_t off = (uint32_t)strtoul(argv[2], nullptr, 10);
    const int sz = so_book_size(book.c_str());
    if (sz <= 0) { printf("读不到 %s/%s\n", dir, book.c_str()); return 1; }

    ui::scopy(rd::g_name, book.c_str(), sizeof(rd::g_name));
    rd::g_fsize = (uint32_t)sz;
    rd::g_md = false;

    // 分章索引（和真机上一样：读 SD 缓存 / 扫一遍）
    chap::start(book.c_str(), sz);
    int guard = 0;
    while (chap::running() && guard++ < 400000) chap::step();
    printf("索引 %d 项\n", chap::count());
    int hit = -1;
    for (int i = 0; i < chap::count(); i++) if (chap::list()[i].off == off) { hit = i; break; }
    if (hit >= 0) {
        char t[160] = {0};
        chap::read_title(hit, t, sizeof(t) - 1);
        printf("索引第 %d 项 = 本偏移，L%d 标题[%s] title_off=%u title_len=%u\n",
               hit + 1, chap::title_level(hit), t, chap::list()[hit].title_off,
               chap::list()[hit].title_len);
    } else {
        printf("⚠ 这个偏移不在索引里（说明跳转偏移跟索引对不上）\n");
    }

    rd::pages_reset(off, 0);                  // ← 目录点一条就是这么干的
    if (!rd::win_load_at(off)) { printf("窗口装载失败\n"); return 1; }
    printf("窗口 off=%u len=%d eof=%d 块数=%d\n", rd::g_win_off, rd::g_win_len,
           (int)rd::g_win_eof, rd::g_doc.n);
    for (int i = 0; i < rd::g_doc.n && i < 8; i++)
        printf("  块%d abs=%u kind=%d text=[%.40s]\n", i,
               rd::g_win_off + rd::g_doc.blk[i].off, rd::g_doc.blk[i].kind,
               rd::g_doc.blk[i].text ? rd::g_doc.blk[i].text : "(null)");

    collect_reset();
    rd::paint_page(0);
    collect_stop();
    printf("页 0 画出来 %ld 字节：\n[%.400s]\n", collect_len(), collect_buf());

    char cn[120] = {0};
    int lv = 0;
    if (rd::chapter_name_at(off, cn, sizeof(cn), &lv)) printf("页脚章名 = [%s] L%d\n", cn, lv);
    else printf("页脚章名 = （空）\n");
    printf("百分比 = %d%%（页表 %d 页，末页起点 %u）\n", rd::page_pct(), (int)rd::g_ps_n,
           rd::g_ps_n ? rd::g_ps[rd::g_ps_n - 1].off : 0u);
    return 0;
}
