# 核对「Windows 侧的编译清单」与「Mac 侧 project.yml 的源文件清单」是否一致。
#
# 为什么要有这个脚本：两边不一致会导致「Windows 上能编、Mac 上缺文件」——
# 这类问题最难查（因为在我这边一切正常）。我不该把这个核对交给朋友。
import os, re, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT = r'D:\d2r-mobile'
WIN  = os.path.join(ROOT, r'work\tools\build_all.ps1')
MAC  = os.path.join(ROOT, r'work\ios\project.yml')

# ---- Windows 侧：build_all.ps1 里的 $SRC 列表 ----
win_srcs = []
s = open(WIN, encoding='utf-8').read()
m = re.search(r'\$SRC\s*=\s*@\((.*?)\)\s*\|', s, re.S)
if m:
    for q in re.findall(r"'([^']+)'", m.group(1)):
        win_srcs.append(q)
print('=== Windows 侧 $SRC（%d 个）===' % len(win_srcs))
for f in win_srcs: print('    ' + f)

# ---- Mac 侧：project.yml 里 sources 的 ../host/* 路径 ----
mac_srcs = []
y = open(MAC, encoding='utf-8').read()
for q in re.findall(r'-\s*path:\s*\.\./host/(\S+)', y):
    mac_srcs.append(q)
print()
print('=== Mac 侧 project.yml（%d 个）===' % len(mac_srcs))
for f in mac_srcs: print('    ' + f)

# ---- 核对 ----
# Windows 侧不含 .S（汇编不参与 x86 编译），所以比对时把汇编单独处理
mac_asm  = [f for f in mac_srcs if f.endswith('.S')]
mac_c    = [f for f in mac_srcs if not f.endswith('.S')]
win_set  = set(win_srcs)
mac_set  = set(mac_c)

only_win = sorted(win_set - mac_set)
only_mac = sorted(mac_set - win_set)

print()
print('=' * 74)
print('核对结果')
print('=' * 74)

# 有意只在 Mac 侧编译的文件（非 aarch64 上不产生代码，或本来就是 Apple 专用）。
# 关键点：**它们在 Windows 上仍必须能「语法通过」** —— 所以下面单独做一次
# 「只编译不链接」的检查（`cl /c`），确保没有语法错。
MAC_ONLY_OK = {
    'hle_nvn_guest.c':      '只有 aarch64 分支产生代码；非 aarch64 是空实现',
    'hle_plat_aarch64.c':   '同上（pthread 键 / 线程返回 / 门分发适配）',
    'hle_nvn_mtl_apply.m':  'Objective-C + Metal，本来就只能 Apple 编译',
}

print('  Windows 侧源文件: %d（C/H，.S 不参与）' % len(win_set))
print('  Mac 侧 C 源文件  : %d（另有 %d 个 .S 汇编）' % (len(mac_set), len(mac_asm)))
print()
if only_win:
    print('  !! 只在 Windows 侧有（Mac 上会缺）:')
    for f in only_win: print('       ' + f)
else:
    print('  只在 Windows 侧的: （无）')

real_only_mac = [f for f in only_mac if f not in MAC_ONLY_OK]
if only_mac:
    print('  只在 Mac 侧有的 %d 个:' % len(only_mac))
    for f in only_mac:
        why = MAC_ONLY_OK.get(f, '**没有说明 —— 需要核实**')
        print('       %-24s %s' % (f, why))
if real_only_mac:
    print('  !! 其中没有正当理由的: %s' % ', '.join(real_only_mac))

print()
print('  .S 汇编（只在 Mac 上汇编）: %s' % (', '.join(mac_asm) or '（无）'))

# ---- 关键补充：Mac-only 的 C 文件在 Windows 上至少语法要过 ----
print()
print('=' * 74)
print('Mac-only 的 C 文件在 Windows 上的语法检查（cl /c，不链接）')
print('=' * 74)
import subprocess, tempfile
cl = None
for cand in ('cl.exe', 'cl'):
    try:
        r = subprocess.run([cand, '/?'], capture_output=True, timeout=20)
        cl = cand; break
    except Exception:
        continue
if not cl:
    print('  跳过：当前环境没有 cl（需要先 . .\\work\\porting\\dm_env.ps1）')
    syntax_bad = []
else:
    syntax_bad = []
    for f in MAC_ONLY_OK:
        if not f.endswith('.c'): continue
        p = os.path.join(ROOT, 'work', 'host', f)
        if not os.path.exists(p): continue
        with tempfile.TemporaryDirectory() as td:
            r = subprocess.run([cl, '/nologo', '/W3', '/utf-8', '/c',
                                '/D_CRT_SECURE_NO_WARNINGS',
                                '/Fo' + os.path.join(td, 'x.obj'), p],
                               capture_output=True, timeout=120)
        # !! cl 在中文 Windows 上打的是 GBK !! 不能按 utf-8 严格解 ——
        # 上一版用 text=True 让 Python 按默认编码解，抛 UnicodeDecodeError 把脚本打断了。
        # 按字节收，再 replace 解码，最稳。
        out = (r.stdout or b'').decode('utf-8', 'replace') + \
              (r.stderr or b'').decode('utf-8', 'replace')
        ok = (r.returncode == 0)
        print('  %-24s %s' % (f, '语法通过' if ok else '**有错**'))
        if not ok:
            syntax_bad.append(f)
            for line in out.split('\n'):
                if 'error' in line.lower():
                    print('       ' + line.strip())
                    break

# ---- 顺带核对：project.yml 引用的每个文件都真实存在 ----
print()
print('=' * 74)
print('project.yml 引用的文件是否都存在')
print('=' * 74)
missing = []
ios_dir = os.path.join(ROOT, r'work\ios')
for q in re.findall(r'-\s*path:\s*([^\.\s][^\s]*)', y):
    p = os.path.join(ios_dir, q)
    if not os.path.exists(p): missing.append(q)
for q in mac_srcs:
    p = os.path.join(ROOT, 'work', 'host', q)
    if not os.path.exists(p): missing.append('../host/' + q)
if not os.path.exists(os.path.join(ios_dir, 'Info.plist')): missing.append('Info.plist')
if missing:
    print('  缺 %d 个:' % len(missing))
    for f in missing: print('       ' + f)
else:
    print('  全部存在（%d 个 C 源 + %d 个 .S + Info.plist）' % (len(mac_c), len(mac_asm)))

# ---- 汇总 ----
print()
print('=' * 74)
bad_total = bool(only_win or real_only_mac or missing or syntax_bad)
print('核对结果: %s' % ('**有 %d 类问题**' % sum(map(bool, (only_win, real_only_mac, missing, syntax_bad)))
                     if bad_total else '全部通过'))
print('=' * 74)
sys.exit(1 if bad_total else 0)
