// ttf_impl.cpp — stb_truetype 的唯一定义点
//
// 原来 STB_TRUETYPE_IMPLEMENTATION 定义在 ttf.h 里，任何 include ttf.h 的
// 编译单元都会生成一份 stbtt_* 实现 —— 单 TU 时没事，plugin.cpp 也要用
// FontCtx 后就成了两个 TU 各一份，链接期重复定义。
// 所以把实现挪到这里，ttf.h 只 include 头拿声明（STBTT_DEF = extern）。
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
