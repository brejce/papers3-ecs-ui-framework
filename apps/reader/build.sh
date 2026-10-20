#!/bin/bash
# 编译 reader.so（手写 g++，不依赖 project_so 宏 —— 那个宏只认 .c）
#
# 用法：cd apps/reader && ./build.sh   → 产出 build/reader.so
# 再上传：curl -X POST "http://<设备IP>/api/ul?path=/apps&name=reader.so" --data-binary @build/reader.so

set -e
cd "$(dirname "$0")"
source ~/esp-env/env.sh >/dev/null 2>&1

GXX=xtensa-esp32s3-elf-g++

# 与固件 plugin.cpp 编译一致的 flags（见 elf_loader.cmake project_so 宏）
CPPFLAGS="-fPIC -fno-exceptions -fno-rtti -std=gnu++2b -fvisibility=hidden \
          -ffunction-sections -fdata-sections -Os \
          -Wall -Wno-unused-parameter"

LDFLAGS="-shared -fPIC -static-libgcc -nostdlib -nostartfiles \
         -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden \
         -Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded \
         -Wl,--allow-shlib-undefined"

mkdir -p build
$GXX $CPPFLAGS -I main -I ../common -c main/reader.cpp -o build/reader.o
$GXX $LDFLAGS build/reader.o -o build/reader.so

echo "OK: build/reader.so"
ls -la build/reader.so
