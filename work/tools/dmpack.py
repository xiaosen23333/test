# 出包工具 —— 把本项目的产物组织成一个可安装的 ipa。
#
# 前提（诚实说明）：
#   那层「壳」（不含游戏内容的 ipa）**必须在 macOS + Xcode 上先构建出来**。
#   这不是本脚本能替代的步骤 —— 壳里是**已编译的宿主程序 + 界面素材**，
#   而宿主是平台相关的（aarch64 + Metal + UIKit）。
#
# 所以本脚本做两件事：
#   ① 组织产物成一个 app 包目录（这部分 Windows 上就能做、也能核对）
#   ② 如果给了壳 ipa，就把内容塞进去、重打成 ipa；没给就只产出包目录
#
# 用法：
#   python dmpack.py --staging <stage 目录> [--shell <壳.ipa>] [--out <输出.ipa>]
#   python dmpack.py --staging <stage 目录> --manifest     # 只列清单（核对用）
import argparse, hashlib, json, os, shutil, struct, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
DM_ROOT = os.path.dirname(HERE)                       # work
PROJ = os.path.dirname(DM_ROOT)                        # D:\d2r-mobile

# 改写后的 5 个模块（改写器产物）
MODULES = ['main', 'sdk', 'rtld', 'NmplDLL.nro', 'lz4.nro']
# 每个模块需要随附的元数据（装载器要读 fixups.tsv 与 gates.tsv）
MODULE_META = ['fixups.tsv', 'gates.tsv']

def say(s): print('[dmpack] %s' % s, flush=True)
def die(s): sys.exit('[dmpack] 失败: ' + s)

