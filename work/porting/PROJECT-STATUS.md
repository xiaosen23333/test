# 项目状态与交接文档

日期：第 35 轮（一键全量回归建立）
仓库：`D:\d2r-mobile`（照 D2R Mobile 的思路移植《静谧田园 / Village in the Shade》到 iPad）

---

## 一、一句话状态

> **Windows 侧的全部工作已完成并逐项验证（16 个自测全过）。
> 剩下的是三件必须用 macOS + Xcode 做的事。**

---

## 二、一键验证

```powershell
# 在仓库根目录
. .\work\porting\dm_env.ps1        # 注入 MSVC 环境
& .\work\tools\build_all.ps1       # 编译全部宿主源码 + 跑全部 16 个自测
```

**当前结果**：

```
boot_selftest            ALL PASS      loader_selftest          ALL PASS
host_selftest            ALL PASS      nn_selftest              ALL PASS
entry_selftest           ALL PASS      tlsdesc_selftest         ALL PASS
libc_init2_selftest      ALL PASS      nvn_selftest             ALL PASS
nvn_obj_selftest         ALL PASS      nvn_metal_selftest       ALL PASS
nvn_shader_selftest      ALL PASS      glslc_selftest           ALL PASS
glslc_invariant_selftest ALL PASS      replay_selftest          ALL PASS
mtl_selftest             ALL PASS      fmt_selftest             ALL PASS

=== 产物清单核对 ===   5 个改写模块齐全   着色器映射表: 126 行
=== 全部通过（编译 16 个，通过 16 个）===
```

退出码 0 = 全通过。**任何人拿到这份仓库，一条命令就能确认它是活的。**

---

## 三、已完成（Windows 侧，全部有验证）

### 3.1 抽取与体检

| 项 | 结果 |
|---|---|
| 游戏 | 《静谧田园 / Village in the Shade》title `01001BA027DD8000` |
| 引擎 | 日本一（Nippon Ichi）自研 **Nmpl** |
| 模块 | ExeFS `main`/`sdk`/`rtld` + RomFS `/NmplDLL.nro` + `/lz4.nro` |
| 图形接口 | **NVN**（538 个函数名，与 D2R 的 523 个重合 **96.8%**） |
| 离线可行性 | ✅ 无网络服务/域名/网络命名空间 |

### 3.2 改写器

| 项 | 结果 |
|---|---|
| 站点总数 | **22,067**（5 个模块） |
| 完成度 | **100%，0 跳过** |
| x18 等价性 | 19,823 / 19,823 |
| cond/pcrel | 429 / 429 |
| br_x18（跳转表） | 9 / 9 |
| MRS 编码 | 18,795 / 18,795 |

### 3.3 装载器

| 项 | 结果 |
|---|---|
| 重定位 | **24,660 条全部应用** |
| **未解析符号** | **0**（glslc 8 条经兜底解析器，TLSDESC 62 个已填） |
| 门表 | 107 条（5 个模块合并排序） |
| 键页 / 线程槽 | 10 / 10 |

### 3.4 宿主 HLE

| 项 | 结果 |
|---|---|
| `nn::` 符号面 | **238 / 238（100%）** |
| 核心子系统 | `nn::os`（46）与 `nn::fs`（27）**均 100%** |
| 存档 | **端到端验证**：建文件 → 写 16 字节 → 读回逐字节一致 |
| 线程/TLS/信号量/事件/计时 | 全部行为验证 |
| 输入 | `dm_input_set_touch` 等接口就绪（iPad 触摸接入点） |

### 3.5 图形层

| 层 | 结果 |
|---|---|
| NVN 接口层 | ✅ 槽表 / 分发 / 注册 / `nvnBootstrapLoader` |
| NVN 对象层 | ✅ 53 个实现（device/pool/buffer/texture/window/queue） |
| 命令录音机 | ✅ 26 个命令类（带 idx 的薄包装，不丢身份） |
| 命令回放器 | ✅ 顺序/参数/分类/清空/未知身份全部核对 |
| **Metal 编码层状态机** | ✅ 9 项（去重 / pass 分组 / 一致性检查） |
| 格式映射 | ✅ **107 条** NVNformat → MTLPixelFormat |
| 枚举映射 | ✅ **49 个取值**（含偏移规律与两处例外） |
| 着色器查找桥 | ✅ **12 / 12 命中，逐字节一致** |
| glslc 接管 | ✅ 8 个符号；**已证客体从不调用**（只需非 NULL） |

### 3.6 宿主入口

| 项 | 结果 |
|---|---|
| `dm_host_boot` | ✅ 端到端验证（装载 → 重定位 → 门 → RomFS → NVN → 着色器表） |
| 客体入口 | ✅ **`nnMain` @ main + 0x9aec30**（按名字从 dynsym 取） |
| 启动计划 | ✅ **5 / 5**：`_init_libc1` → `_init` → `nnosInitialize` → `nninitStartup` → `nnMain` |

---

## 四、未完成（全部需要 macOS + Xcode）

