# patches/ — 第三方组件的本地改动

这个目录存的是**本地对第三方库的改动**（diff 形式）。

## 为什么要它

`components/` 下这四个是**第三方库**，不是本项目的代码：

| 组件 | 来源 | 许可证 |
|---|---|---|
| `m5gfx` | m5stack/M5GFX v0.2.29 | MIT |
| `m5unified` | m5stack/M5Unified v0.2.22 | MIT |
| `elf_loader` | espressif/esp-iot-solution v1.3.3 | Apache-2.0 |
| `cmake_utilities` | espressif/esp-iot-solution v0.5.3 | Apache-2.0 |

它们加起来 **165MB / 1000+ 文件**。如果直接提交进 git，仓库就变成一份几百 MB 的
第三方代码副本 —— 而且每次上游更新都要手工 diff 一遍。

所以：**第三方代码不进仓库，仓库只留改动本身**（几十 KB 的 patch）。

还原方式：

```bash
./tools/fetch_components.sh        # 拉上游 zip + 打补丁，30 秒
idf.py build
```

## 关键约束：不能交给组件管理器自动拉

这些组件**必须**放在 `components/` 下（本地组件优先级最高），**不能**在
`main/idf_component.yml` 里声明成 registry 依赖。

原因（`main/idf_component.yml` 顶部有详细版）：组件管理器会按 `dependencies.lock`
把上游版本重新下载覆盖，补丁就没了。实测后果是 `elf_loader` 的符号表补丁被冲掉，
`.so` 加载时 `dlsym` 返回 PSRAM 数据总线地址 → `Guru Meditation:
InstructionFetchError`（PC/EXCVADDR 都在 `0x3c28xxxx`），launcher 里打开任何 `.so`
立刻退回主页。

`dependencies.lock` 里记的是**绝对路径**（组件管理器生成的），换机器就是死路径。
所以它被 `.gitignore` 排除，`fetch_components.sh` 跑完会顺手删掉，下次 build 自动重生成。

## 目录结构

```
patches/
├── m5gfx/
│   ├── Panel_EPD.cpp.patch     ← EPD 波形换表钩子 + 4bpp 区域存取 + 仿射快路径
│   └── Panel_EPD.hpp.patch
├── m5unified/
│   ├── Touch_Class.cpp.patch   ← 只读原始触点 + I2C 互斥
│   └── Touch_Class.hpp.patch
└── elf_loader/
    └── esp_elf.c.patch         ← 符号表地址也要 elf_remap_text
```

## 各处改动是什么

### `m5gfx/Panel_EPD.{cpp,hpp}` —— EPD 波形换表 + 区域存取

| 改动 | 为什么 |
|---|---|
| `reload_lut_table()` / `lut_alloc_steps()` | 运行时换显示波形（不用刷固件）。上游只认 `init_intenal()` 里那套固定波形表。 |
| `_lut_alloc_steps`（多留 16 行余量） | 上游的 `_lut_2pixel` 按**原始**步数分配。运行时装一块更大的波形表 → 越界 → `reload_lut_table()` 拒绝 → 屏幕停在旧画面，看起来像白屏死机。 |
| `copy_region_out/in()` | 4bpp 区域**逐位**存取。走公开 API（`readRect`+`pushImage`）会被面板的 RGB565→4bit 有序抖动改写：只有灰阶 0/8/15 能原样往返，1–7 和 9–14 各裂成两级 → 弹窗「存底+恢复」会花。 |
| `copy_region_out/in_fast()` | 与上面**逐字节等价**的快路径。原版每像素重算旋转/翻转 + 一次乘法，全屏 540×960 要 ~199ms。四个变换在 (i,j) 上是仿射的，所以只要每行起点 + 每像素增量。`plugin.cpp` 里有开机自检（两个版本 memcmp，不等就不用快版）。 |
| `build_lut_table()` | 从 `config_detail()` 的五张波形表摊平成 2 像素查表。 |
| `isDrawing()` | 真正判断「波形跑完没」的标志。`displayBusy()` 查的是**更新队列满没满** —— `display()` 一返回队列就空了 → 循环秒退，实测推屏耗时从 873ms 变成假的 2ms。 |
| 推屏任务钉**创建它的核** | 上游写的是「创建核 +1」。UI 独占 core1 之后，epd 再跑到 core1 就是跟 UI 抢同一个核。 |

