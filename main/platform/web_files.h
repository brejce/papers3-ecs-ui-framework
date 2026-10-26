#pragma once

// platform/web_files.h — SD 卡的网页文件管理（**FTP 目录列表那种样子**）
//
// 手机连上热点后打开 http://192.168.4.1/ ，看到一张目录表：
//   名字（可点：目录进目录 / 文件下载） · 大小 · 时间 · 改名 / 删
// 顶部：上一级 · 新建文件夹 · 上传
//
// 页面**几乎全由服务器渲染**，JS 只剩二十来行（上传 + 确认框 + 提交一个隐藏表单）。好处：
//   - 浏览器前进/后退正常，不会卡进一个出不去的状态（旧版是全 fetch 的 SPA，出错就没法退）；
//   - 中文名直接写进 HTML（UTF-8），不经过 JSON/JS 那圈转义；
//   - 删除/改名/新建走 POST 表单，不是 GET 链接 —— 免得浏览器预取把文件删了。
//
// 路由：
//   GET  /?path=/dir                  列目录
//   GET  /api/dl?path=/a/b.txt        下载
//   POST /api/ul?path=/dir&name=x     上传（body = 文件原始字节，流式落盘）
//   POST /api/rm    path=/a/b.txt     删除（**递归**，非空目录连内容一起删）
//   POST /api/mv    path=/a/b&to=/c   改名 / 移动
//   POST /api/mkdir path=/dir/new     建目录
//
// 前置条件：FATFS 必须开 **长文件名 + UTF-8 API 编码**
//   （sdkconfig：CONFIG_FATFS_LFN_HEAP + CONFIG_FATFS_API_ENCODING_UTF_8）
//   否则 readdir 拿到的名字在 8.3 那层就废了，中文目录根本显示不出来。
//
// 安全：路径夹在 /sdcard 里、拒绝 ".."；**没有登录** —— 配网用的临时热点，谁连上谁能改。

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"          // /api/stat：两次采样的时间差
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"  // uxTaskGetSystemState → 双核空闲任务计数
#include "freertos/task.h"

namespace papers3::web_files {

static const char* TAG  = "WEB";
static const char* ROOT = "/sdcard";

// ══════════ 工具 ══════════

inline int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline void urldecode(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 1 < cap; i++) {
        if (s[i] == '%' && s[i + 1] && s[i + 2]) {
            int h = hexv(s[i + 1]), l = hexv(s[i + 2]);
            if (h >= 0 && l >= 0) { out[o++] = (char)((h << 4) | l); i += 2; continue; }
        }
        out[o++] = (s[i] == '+') ? ' ' : s[i];
    }
    out[o] = 0;
}

// 百分号编码（拼链接用）。中文名必须走这里。
inline void urlenc(const char* s, char* out, size_t cap) {
    static const char* H = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 4 < cap; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == '/') {                     // 路径分隔符留着，日志里好读
            out[o++] = '/';
        } else {
            out[o++] = '%'; out[o++] = H[c >> 4]; out[o++] = H[c & 15];
        }
    }
    out[o] = 0;
}

// HTML 转义（进属性/文本前必须）
inline void htmlesc(const char* s, char* out, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; s[i] && o + 7 < cap; i++) {
        switch (s[i]) {
            case '"':  memcpy(out + o, "&quot;", 6); o += 6; break;
            case '&':  memcpy(out + o, "&amp;",  5); o += 5; break;
            case '<':  memcpy(out + o, "&lt;",   4); o += 4; break;
            case '>':  memcpy(out + o, "&gt;",   4); o += 4; break;
            default:   out[o++] = s[i];
        }
    }
    out[o] = 0;
}

inline bool qget(httpd_req_t* req, const char* key, char* out, size_t cap) {
    char qs[1024], raw[640];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) return false;
    if (httpd_query_key_value(qs, key, raw, sizeof(raw)) != ESP_OK) return false;
    urldecode(raw, out, cap);
    return true;
}

// ── 请求期工作缓冲：**一律放 PSRAM** ───────────────────────────────────
// 原来这里有三个 4KB 的 static（body / 上传块 / 下载块）= 12KB 常驻**内部 RAM**，
// 而这颗芯片内部 RAM 只剩十几 KB：跑一阵之后 httpd 建连接、发大响应就分配不到，
// 表现是「列表 200 但 0 字节」「下载卡住不返回」。
inline char* wbuf_slot(int slot, size_t need) {
    static char*  p[12]   = {nullptr};
    static size_t cap[12] = {0};
    if (slot < 0 || slot >= 12) return nullptr;
    if (need > cap[slot]) {
        if (p[slot]) heap_caps_free(p[slot]);
        p[slot] = (char*)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
        cap[slot] = p[slot] ? need : 0;
    }
    return p[slot];
}