def sha256_file(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()

# ---------------------------------------------------------------- 清单

def build_manifest(patcher_out, shader_dir, ios_dir):
    """列出应该进包的所有文件，并核对每个都在。"""
    items = []   # (包内相对路径, 源路径, 说明)
    def add(inner, src, note):
        items.append((inner, src, note))

    # ① 改写后的 5 个模块 + 元数据
    for m in MODULES:
        add('dm/%s.patched.bin' % m, os.path.join(patcher_out, m + '.patched.bin'),
            '改写后的模块')
        for meta in MODULE_META:
            p = os.path.join(patcher_out, '%s.%s' % (m, meta))
            if os.path.exists(p):
                add('dm/%s.%s' % (m, meta), p, '装载器元数据')

    # ② 着色器：映射表 + Metal 源码
    smap = os.path.join(shader_dir, 'shader_map.tsv')
    if os.path.exists(smap):
        add('dm/shader_map.tsv', smap, '着色器映射表（运行期按 key 查）')
    metal = os.path.join(shader_dir, 'metal')
    if os.path.isdir(metal):
        for f in sorted(os.listdir(metal)):
            if f.endswith('.metal'):
                add('dm/metal/' + f, os.path.join(metal, f), 'Metal 着色器源码')

    # ③ iOS 外壳源码（Mac 上编译用；不进包，但列出来提醒还缺它）
    for f in ('main.m', 'front_ios.m', 'Info.plist'):
        p = os.path.join(ios_dir, f)
        if os.path.exists(p):
            items.append(('@source/' + f, p, 'iOS 外壳源码（Mac 上编译）'))

    return items

def print_manifest(items):
    n_in = [x for x in items if not x[0].startswith('@')]
    n_src = [x for x in items if x[0].startswith('@')]
    print()
    print('=== 进包文件（%d 个）===' % len(n_in))
    total = 0
    by_kind = {}
    for inner, src, note in n_in:
        sz = os.path.getsize(src) if os.path.exists(src) else -1
        total += max(0, sz)
        by_kind[note] = by_kind.get(note, 0) + 1
    for k, v in sorted(by_kind.items(), key=lambda x: -x[1]):
        print('  %-32s %d 个' % (k, v))
    print('  合计 %.1f MB' % (total / 1048576.0))
    print()
    print('=== 源码文件（需在 Mac 上编译，不进包）===')
    for inner, src, note in n_src:
        print('  %-24s %s' % (inner.replace('@source/', ''), note))
    print()
    missing = [(i, s) for i, s, _ in n_in if not os.path.exists(s)]
    return missing

# ---------------------------------------------------------------- 组织包目录

def stage(manifest, out_dir):
    if os.path.isdir(out_dir):
        say('清空并重建 %s' % out_dir)
        shutil.rmtree(out_dir)
    os.makedirs(out_dir)
    n = 0
    for inner, src, note in manifest:
        if inner.startswith('@'):
            continue
        dst = os.path.join(out_dir, inner.replace('/', os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        n += 1
    say('已放入 %d 个文件 -> %s' % (n, out_dir))
    return n

# ---------------------------------------------------------------- 打 ipa

def wrap_ipa(stage_dir, shell_ipa, out_ipa):
    """把 stage_dir 的内容塞进壳 ipa 的 Payload/<App>.app/ 下，重打成 ipa。"""
    if not os.path.exists(shell_ipa):
        die('壳 ipa 不存在: %s' % shell_ipa)
    with zipfile.ZipFile(shell_ipa) as z:
        names = z.namelist()
    # 找 app 目录
    app_dir = None
    for n in names:
        if n.startswith('Payload/') and '.app/' in n:
            app_dir = n.split('.app/')[0] + '.app'
            break
    if not app_dir:
        die('壳 ipa 里找不到 Payload/*.app/ —— 它可能不是 ipa')
    say('壳里的 app 目录: %s' % app_dir)
    say('壳里已有 %d 个条目' % len(names))

    # 保留壳里的一切，额外加入我们的内容
    with zipfile.ZipFile(shell_ipa) as zin, \
         zipfile.ZipFile(out_ipa, 'w', zipfile.ZIP_DEFLATED) as zout:
        for item in zin.infolist():
            zout.writestr(item, zin.read(item.filename))
        added = 0
        for root, _dirs, files in os.walk(stage_dir):
            for f in files:
                p = os.path.join(root, f)
                rel = os.path.relpath(p, stage_dir).replace(os.sep, '/')
                zout.write(p, app_dir + '/' + rel)
                added += 1
    say('加了 %d 个文件 -> %s (%.1f MB)' % (
        added, out_ipa, os.path.getsize(out_ipa) / 1048576.0))
    return added

# ---------------------------------------------------------------- 入口

def main():
    ap = argparse.ArgumentParser(description='把本项目的产物组织成可安装的 ipa')
    ap.add_argument('--patcher-out', default=os.path.join(DM_ROOT, 'patcher', 'out'),
                    help='改写器产物目录（*.patched.bin / fixups.tsv / gates.tsv）')
    ap.add_argument('--shader-dir', default=os.path.join(DM_ROOT, 'shaders2'),
                    help='着色器产物目录（shader_map.tsv / metal/）')
    ap.add_argument('--ios-dir', default=os.path.join(PROJ, 'work', 'ios'),
                    help='iOS 外壳源码目录')
    ap.add_argument('--staging', default=os.path.join(DM_ROOT, 'ipastage'),
                    help='组织出的包目录')
    ap.add_argument('--shell', default=None, help='壳 ipa（**需要先在 Mac 上构建**）')
    ap.add_argument('--out', default=None, help='输出 ipa 路径')
    ap.add_argument('--manifest', action='store_true', help='只列清单，不做别的')
    a = ap.parse_args()

    say('产物目录: %s' % a.patcher_out)
    say('着色器目录: %s' % a.shader_dir)
    say('iOS 源码: %s' % a.ios_dir)

    items = build_manifest(a.patcher_out, a.shader_dir, a.ios_dir)
    missing = print_manifest(items)
    if missing:
        print('=== 缺文件（%d 个）===' % len(missing))
        for i, s in missing[:20]:
            print('  %-40s <- %s' % (i, s))
        die('先补齐上面的产物（检查改写器与着色器链是否都跑过）')

    if a.manifest:
        return 0

    stage(items, a.staging)

    if a.shell:
        out = a.out or (a.staging + '.ipa')
        wrap_ipa(a.staging, a.shell, out)
    else:
        print()
        print('=' * 68)
        print('没有提供壳 ipa（--shell），所以只组织出了包目录，没有打 ipa。')
        print()
        print('还缺的那一步必须在 macOS + Xcode 上做：')
        print('  ① 用 Xcode 编译 work/ios/{main.m, front_ios.m} 与 work/host/*.c')
        print('     （宿主是 aarch64 + Metal + UIKit 的平台代码，Windows 上编不了）')
        print('  ② 连同 Info.plist 与图标打出「不含游戏内容的壳 ipa」')
        print('  ③ 再回到任意系统上跑：')
        print('     python %s --shell <壳.ipa> --out <输出.ipa>' % os.path.basename(__file__))
        print('=' * 68)
    return 0

if __name__ == '__main__':
    sys.exit(main())
