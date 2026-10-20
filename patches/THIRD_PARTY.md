# 第三方代码与数据 —— 来源与许可证

本项目自己的代码之外，用到/引用了下面这些第三方内容。
**第三方代码本身不进仓库**（见 [`patches/README.md`](patches/README.md)），由
`./tools/fetch_components.sh` 按需还原。

## 代码（由脚本拉取）

| 组件 | 上游 | 版本 | 许可证 | 拉取方式 |
|---|---|---|---|---|
| M5GFX | https://github.com/m5stack/M5GFX | 0.2.29 | MIT | ESP 组件 registry |
| M5Unified | https://github.com/m5stack/M5Unified | 0.2.22 | MIT | ESP 组件 registry |
| elf_loader | https://github.com/espressif/esp-iot-solution (`components/elf_loader`) | 1.3.3 | Apache-2.0 | ESP 组件 registry |
| cmake_utilities | https://github.com/espressif/esp-iot-solution (`tools/cmake_utilities`) | 0.5.3 | Apache-2.0 | ESP 组件 registry |
| stb_truetype.h | https://github.com/nothings/stb | v1.26 | Public Domain (MIT/Unlicense) | GitHub tarball |

这五个都在 `components/` 或 `main/third_party/` 下落地，**由 `tools/fetch_components.sh` 拉**，
不在 git 里。本地对它们的改动见 `patches/`（5 个文件的 diff）。

> ⚠️ 拉下来的第三方代码仍受**它们自己的许可证**约束 —— MIT / Apache-2.0 都要求保留
> 版权声明和许可证文本（脚本会连同 `LICENSE` / `license.txt` 一起拉下来）。

## 数据文件（**在仓库里**）

| 文件 | 来源 | 说明 |
|---|---|---|
| `tools/junda_freq.txt` | 「现代汉语常用字频表」，原始出处见该文件头部注释（mandarintools.com 的 CEDICT 词频表） | GB2312 编码，供 `tools/make_pinyin_index.py` 给候选字排序。**上游原站已 404**，所以这份副本必须留在仓库里，否则拼音候选排序会退化成按码位排。 |

## 字体（**不在仓库里**）

字体文件有独立的版权，**不进仓库**（`.gitignore` 已排除 `fonts/*.ttf`）。
用到的字体及其授权：

| 字体 | 项目 | 授权 |
|---|---|---|
| 鸿蒙 HarmonyOS Sans SC | https://developer.huawei.com/consumer/cn/design/harmonyos-font/ | 免费商用，但**不可修改字库本身、不可单独售卖** |
| Maple Mono CN | https://github.com/subframe7536/maple-font | OFL-1.1 |
| Smiley Sans（得意黑） | https://github.com/atelier-anchor/smiley-sans | OFL-1.1 |

> 鸿蒙字库的裁剪版严格说算衍生作品 —— 自用/内部测试没问题，**对外发布前找官方确认**。
> 详见 `docs/display/TTF_FONT.md` 末尾。

## 书籍内容（**不在仓库里**）

`books/` 下是版权方的小说文本，仅供本地测试，不进仓库。

## 一句话

**仓库里的代码都是本项目的**；第三方代码要么由脚本拉，要么是明确标注的第三方数据文件
（且带来源说明）。改动上游组件的步骤见 `patches/README.md`。
