#!/bin/bash
# 在电脑上跑排版引擎的回归测试（不碰设备）
#   用法：./run.sh [书文件名...]        默认跑一本小书；传全部三本要几分钟
#   BOOK_DIR 默认 = 项目里的 books/
set -e
cd "$(dirname "$0")"
HERE="$(pwd)"
BOOK_DIR="${BOOK_DIR:-$(cd "$HERE/../../.." && pwd)/books}"

GXX="${GXX:-g++}"
mkdir -p build
$GXX -std=c++20 -O2 -w \
     -I ../main -I .. -I ../../common \
     host_test.cpp stub_so.cpp \
     -o build/host_test

echo "BOOK_DIR=$BOOK_DIR"
BOOK_DIR="$BOOK_DIR" ./build/host_test "$@"