// POST body（application/x-www-form-urlencoded）：**只能读一次**，所以先整份读进来再取键
static char* g_body = nullptr;          // 首次用时从 PSRAM 拿（见 wbuf）
static int  g_body_len = 0;
static const int G_BODY_CAP = 4096;

inline bool read_body(httpd_req_t* req) {
    g_body_len = 0;
    if (!g_body) g_body = wbuf_slot(0, (size_t)G_BODY_CAP);
    if (!g_body) return false;
    int len = req->content_len;
    if (len <= 0 || len >= G_BODY_CAP) return false;
    while (g_body_len < len) {
        int r = httpd_req_recv(req, g_body + g_body_len, len - g_body_len);
        if (r <= 0) { if (r == HTTPD_SOCK_ERR_TIMEOUT) continue; return false; }
        g_body_len += r;
    }
    g_body[g_body_len] = 0;
    return true;
}

inline bool body_get(const char* key, char* out, size_t cap) {
    size_t klen = strlen(key);
    for (char* p = g_body; p && *p; ) {
        char* amp = strchr(p, '&');
        if (amp) *amp = 0;                              // 临时截断当前段
        bool hit = (strncmp(p, key, klen) == 0 && p[klen] == '=');
        if (hit) urldecode(p + klen + 1, out, cap);     // 在截断态下解码值
        if (amp) *amp = '&';                            // 恢复 & —— 否则下次 body_get 看不到后面的段
        if (hit) return true;
        p = amp ? amp + 1 : nullptr;
    }
    return false;
}

// 卡内路径（以 / 开头）→ 真实绝对路径
inline bool safe_path(const char* qpath, char* out, size_t cap) {
    if (!qpath || qpath[0] != '/' || strlen(qpath) > 300) return false;
    if (strstr(qpath, "..")) return false;
    snprintf(out, cap, "%s%s", ROOT, qpath);
    return true;
}

