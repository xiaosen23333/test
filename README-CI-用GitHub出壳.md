# 用 GitHub Actions 出「壳」——不需要自己有 Mac

这个仓库里已经放好了一条现成的工作流：`.github/workflows/ios-shell.yml`。
它会向 GitHub 借一台 macOS 机器，装 XcodeGen、生成工程、用 Xcode 编译、
打包成 **未签名的 `DmGame-shell.ipa`**，然后作为构建产物给你下载。

你这边一台 Mac 都不需要。

---

## 一、一次性准备

### 1. 在 GitHub 上建一个空仓库

打开 <https://github.com/new>：

- **Repository name**：随便，比如 `dm-ios-shell`
- **Public / Private**：
  - 选 **Public** → Actions 的 macOS 机器**完全免费、不限时长**
  - 选 **Private** → 免费额度 2,000 分钟/月，而 macOS 按 **10 倍**扣，
    等于每月约 **200 分钟** macOS 机器。这套编译一次大概 5~8 分钟，
    也就是每月能编 25~40 次，够用
- **不要**勾 "Add a README file"（保持空仓库，否则 push 会冲突）

### 2. 把代码推上去

在本仓库根目录（就是能看到 `work/` 和 `.github/` 的那一层）执行：

```bash
git remote add origin https://github.com/<你的用户名>/dm-ios-shell.git
git push -u origin main
```

`<你的用户名>` 换成你自己的。第一次 push 会弹窗让你登录 GitHub。

**推完就自动开始编译了** —— 打开仓库页面的 **Actions** 页签就能看到。

### 3. 改动之后怎么重编

改完代码：

```bash
git add -A
git commit -m "描述"
git push
```

不想改代码、只想重跑一次：**Actions** → 左边选 **Build iOS shell**
→ 右上 **Run workflow** → 选 `main` → 绿色按钮。

---

## 二、拿到「壳」

1. 进仓库 → **Actions** 页签 → 点左边 **Build iOS shell**
2. 等它跑完（绿色对勾，约 5~8 分钟；第一次装 XcodeGen 会稍慢）
3. 点进那次运行，页面**最下方**有 **Artifacts**：
   - `DmGame-shell-ipa` ← **就要这个**，下载下来解压得到 `DmGame-shell.ipa`
   - `build-log` ← 编译日志，编译失败时也要把它下载下来

### 如果失败了

点进那次运行 → 展开红色的那一步，就能看到报错。

**先看日志最末尾的「全量语法体检」** —— 那一步会把整个工程的语法错误
一次性列出来，比翻上面 `xcodebuild` 的输出快得多。

---

## 三、之后就不用再碰 GitHub 了

拿到壳之后，剩下的都在你自己的 Windows 上做：

```
① 壳（DmGame-shell.ipa）        ← GitHub Actions 给的，约 3~6 MB
② 把你手上的游戏文件塞进壳       ← Windows 上跑打包脚本，全程本地
③ 用爱思助手 / Sideloadly 自签装机 ← Windows 上做，免费 Apple ID 即可
```

所以其实就是**借一次 GitHub 的 Mac，出一只壳**，之后完全是本地流程。

---

## 四、这份工作流做了哪些「防坑」的事

| 步骤 | 为什么要有 |
|---|---|
| 打印 macOS / Xcode / SDK 版本、目录树、源文件数量 | 出问题时不用猜环境 |
| 生成工程后打印 pbxproj 里的文件数 | 立刻能看出源文件清单有没有生效 |
| **校验优化档位没写坏** | `GCC_OPTIMIZATION_LEVEL` 写成 `-O2` 会被拼成 `-O-O2`，这一关直接拦住，不用等整轮编译 |
| 编译后再逐个核对每个 `*.c/*.m/*.S` 是否真的进了编译日志 | 「清单漏了某个文件」是这类构建最难查的错，这一步会直接点名 |
| **全量语法体检** | `xcodebuild` 一报错就停，这一步用 `clang -fsyntax-only` 把所有文件扫一遍，一次性看全所有错误 |
| `lipo -info` 确认产物是 arm64 | 编成 x86_64 的话真机装不上 |
| 打完 ipa 后 `unzip -l` 列出内部结构 | 确认是规范的 `Payload/DmGame.app` |
| 无论成功失败都上传 `build.log` | 失败时日志一定拿得到 |

---

## 五、相对原说明改动的地方

### 1. `GCC_OPTIMIZATION_LEVEL` 写法 —— 第一次编译失败的原因就是它

`work/ios/project.yml` 原来写的是：

```yaml
GCC_OPTIMIZATION_LEVEL: "-O2"
```

**这是错的。** Xcode 的 `GCC_OPTIMIZATION_LEVEL` 只是**紧跟在 `-O` 后面的那一部分**，
`-O` 前缀由 Xcode 自己补，于是 clang 实际收到的是 **`-O-O2`**，直接报：

```
error: invalid integral value '-O2' in '-O-O2'
```

已改成只写档位：

```yaml
GCC_OPTIMIZATION_LEVEL: "1"   # Debug，clang 收到 -O1
GCC_OPTIMIZATION_LEVEL: "2"   # Release，clang 收到 -O2
```

合法取值只有 `0 / 1 / 2 / 3 / s / fast`。
（原文件 base 处写的是数组 `["-O1","-O2"]`，同样不合规，一并改掉了。）

这不是环境问题 —— 是 `project.yml` 在 Windows 上从没被 Xcode 读过，
所以一直没暴露。

### 2. 汇编文件的归类

`entry.S` / `dm_guest_entry.S` / `hle_nvn_mtl_apply.m` 显式加了
`buildPhase: sources`。否则 `.S` 的归类要靠 XcodeGen 猜，
万一被丢进 Resources，链接期就会报 `_dm_gate` 未定义。

### 3. 换行符

加了 `.gitattributes`（`* text=auto eol=lf`），避免 Windows 上的
`core.autocrlf` 把源码换成 CRLF 再推给 macOS。二进制文件也单独标注了。

---

## 六、出问题时最可能的原因

| 现象 | 原因 |
|---|---|
| `invalid integral value '-O2' in '-O-O2'` | 优化档位带了 `-O` 前缀，只写数字（已修） |
| `Unable to find a destination` | 已用 `-destination 'generic/platform=iOS'` 规避 |
| 某个 `../host/*.c` 找不到 | `project.yml` 的 `sources` 清单与实际目录不符 —— CI 的核对步骤会点名是哪个 |
| 一堆 `Use of undeclared identifier` | 同上，多半是漏了某个 `*.c` |
| 汇编报错 | `ARCHS` 已是 `arm64` |
| 链接报 `_dm_thread_returned` / `_gate_dispatch` / `_unimpl_c` 未定义 | 是宿主侧缺实现，不是环境问题 |
| `MTLPixelFormat` 之类未定义 | 与 Apple 头文件无关，`hle_nvn_fmt.c` 只返回整数 |

---

## 七、提醒

CI 只能证明**「这些代码能被 clang 编过」**。
`hle_nvn_mtl_apply.m` 里那 3 个 `TODO`、`hle_nvn_guest.c` 里启动序列的顺序
都还是要在真机上验的。但把「能不能编过」这一关先过掉，
就排除了最大的一类不确定性。
