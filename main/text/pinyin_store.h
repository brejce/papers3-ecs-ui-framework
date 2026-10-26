#pragma once

// ──────────────────────────────────────────────
// pinyin_store.h — 拼音索引的「文件化存储」
//
// 三级来源，优先级从低到高：
//   ① 内置默认表（编译进固件，pinyin_index.h）—— 永远有底，SD 没插/文件坏了照样能打字
//   ② SD 文件 /sdcard/m5ui/pinyin.idx —— **可升级**：换文件即换词库，不用重刷固件
//   ③ 运行时按使用习惯调序 —— 选过的候选往前挪；改动落盘（dirty → save）
//
// 文件格式（定长记录，整体读写）：
//   header : magic "PYIX"(4) | version u16 | cand_max u16 | count u32
//   entry  : py[12] | n u8 | pad u8 | cand[cand_max] u16 | cnt[cand_max] u8
//            └ 前面优先级高              └ 使用计数（真词频，排序键，饱和 255）
// 记录大小 = 14 + cand_max*3，cand_max = CAND_MAX 时 = 110B（Entry 有 static_assert 兜着）
// ──────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "text/pinyin_index.h"

namespace papers3::ime {

static const char* STORE_TAG = "PYIDX";

static constexpr int  PY_MAX   = 12;    // 拼音串上限（含结尾 0）
static constexpr int  CAND_MAX = 32;    // 每音节候选上限（与生成脚本一致）
static constexpr uint16_t IDX_VERSION = 2;   // v2：每条候选多了使用计数 cnt[]
static constexpr const char* IDX_DIR  = "/sdcard/m5ui";
static constexpr const char* IDX_PATH = "/sdcard/m5ui/pinyin.idx";

// 内存中的一条：与文件记录**逐字节对应**
struct Entry {
    char     py[PY_MAX];
    uint8_t  n;
    uint8_t  pad;
    uint16_t cand[CAND_MAX];
    uint8_t  cnt[CAND_MAX];   // 使用计数（饱和 255）—— 真正的词频，排序键
};
// 布局飘了就直接编译不过，别等到写坏文件
static_assert(sizeof(Entry) == PY_MAX + 2 + CAND_MAX * 3,
              "Entry 布局必须和 pinyin.idx 的记录格式一致");

// 按使用计数降序**稳定**排序：计数相同就保持原序 = 保住「字频基线」
// （所以没被选过的字仍按生成器排好的常用度顺序，用过的才往上冒）
inline void entry_sort_by_count(Entry& e) {
    for (int i = 1; i < e.n; i++) {
        uint16_t c = e.cand[i];
        uint8_t  k = e.cnt[i];
        int j = i - 1;
        while (j >= 0 && e.cnt[j] < k) {
            e.cand[j + 1] = e.cand[j];
            e.cnt[j + 1]  = e.cnt[j];
            j--;
        }
        e.cand[j + 1] = c;
        e.cnt[j + 1]  = k;
    }
}

struct Table {
    Entry* ent     = nullptr;
    int    count   = 0;
    bool   from_sd = false;
    bool   dirty   = false;   // 有改动待落盘
};

inline Table g_table;   // 单实例（输入法只有一个）

// ── 分配 ──
inline Entry* table_alloc(int n) {
    if (n <= 0) return nullptr;
    size_t bytes = (size_t)n * sizeof(Entry);
    Entry* p = (Entry*)heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);   // 优先 PSRAM
    if (!p) p = (Entry*)heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL);
    if (p) memset(p, 0, bytes);
    return p;
}

// ── 从内置表填一份（SD 不可用时的底）──
inline void table_builtin(Table& t) {
    if (t.ent) { heap_caps_free(t.ent); t.ent = nullptr; }
    t.count = PINYIN_INDEX_COUNT;
    t.ent = table_alloc(t.count);
    if (!t.ent) { t.count = 0; return; }
    for (int i = 0; i < t.count; i++) {
        const PinyinEntry& s = PINYIN_INDEX[i];
        strncpy(t.ent[i].py, s.pinyin, PY_MAX - 1);
        int n = s.count > CAND_MAX ? CAND_MAX : s.count;
        t.ent[i].n = (uint8_t)n;
        for (int k = 0; k < n; k++) t.ent[i].cand[k] = s.cand[k];
    }
    t.from_sd = false;
    t.dirty   = false;
}

// ── 按拼音字典序排一下（文件可能没排；二分查找要求有序）──
inline int entry_cmp(const void* a, const void* b) {
    return strcmp(((const Entry*)a)->py, ((const Entry*)b)->py);
}

