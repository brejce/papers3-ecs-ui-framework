#!/bin/bash
# 编译 terminal.so（手写 g++，不依赖 project_so 宏 —— 那个宏只认 .c）
# 用法：cd apps/terminal && ./build.sh   → 产出 build/terminal.so
# 上传：curl -X POST "http://<设备IP>/api/ul?path=/apps&name=terminal.so" --data-binary @build/terminal.so
set -e
cd "$(dirname "$0")"
source ~/esp-env/env.sh >/dev/null 2>&1

GXX=xtensa-esp32s3-elf-g++
NM=xtensa-esp32s3-elf-nm

CPPFLAGS="-fPIC -fno-exceptions -fno-rtti -std=gnu++2b -fvisibility=hidden \
          -ffunction-sections -fdata-sections -Os \
          -Wall -Wno-unused-parameter \
          -fno-tree-loop-distribute-patterns"   # 别把手写的搬字节循环优化成 memmove（加载器白名单里没有）
# ★ 必须显式 -lgcc：-nostdlib 会把 libgcc 一起排除，此时 -static-libgcc 只是"若链则静态"，
#   并不会把 libgcc 拉进来。用了 double/float 就会有 __muldf3 / __divdf3 / __fixdfsi 这类
#   软浮点 helper 变成未定义符号 → dlopen 报 "Can't find symbol __muldf3"（实测踩过）。
LDFLAGS="-shared -fPIC -static-libgcc -nostdlib -nostartfiles \
         -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden \
         -Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded \
         -Wl,--allow-shlib-undefined"

mkdir -p build
$GXX $CPPFLAGS -I main -I ../common -c main/terminal.cpp -o build/terminal.o
# ★ 这里**故意不链 -lgcc**：本 app 的数值全是 32 位整数（见 terminal.cpp 顶部），
#   不需要 __muldf3 这类软浮点 helper。反过来，一旦链 -lgcc，本机链接器会在收尾时
#   "double free or corruption" 崩掉（信号 6）——虽然 .so 其实写出来了，但不吃这种产物。
#   真要用 double/float 时：必须显式 -lgcc 且写在 .o 之后，并接受上面那个崩溃。
$GXX $LDFLAGS build/terminal.o -o build/terminal.so
echo "OK: build/terminal.so  ($(stat -c%s build/terminal.so) bytes)"

# ★ 「编译通过 ≠ 能加载」：elf_loader 的 libc 是逐符号白名单，漏一个只在 dlopen 那一刻炸。
#   这里把未定义符号全部列出来，宿主导出的 so_* 之外的都必须在这张白名单里。
ALLOW="so_psram_alloc so_psram_free so_snprintf so_list_books so_read_book so_list_fonts \
so_cfg_load so_cfg_save so_font_load so_font_builtin so_font_free so_font_set_size \
so_font_measure so_font_measure_prefix so_font_draw so_font_line_height so_font_size \
so_font_has so_font_ink_bounds so_font_text_ink \
so_battery_level so_is_charging so_cpu_load so_mem_used_pct so_mem_size_kb so_mem_free_kb \
so_clock_hhmm so_clock_valid \
so_fill_rect so_set_epd_mode so_display \
so_sh_exec so_ime_open so_ime_set_sticky so_ime_band_y so_bar_set so_wait_display so_fb_save so_fb_restore so_touch so_now_ms so_log so_log_file so_exit \
memset memcpy strlen strcmp strchr strrchr strtol strtod strtol strcspn strncat strerror \
puts putchar fputc fputs printf vfprintf fprintf fwrite \
usleep sleep exit close malloc calloc realloc free clock_gettime strftime \
pthread_create pthread_attr_init pthread_attr_setstacksize"
BAD=0
for s in $($NM -u build/terminal.so | awk '{print $NF}' | sort -u); do
    case "$s" in
        # ★ 只跳过真正的链接期伪符号。**绝不能把 __* 一律跳过** —— __muldf3/__divdf3 这类
        #   libgcc 软浮点 helper 就是这么漏过去的（实测：编译链接全过，dlopen 才炸）。
        _GLOBAL_OFFSET_TABLE_|__bss_start|_edata|_end|__stack|__init_array*|__fini_array*) continue ;;
        \$*) continue ;;
    esac
    ok=0
    for a in $ALLOW; do [ "$s" = "$a" ] && ok=1 && break; done
    if [ $ok -eq 0 ]; then echo "  ✗ 白名单外符号: $s  → dlopen 会失败"; BAD=1; fi
done
[ $BAD -eq 0 ] && echo "白名单核对通过（无白名单外未定义符号）"
exit $BAD