### `m5unified/Touch_Class.{cpp,hpp}` —— 触摸只读 + 互斥

| 改动 | 为什么 |
|---|---|
| `readRawOnly()` | 只读原始点，**不推进状态机**。用 `update()` 会把固件页面依赖的 `wasPressed()` 状态提前消费掉 → 页面按不动。 |
| `initThreadSafety()` + 递归互斥 | 主循环的 `M5.update()` 和 core1 触摸采样任务都会调 `update()`，触摸链路的 I2C 没有任何互斥 → 并发传输。加锁后串行化。 |

### `elf_loader/esp_elf.c` —— 符号表也要 remap

上游只对「入口点」和「重定位」做了 `elf_remap_text`，**唯独符号表这段漏了**。
于是 `dlsym()` 返回 PSRAM 的**数据总线地址**（`0x3cxxxxxx`），一调用就崩。

补丁给符号表地址补上 `elf_remap_text`（`#ifdef CONFIG_ELF_LOADER_CACHE_OFFSET` 保护，
只在 ESP32-S2/S3 + PSRAM 加载时生效）。

## 重新生成补丁

改了 `components/` 下的文件之后：

```bash
# 1. 先确认上游版本号没变（tools/fetch_components.sh 里的 COMPONENTS 数组）
# 2. 重新生成某个组件的补丁
UP=/tmp/_reg/m5stack__m5gfx-v0.2.29      # 上游 zip 解压出来的目录
diff -u "$UP/src/lgfx/v1/platforms/esp32/Panel_EPD.cpp" \
        components/m5gfx/src/lgfx/v1/platforms/esp32/Panel_EPD.cpp \
  | sed '1s|.*|--- a/src/lgfx/v1/platforms/esp32/Panel_EPD.cpp|; 2s|.*|+++ b/src/lgfx/v1/platforms/esp32/Panel_EPD.hpp|' \
  > patches/m5gfx/Panel_EPD.cpp.patch
```

更省事的做法（自动改写路径头 + 验证往返一致）：

```bash
python3 - <<'EOF'
import subprocess, os
ROOT="/home/brejce/Project/m5ui"; UP="/tmp/_reg/m5stack__m5gfx-v0.2.29"
name, rel, flat = "m5gfx", "src/lgfx/v1/platforms/esp32/Panel_EPD.cpp", "Panel_EPD.cpp"
out = subprocess.run(["diff","-u",f"{UP}/{rel}",f"{ROOT}/components/m5gfx/{rel}"],
                     capture_output=True,text=True).stdout
lines=[]
for ln in out.splitlines(True):
    if   ln.startswith("--- "): lines.append(f"--- a/{rel}\n")
    elif ln.startswith("+++ "): lines.append(f"+++ b/{rel}\n")
    else:                       lines.append(ln)
os.makedirs(f"{ROOT}/patches/{name}", exist_ok=True)
open(f"{ROOT}/patches/{name}/{flat}.patch","w").writelines(lines)
print("wrote", f"patches/{name}/{flat}.patch")
EOF
```

改完**必须**验证往返一致（补丁打到新拉的上游上，结果要和 `components/` 里的完全一样）：

```bash
rm -rf /tmp/_v && mkdir -p /tmp/_v && unzip -q <上游zip> -d /tmp/_v
patch -p1 -d /tmp/_v < patches/m5gfx/Panel_EPD.cpp.patch
diff -r /tmp/_v/... components/m5gfx/...     # 必须无差异
```

## 升级上游版本时

1. 改 `tools/fetch_components.sh` 里的 `COMPONENTS` 数组（版本号）
2. `./tools/fetch_components.sh --force`
3. 按上面「重新生成补丁」重新出 patch
4. `idf.py build` 验证
5. **看上游有没有把我们的补丁合进去** —— 合了就删掉对应的 patch

> ⚠️ 上游 M5GFX 已经到 0.2.34（本地 vendor 的是 0.2.29）。升级前先在
> `epd_lut_lab/` 那种实验目录里验证，别直接动线上版本。