// /a/b/c → /a/b
inline void parent_of(const char* path, char* out, size_t cap) {
    const char* s = strrchr(path, '/');
    if (!s || s == path) { snprintf(out, cap, "/"); return; }
    size_t n = (size_t)(s - path);
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

// 冒号等号都不进 URL 的 303 跳回列表
inline esp_err_t back_to(httpd_req_t* req, const char* qpath) {
    char enc[640], loc[760];
    urlenc(qpath, enc, sizeof(enc));
    snprintf(loc, sizeof(loc), "/?path=%s", enc);
    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", loc);
    return httpd_resp_send(req, nullptr, 0);
}

// ── 递归删除（非空目录也能删）──
// 每层一个静态缓冲：HTTP 任务是单线程的，别用栈（递归 + 栈上 1KB 会啃光 8KB 栈）
inline bool rm_recursive(const char* path, int depth) {
    static char* rm = wbuf_slot(9, 6 * 1024);   // PSRAM：6 层 × 1KB 留在内部 RAM 太贵
    if (!rm) return false;
    char (*buf)[1024] = (char (*)[1024])rm;
    if (depth >= 6) { ESP_LOGW(TAG, "目录太深，放弃: %s", path); return false; }

    struct stat st;
    if (stat(path, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;

    DIR* d = opendir(path);
    if (!d) return false;
    char* child = buf[depth];
    struct dirent* e;
    bool ok = true;
    while ((e = readdir(d)) != nullptr) {
        if (e->d_name[0] == '.') continue;
        snprintf(child, 1024, "%s/%s", path, e->d_name);
        if (!rm_recursive(child, depth + 1)) ok = false;
    }
    closedir(d);
    return ok && rmdir(path) == 0;
}

// ══════════ 页面外壳（服务器渲染，只留这点 JS）══════════

static const char PAGE_HEAD[] = R"HTML(<!DOCTYPE html><html lang="zh"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>SD</title><style>
body{font:14px/1.6 ui-monospace,Menlo,Consolas,"PingFang SC",monospace;margin:0;background:#fff;color:#111}
header{padding:8px 12px;background:#eee;border-bottom:1px solid #ccc}
#path{margin-top:2px;word-break:break-all;color:#036}
.bar{padding:8px 12px;background:#fafafa;border-bottom:1px solid #ddd}
.b{display:inline-block;padding:5px 10px;margin:0 6px 5px 0;border:1px solid #bbb;border-radius:5px;
   background:#fff;font-size:13px;cursor:pointer;color:#036;text-decoration:none}
.b:active{background:#e8e8e8}
.r{color:#a00;border-color:#e3b8b8}
table{width:100%;border-collapse:collapse}
th{background:#fafafa;font-weight:600;font-size:12px;color:#555;text-align:left;padding:6px 10px;border-bottom:1px solid #ddd}
td{padding:7px 10px;border-bottom:1px solid #eee;text-align:left}
td.n{word-break:break-all}
td.s{white-space:nowrap;color:#555;text-align:right;width:1%}
td.a{white-space:nowrap;width:1%;text-align:right}
a{color:#036}
#msg{position:fixed;left:0;right:0;bottom:0;padding:8px 12px;background:#222;color:#fff;font-size:13px;display:none}
</style></head><body>
)HTML";

static const char PAGE_TAIL[] = R"HTML(
<div id="msg"></div>
<form id="op" method="post" style="display:none"><input id="op_p" name="path"><input id="op_t" name="to"></form>
<script>
function $(s){return document.querySelector(s)}
function act(a,p,t){var f=$('#op');f.action=a;$('#op_p').value=p;$('#op_t').value=t||'';f.submit()}
document.addEventListener('click',function(ev){
  var t=ev.target.closest?ev.target.closest('a[data-k]'):null; if(!t)return; ev.preventDefault();
  var k=t.getAttribute('data-k'),a=t.getAttribute('data-a'),p=t.getAttribute('data-p'),n=t.getAttribute('data-n');
  if(k=='改名'){var v=prompt('新名字：',n); if(v&&v!=n) act(a,p.replace(/\/[^\/]*$/,'')+'/'+v,'')}
  else if(k=='新建'){var v=prompt('新文件夹名：'); if(v) act(a,p+'/'+v,'')}
  else if(k=='删除'){ if(confirm('删除「'+n+'」'+(t.getAttribute('data-r')=='1'?'（连同里面全部内容）':'')+' ？')) act(a,p,'') }
});
$('#file').onchange=function(){
  var fs=[].slice.call(this.files); this.value=''; var cur=$('#cur').value; var i=0;
  (function next(){
    if(i>=fs.length){location.href='/?path='+encodeURIComponent(cur);return}
    $('#msg').textContent='上传 '+fs[i].name+' …'; $('#msg').style.display='block';
    fetch('/api/ul?path='+encodeURIComponent(cur)+'&name='+encodeURIComponent(fs[i].name),{method:'POST',body:fs[i]})
      .then(function(r){ if(!r.ok) throw 0; i++; next() },
            function(){ $('#msg').textContent='上传失败：'+fs[i].name; i++; setTimeout(next,1500) });
  })();
};
</script></body></html>
)HTML";

// ══════════ 处理器 ══════════

static esp_err_t h_page(httpd_req_t* req) {
    char qp[320] = "/", dir[512];
    qget(req, "path", qp, sizeof(qp));
    if (!safe_path(qp, dir, sizeof(dir))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    DIR* d = opendir(dir);
    if (!d) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "目录打不开");
        return ESP_FAIL;
    }

    // 大缓冲放 PSRAM：HTTP 任务单线程，不怕共享；但这组一共 ~13.8KB，
    // 留在 .bss 就是常驻内部 RAM（这芯片只剩十几 KB）→ 跑一阵网页就 0 字节。
    char* pe   = wbuf_slot(3, 800);
    char* he   = wbuf_slot(4, 704);
    char* ep   = wbuf_slot(5, 1600);
    char* row  = wbuf_slot(6, 8192);
    char* path = wbuf_slot(7, 640);
    char* ne   = wbuf_slot(8, 900);
    if (!pe || !he || !ep || !row || !path || !ne) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem");
        closedir(d);
        return ESP_FAIL;
    }
    char parent[400], penc[800];

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req, PAGE_HEAD);

    parent_of(qp, parent, sizeof(parent));
    urlenc(parent, penc, sizeof(penc));
    htmlesc(qp, he, 700);
    snprintf(row, 8192,
             "<header><b>SD 卡</b><div id=\"path\">%s</div></header>"
             "<div class=\"bar\">"
             "<a class=\"b\" href=\"/?path=%s\">↑ 上一级</a>"
             "<a class=\"b\" href=\"#\" data-k=\"新建\" data-a=\"/api/mkdir\" data-p=\"%s\">＋ 文件夹</a>"
             "<label class=\"b\">⬆ 上传<input id=\"file\" type=\"file\" multiple style=\"display:none\"></label>"
             "<input id=\"cur\" type=\"hidden\" value=\"%s\">"
             "<table><tr><th>名字</th><th>大小</th><th>时间</th><th></th></tr>",
             he, penc, he, he);
    httpd_resp_sendstr_chunk(req, row);

    struct dirent* e;
    int n = 0;
    while ((e = readdir(d)) != nullptr && n < 500) {
        if (e->d_name[0] == '.') continue;
        char full[1024];
        snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(full, &st) != 0) continue;
        const bool isdir = S_ISDIR(st.st_mode);

        // 卡内路径（结尾拼名字）→ 编码/转义两份
        snprintf(path, 640, "%s%s%s", qp, (strcmp(qp, "/") == 0 ? "" : "/"), e->d_name);
        urlenc(path, pe, 800);
        htmlesc(path, ep, 1600);
        htmlesc(e->d_name, ne, 900);

        char size[32] = "";
        if (!isdir) {
            double v = (double)st.st_size;
            if (v < 1024)             snprintf(size, sizeof(size), "%d B", (int)v);
            else if (v < 1024 * 1024) snprintf(size, sizeof(size), "%.1f K", v / 1024);
            else                      snprintf(size, sizeof(size), "%.1f M", v / 1048576);
        }
        char when[32] = "";
        if (st.st_mtime > 946684800) {              // 2000 年以前当没设时间
            struct tm tmv;
            localtime_r(&st.st_mtime, &tmv);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tmv);
        }

        snprintf(row, 8192,
                 "<tr><td class=\"n\">%s<a href=\"%s%s\">%s</a></td>"
                 "<td class=\"s\">%s</td><td class=\"s\">%s</td>"
                 "<td class=\"a\">"
                 "<a class=\"b\" href=\"#\" data-k=\"改名\" data-a=\"/api/mv\" data-p=\"%s\" data-n=\"%s\">改名</a>"
                 "<a class=\"b r\" href=\"#\" data-k=\"删除\" data-a=\"/api/rm\" data-p=\"%s\" data-n=\"%s\"%s>删</a>"
                 "</td></tr>",
                 isdir ? "📁 " : "📄 ",
                 isdir ? "/?path=" : "/api/dl?path=", pe,
                 ne, size, when, ep, ne, ep, ne, isdir ? " data-r=\"1\"" : "");
        httpd_resp_sendstr_chunk(req, row);
        n++;
    }
    closedir(d);
    httpd_resp_sendstr_chunk(req, "</table>");
    httpd_resp_sendstr_chunk(req, PAGE_TAIL);
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static esp_err_t h_op(httpd_req_t* req) {
    char p[320], to[320] = {0}, full[640], full2[640], back[400];
    if (!read_body(req) || !body_get("path", p, sizeof(p)) || !safe_path(p, full, sizeof(full))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    parent_of(p, back, sizeof(back));

    if (strcmp(req->uri, "/api/rm") == 0) {
        if (strcmp(full, ROOT) == 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "不能删根");
            return ESP_FAIL;
        }
        bool ok = rm_recursive(full, 0);
        ESP_LOGI(TAG, "%s %s", ok ? "已删除" : "删除失败", full);
        return back_to(req, back);
    }
    if (strcmp(req->uri, "/api/mkdir") == 0) {
        bool ok = (mkdir(full, 0777) == 0);
        ESP_LOGI(TAG, "%s %s", ok ? "已建目录" : "建目录失败", full);
        return back_to(req, back);
    }
    if (strcmp(req->uri, "/api/mv") == 0) {
        if (!body_get("to", to, sizeof(to)) || !safe_path(to, full2, sizeof(full2))) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad target");
            return ESP_FAIL;
        }
        if (strcmp(full, full2) == 0) return back_to(req, back);
        bool ok = (rename(full, full2) == 0);
        ESP_LOGI(TAG, "%s %s → %s", ok ? "已改名" : "改名失败", full, full2);
        return back_to(req, back);
    }
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no such op");
    return ESP_FAIL;
}

// ── /api/stat：实时负载（双核各一个占用率 + 堆/PSRAM）──
// 双核占用率 = 100 − 该核空闲任务（IDLE0/IDLE1）在两次请求之间占的时间比。
// ★ 第一次请求只建立基线（cpu=-1），隔 2 秒再取一次才有值。
static esp_err_t h_stat(httpd_req_t* req) {
    static TaskStatus_t st[48];
    static uint64_t last_idle0 = 0, last_idle1 = 0;
    static int64_t  last_us = 0;

    uint64_t idle0 = 0, idle1 = 0;
    const int got = (int)uxTaskGetSystemState(st, 48, nullptr);
    for (int i = 0; i < got; ++i) {
        const char* nm = st[i].pcTaskName ? st[i].pcTaskName : "";
        if (strncmp(nm, "IDLE0", 5) == 0)      idle0 = st[i].ulRunTimeCounter;
        else if (strncmp(nm, "IDLE1", 5) == 0) idle1 = st[i].ulRunTimeCounter;
    }

    const int64_t now = esp_timer_get_time();
    const int64_t dt  = (last_us != 0) ? (now - last_us) : 0;
    int c0 = -1, c1 = -1;
    if (dt > 0) {
        c0 = (int)(100 - (int64_t)(idle0 - last_idle0) * 100 / dt);
        c1 = (int)(100 - (int64_t)(idle1 - last_idle1) * 100 / dt);
        if (c0 < 0) c0 = 0; else if (c0 > 100) c0 = 100;
        if (c1 < 0) c1 = 0; else if (c1 > 100) c1 = 100;
    }
    last_idle0 = idle0; last_idle1 = idle1; last_us = now;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "{\"cpu0\":%d,\"cpu1\":%d,\"dt_ms\":%d,\"tasks\":%d,\"heap\":%u,\"psram_free\":%u}\n",
             c0, c1, (int)(dt / 1000), got,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, buf);
}

// ── /api/tasks：任务表（名字 / 钉在哪个核 / 优先级 / 累计 CPU / 栈余量）──
// 需要 CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID=y 才有 xCoreID。
static esp_err_t h_tasks(httpd_req_t* req) {
    static TaskStatus_t st[48];
    const int got = (int)uxTaskGetSystemState(st, 48, nullptr);
    httpd_resp_set_type(req, "text/plain");
    char line[160], cbuf[16];
    for (int i = 0; i < got; ++i) {
        const BaseType_t c = st[i].xCoreID;
        if (c < 0) snprintf(cbuf, sizeof(cbuf), "any");
        else       snprintf(cbuf, sizeof(cbuf), "%d", (int)c);
        snprintf(line, sizeof(line), "%-16s core=%-3s prio=%-2u cpu=%9lluus  stack_free=%u\n",
                 st[i].pcTaskName ? st[i].pcTaskName : "?",
                 cbuf,
                 (unsigned)st[i].uxCurrentPriority,
                 (unsigned long long)st[i].ulRunTimeCounter,
                 (unsigned)st[i].usStackHighWaterMark);
        httpd_resp_sendstr_chunk(req, line);
    }
    httpd_resp_sendstr_chunk(req, nullptr);
    return ESP_OK;
}

static esp_err_t h_dl(httpd_req_t* req) {
    char qp[320], full[640];
    if (!qget(req, "path", qp, sizeof(qp)) || !safe_path(qp, full, sizeof(full))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    FILE* f = fopen(full, "rb");
    if (!f) { httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "打不开"); return ESP_FAIL; }

    // 中文名下载：filename* 走 RFC 5987（UTF-8 + 百分号编码），另给个 ASCII 兜底名
    const char* base = strrchr(full, '/');
    base = base ? base + 1 : full;
    char enc[900], cd[1000];
    urlenc(base, enc, sizeof(enc));
    snprintf(cd, sizeof(cd), "attachment; filename=\"download\"; filename*=UTF-8''%s", enc);

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", cd);

    char* buf = wbuf_slot(2, 4096);
    if (!buf) { fclose(f); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    size_t r;
    bool bad = false;
    while ((r = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) { bad = true; break; }
    }
    fclose(f);
    // ★ 无论成败都要发结束块：不发就等于把连接晾在那儿，浏览器只能一直等（实测症状）
    httpd_resp_send_chunk(req, nullptr, 0);
    if (bad) { ESP_LOGW(TAG, "下载中断 %s（内部 RAM 不足？）", full); return ESP_FAIL; }
    ESP_LOGI(TAG, "下载 %s", full);
    return ESP_OK;
}

static esp_err_t h_ul(httpd_req_t* req) {
    char qp[320] = "/", name[300], dir[512], full[1024];
    if (!qget(req, "path", qp, sizeof(qp)) || !qget(req, "name", name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "缺参数");
        return ESP_FAIL;
    }
    if (!name[0] || strchr(name, '/') || strstr(name, "..")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad name");
        return ESP_FAIL;
    }
    if (!safe_path(qp, dir, sizeof(dir))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad path");
        return ESP_FAIL;
    }
    snprintf(full, sizeof(full), "%s/%s", dir, name);

    FILE* f = fopen(full, "wb");
    if (!f) { httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "写不进去"); return ESP_FAIL; }
    ESP_LOGI(TAG, "上传 %s (%d 字节)", full, (int)req->content_len);

    int left = req->content_len;
    char* buf = wbuf_slot(1, 4096);
    if (!buf) { fclose(f); unlink(full); httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no mem"); return ESP_FAIL; }
    while (left > 0) {
        int want = left > 4096 ? 4096 : left;
        int got = httpd_req_recv(req, buf, want);
        if (got <= 0) {
            if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
            fclose(f); unlink(full);
            return ESP_FAIL;
        }
        if (fwrite(buf, 1, got, f) != (size_t)got) { fclose(f); return ESP_FAIL; }
        left -= got;
    }
    fclose(f);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

// ══════════ 启动 / 关闭 ══════════
// 服务句柄留一份：热点页和「连 WiFi」页共用同一个服务器（谁都能开关）
inline httpd_handle_t g_srv = nullptr;

inline bool start() {
    if (g_srv) return true;                 // 已经在跑（两页共用一个）
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 12;
    cfg.stack_size       = 8192;
    cfg.lru_purge_enable = true;
    cfg.core_id          = 0;      // 双核：httpd 归 core0（和 WiFi 同核，别跑到 UI 核上）

    httpd_handle_t srv = nullptr;
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start 失败（内存不够？）");
        return false;
    }

    struct { const char* uri; httpd_method_t m; esp_err_t (*h)(httpd_req_t*); } regs[] = {
        {"/",          HTTP_GET,  h_page},
        {"/api/stat",  HTTP_GET,  h_stat},
        {"/api/tasks", HTTP_GET,  h_tasks},
        {"/api/dl",    HTTP_GET,  h_dl},
        {"/api/ul",    HTTP_POST, h_ul},
        {"/api/rm",    HTTP_POST, h_op},
        {"/api/mv",    HTTP_POST, h_op},
        {"/api/mkdir", HTTP_POST, h_op},
    };
    for (auto& r : regs) {
        httpd_uri_t u = { .uri = r.uri, .method = r.m, .handler = r.h, .user_ctx = nullptr };
        httpd_register_uri_handler(srv, &u);
    }
    ESP_LOGI(TAG, "文件管理页面已就绪（目录列表 / 下载 / 上传 / 删 / 改名 / 建目录）");
    g_srv = srv;
    return true;
}

// 关掉网页服务（文件还都在 SD 上，只是网页打不开了）
inline void stop() {
    if (!g_srv) return;
    httpd_stop(g_srv);
    g_srv = nullptr;
    ESP_LOGI(TAG, "文件管理已关（再开就恢复）");
}

inline bool running() { return g_srv != nullptr; }

// ── 自愈 + 观测 ────────────────────────────────────────────────
// 症状：跑久了网页「200 但 0 字节」或直接卡死。根子在内部 RAM（只有十几 KB），
// httpd 建连接/发大响应要内部缓冲，分配不到就半途而废且不报错。
// 这里每 5s 看一眼内部最大可用块：低到危险就先记一笔、再把 httpd 重建一次恢复现场。
inline void tick() {
    static int64_t t_check = 0, t_log = 0, t_heal = 0;
    const int64_t now = esp_timer_get_time() / 1000;
    if (now - t_check < 5000) return;
    t_check = now;
    const size_t fr  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const size_t big = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    if (fr < 14336 && now - t_log > 60000) {          // 60s 最多一条，别刷屏
        t_log = now;
        ESP_LOGW(TAG, "内部 RAM 紧张：free %uB / 最大块 %uB（httpd 可能开始 0 字节）",
                 (unsigned)fr, (unsigned)big);
    }
    // ★ 自愈要限频：内部 RAM 一旦紧张就是长期紧张，不限频就是每 5s 一次
    //   stop/start —— 每次都会掐断正在传的那条连接，最后看起来像"网页反复抽风"。
    if (g_srv && big < 6144 && now - t_heal > 30000) {
        t_heal = now;
        ESP_LOGW(TAG, "内部最大块只剩 %uB → 重建 httpd 自愈", (unsigned)big);
        stop();
        start();
    }
}

} // namespace papers3::web_files