| # | 项 | 说明 |
|---|---|---|
| 1 | **Metal API 调用层** | `work/host/hle_nvn_mtl_apply.m`，3 个 TODO 已标清 |
| 2 | **iOS 外壳编译** | `work/ios/{main.m,front_ios.m,Info.plist}` 已写好 |
| 3 | **出壳** | 编译出「不含游戏内容的 ipa」（约 4 MB） |

### 之后回到任意系统（含 Windows）

```powershell
python work\tools\dmpack.py --shell <壳.ipa> --out <输出.ipa>
```

然后爱思助手自签装机，再把 `RomFS/data` 的 18 个 `.data` 拷进 App 的 Documents。

### 为什么必须 Mac

- Metal 需要 `Metal.framework` 与 `MTLCommandBuffer` 等真类型
- iOS 外壳需要 UIKit，且**壳里是已编译的宿主程序** —— 而宿主是 aarch64 + Metal 代码
- D2R 的壳对本游戏无效（壳里是 D2R 专属的宿主与内容核对表）

---

## 五、产物清单

| 类别 | 位置 | 规模 |
|---|---|---|
| 改写后的模块 | `work/patcher/out/*.patched.bin` | 5 个，36.8 MB |
| 着色器 | `work/shaders2/metal/*.metal` | 116 个 + 映射表 126 条 |
| 宿主源码 | `work/host/*.c/.h/.m` | **41 个文件，6,326 行** |
| 宿主自测 | `work/host/tests/*.c` | 19 个，2,013 行 |
| iOS 外壳 | `work/ios/` | 3 个文件，310 行 |
| 工具脚本 | `work/porting/*.py` | ~110 个 |
| 报告 | `work/porting/*REPORT*.md` | 每轮一份，含错误复盘 |

---

## 六、这个项目里我犯过的错（52+ 次，分类）

| 类别 | 次数 | 代表 |
|---|---|---|
| **先猜结构、不查「谁用它」** | 2 次大弯路 | 第 28 轮在 MOD0 猜入口（该查 dynsym）；第 30~32 轮把 glslc 当硬阻塞（该先查它会不会被调用） |
| 平台差异 | 5 次 | MSVC 不支持嵌套函数 / `__int128` |
| 生成式代码三处不同步 | ~8 次 | 规则表 / 实现 / 头文件 |
| 声明与定义位置 | ~8 次 | C 里必须先声明后用 |
| **自测的判据写错** | ~5 次 | 阈值写「大约」（`>=30` 实际 27、`>=120` 实际 107） |
| **截断式查看导致误判** | 3 次 | 只读输出前几行 / `Select-Object -First` |
| 编码与转义 | 4 次 | PowerShell 无 BOM、块注释里的 `*/` |

### 最值得记的两条

1. **遇到「某个值从哪来 / 会不会被调用」，先查参考实现或调用点，再动手。**
   我两次大弯路都是跳过了这一步。
2. **凡能精确的数字，判据就用精确值。** 我用「大约」做断言，两次被自己的模糊预期绊倒。

---

## 七、诚实评估：这个目标还远吗

| 维度 | 评估 |
|---|---|
| Windows 侧技术工作 | ✅ **完成** |
| 剩余技术工作 | Metal API 层（中等）+ 外壳编译出壳（小） |
| 但 | **全部需要 Mac** |
| 装机之后 | 必然还有大量真机调试（画面、输入、性能、存档路径…） |
| D2R 的对照 | 它的宿主 23,984 行 + 图形 21,658 行 + iOS 14,466 行、7,212 个着色器 —— **是一条走了很久的路** |

**这个项目现在是「Windows 侧已备好，等 Mac」的状态。**
用户已明确说明借不到 Mac，所以接下来需要用户决定：
找一台 Mac（哪怕云 Mac 一小时），或者改用模拟器路线（Eden 的 iOS 版，用户已跑通过）。

---

## 八、本轮我犯的错（累计 56 次）

| 次数 | 我写的 | 实际 |
|---|---|---|
| 54 | `dm_host_ticks` 在 5 个自测里各定义一份 | 与 `hle_nvn_boot.c` 冲突 → LNK2005 多重定义 |
| 55 | 自测清单里名字与文件名不一致 | `libc_init2` vs `libc_init2_selftest.c` |
| 56 | PowerShell 里用正则插入 C 的 `printf("\n...")` | 转义把 `\n` 写坏 → 字符串跨行，编译不过。**改用 Python 改文件** |
| 57 | `build_all.ps1` 存成无 BOM 的 UTF-8 | PowerShell 按 GBK 读 → 中文乱码把引号吃掉 → 语法错（**与 `dm_env.ps1` 同一个坑**） |

**第 57 条我在这个项目里已经踩过一次**（`dm_env.ps1` 需要 BOM）——
说明「环境相关的约束」我没有形成检查清单。

**第 56 条的教训**：**不要用 shell 的正则去改 C 源码**（转义层级太多）。
用 Python 改文件，一次就对。