// ── 从 SD 文件读；没有/坏了就退回内置 ──
// 返回 true = 就绪（不代表来自 SD，看 table.from_sd）
inline bool table_load(Table& t) {
    FILE* f = fopen(IDX_PATH, "rb");
    if (!f) {
        ESP_LOGI(STORE_TAG, "no %s — 用内置默认索引", IDX_PATH);
        table_builtin(t);
        return t.count > 0;
    }

    char magic[4] = {0};
    uint16_t ver = 0, cmax = 0;
    uint32_t cnt = 0;
    bool ok = fread(magic, 1, 4, f) == 4
           && memcmp(magic, "PYIX", 4) == 0
           && fread(&ver,  sizeof(ver),  1, f) == 1
           && fread(&cmax, sizeof(cmax), 1, f) == 1
           && fread(&cnt,  sizeof(cnt),  1, f) == 1
           && ver == IDX_VERSION && cmax > 0 && cmax <= CAND_MAX && cnt > 0 && cnt < 100000;

    if (ok) {
        t.ent = table_alloc((int)cnt);
        ok = (t.ent != nullptr);
        if (ok) {
            const int rec = PY_MAX + 2 + cmax * 3;          // cand(u16) + cnt(u8)
            uint8_t* buf = (uint8_t*)heap_caps_malloc(rec, MALLOC_CAP_INTERNAL);
            ok = (buf != nullptr);
            if (ok) {
                int got = 0;
                for (uint32_t i = 0; i < cnt; i++) {
                    if (fread(buf, 1, rec, f) != (size_t)rec) break;
                    Entry& e = t.ent[got];
                    memcpy(e.py, buf, PY_MAX);
                    e.py[PY_MAX - 1] = 0;
                    if (!e.py[0]) continue;                    // 拼音空 → 丢
                    e.n = buf[PY_MAX];
                    if (e.n > CAND_MAX) e.n = CAND_MAX;
                    memcpy(e.cand, buf + PY_MAX + 2, (size_t)e.n * 2);
                    memcpy(e.cnt,  buf + PY_MAX + 2 + cmax * 2, e.n);
                    got++;
                }
                heap_caps_free(buf);
                t.count = got;
                ok = (got > 0);
                if (ok) {
                    qsort(t.ent, t.count, sizeof(Entry), entry_cmp);   // 按拼音有序（二分要用）
                    for (int i = 0; i < t.count; i++) entry_sort_by_count(t.ent[i]);  // 再按词频
                }
            } else {
                heap_caps_free(t.ent); t.ent = nullptr;
            }
        }
    }
    fclose(f);

    if (!ok) {
        ESP_LOGW(STORE_TAG, "文件坏/格式不对 — 退回内置索引");
        table_builtin(t);
        return t.count > 0;
    }
    t.from_sd = true;
    t.dirty   = false;
    ESP_LOGI(STORE_TAG, "SD 索引: %d 音节 (%.1f KB)", t.count,
             (double)t.count * sizeof(Entry) / 1024.0);
    return true;
}

// ── 写回 SD（把运行时的调序结果固化）──
inline bool table_save(Table& t) {
    if (!t.ent || t.count <= 0) return false;
    mkdir(IDX_DIR, 0777);                 // 已存在会返回失败，忽略
    FILE* f = fopen(IDX_PATH, "wb");
    if (!f) { ESP_LOGW(STORE_TAG, "save: 打不开 %s", IDX_PATH); return false; }

    uint16_t ver = IDX_VERSION, cmax = CAND_MAX;
    uint32_t cnt = (uint32_t)t.count;
    fwrite("PYIX", 1, 4, f);
    fwrite(&ver,  2, 1, f);
    fwrite(&cmax, 2, 1, f);
    fwrite(&cnt,  4, 1, f);

    const size_t rec = sizeof(Entry);
    for (int i = 0; i < t.count; i++) fwrite(&t.ent[i], rec, 1, f);
    fclose(f);
    t.dirty = false;
    ESP_LOGI(STORE_TAG, "索引已落盘: %d 音节", t.count);
    return true;
}

// ── 查找 ──
inline Entry* table_find(Table& t, const char* py) {
    int lo = 0, hi = t.count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int r = strcmp(py, t.ent[mid].py);
        if (r == 0) return &t.ent[mid];
        if (r < 0) hi = mid - 1; else lo = mid + 1;
    }
    return nullptr;
}

// 前缀的第一项下标（-1 = 没有）；表是有序的，顺序扫也行（400 条）
inline int table_prefix_first(Table& t, const char* py, int len) {
    for (int i = 0; i < t.count; i++) {
        if (strncmp(t.ent[i].py, py, (size_t)len) == 0) return i;
        if (strncmp(t.ent[i].py, py, (size_t)len) > 0) break;   // 已经越过前缀区间
    }
    return -1;
}

// ── 用量调序：给选中的候选计数 +1，再按计数重排这个音节 ──
// 真词频（饱和 255）：用得多自然靠前；计数相同的仍保持生成器排好的字频基线顺序。
inline void table_bump(Table& t, const char* py, uint16_t cp) {
    Entry* e = table_find(t, py);
    if (!e) return;
    for (int i = 0; i < e->n; i++) {
        if (e->cand[i] != cp) continue;
        if (e->cnt[i] < 255) e->cnt[i]++;
        entry_sort_by_count(*e);
        t.dirty = true;
        return;
    }
}

} // namespace papers3::ime
