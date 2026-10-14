# 端到端流水线审计：从原始游戏文件一路核对到最终产物。
#
# 目标不是「再跑一遍」，而是**独立核对每个环节的产物与下一环节的输入对得上**，
# 并且**用密码学/结构性判据**（哈希、尺寸、魔数、条数），而不是「看起来对」。
#
# 这样做的价值：把「这套东西是活的」变成可复核的事实，而不是我的叙述。
import os, sys, json, struct, hashlib, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

ROOT  = r'D:\d2r-mobile'
GAME  = os.path.join(ROOT, r'work\extract\game')
PATCH = os.path.join(ROOT, r'work\patcher\out')
SH    = os.path.join(ROOT, r'work\shaders2')
HOST  = os.path.join(ROOT, r'work\host')
IOS   = os.path.join(ROOT, r'work\ios')

fails = []
def check(name, ok, detail=''):
    print('  %-46s %s%s' % (name, 'OK' if ok else '**不符**', ('  ' + detail) if detail else ''))
    if not ok: fails.append(name)

def sha256(p, n=None):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        d = f.read(n) if n else f.read()
        h.update(d)
    return h.hexdigest()

def size(p): return os.path.getsize(p)

print('=' * 74)
print('阶段 0：原始游戏文件')
print('=' * 74)
exefs = os.path.join(GAME, 'ExeFS')
base_nsp = None
for d in (os.path.join(ROOT, 'Game'),):
    if os.path.isdir(d):
        for f in os.listdir(d):
            if f.endswith('.nsp'):
                base_nsp = os.path.join(d, f)
if base_nsp:
    check('原始 .nsp 存在', True, '%s  %.2f GB' % (os.path.basename(base_nsp), size(base_nsp)/2**30))
else:
    check('原始 .nsp 存在', False)

for m in ('main', 'sdk', 'rtld'):
    p = os.path.join(exefs, m)
    check('ExeFS/%s' % m, os.path.exists(p), '%d 字节' % size(p) if os.path.exists(p) else '')
for m in ('NmplDLL.nro', 'lz4.nro'):
    p = os.path.join(GAME, m)
    check('%s' % m, os.path.exists(p), '%d 字节' % size(p) if os.path.exists(p) else '')
rf = os.path.join(GAME, 'romfs_filelist.txt')
check('RomFS 文件清单', os.path.exists(rf),
      '%d 个文件' % sum(1 for _ in open(rf, encoding='utf-8', errors='replace')) if os.path.exists(rf) else '')

print()
print('=' * 74)
print('阶段 1：着色器链')
print('=' * 74)
sm = os.path.join(SH, 'shader_map.tsv')
check('shader_map.tsv', os.path.exists(sm))
rows = []
if os.path.exists(sm):
    for line in open(sm, encoding='utf-8'):
        if line.startswith('#') or not line.strip(): continue
        f = line.rstrip('\n').split('\t')
        if len(f) >= 8: rows.append(f)
    check('映射表条数 = 126', len(rows) == 126, '实际 %d' % len(rows))
    check('每条都有 16 字符 key', all(len(r[2]) == 16 for r in rows))
    stages = collections.Counter(r[1] for r in rows)
    check('阶段分布 vs=64 / ps=62', stages.get('vs') == 64 and stages.get('ps') == 62, str(dict(stages)))

mdir = os.path.join(SH, 'metal')
metals = sorted(f for f in os.listdir(mdir)) if os.path.isdir(mdir) else []
check('Metal 着色器 116 个', len(metals) == 116, '实际 %d' % len(metals))
# 每个 .metal 的文件名应当是某个 key
keys = set(r[2] for r in rows)
named = set(f[:-6] for f in metals if f.endswith('.metal'))
check('.metal 文件名都对应一个 key', named <= keys, '不在表里的: %d' % len(named - keys))

# **关键核对**：用 shader_1_00.dat 的控制段重算 key，看能否命中表
dat = os.path.join(GAME, r'data\shader_1_00.dat')
if os.path.exists(dat) and rows:
    d = open(dat, 'rb').read()
    check('shader_1_00.dat 魔数 FAFULLFS', d[:8] == b'FAFULLFS', d[:8].decode('latin1'))
    hit = miss = 0
    for r in rows:
        ctrl = int(r[4])
        if ctrl + 0x880 > len(d): miss += 1; continue
        h = hashlib.sha256(d[ctrl:ctrl + 0x880]).hexdigest()[:16]
        if h == r[2]: hit += 1
        else: miss += 1
    check('控制段重算 key 命中 126/126', hit == 126 and miss == 0, '命中 %d 失配 %d' % (hit, miss))
    # 且**离线产出的每个 .metal 的 key**都能在 dat 里找到（说明不是凭空造的）
    ctrl_keys = set()
    for r in rows:
        ctrl = int(r[4])
        if ctrl + 0x880 <= len(d):
            ctrl_keys.add(hashlib.sha256(d[ctrl:ctrl+0x880]).hexdigest()[:16])
    check('.metal 的 key 都能在 dat 里找到', named <= ctrl_keys, '找不到的 %d' % len(named - ctrl_keys))

print()
print('=' * 74)
print('阶段 2：改写器产物')
print('=' * 74)
MODS = ['main', 'sdk', 'rtld', 'NmplDLL.nro', 'lz4.nro']
tot_sites = 0
for m in MODS:
    binp = os.path.join(PATCH, m + '.patched.bin')
    fixp = os.path.join(PATCH, m + '.fixups.tsv')
    gatp = os.path.join(PATCH, m + '.gates.tsv')
    sitp = os.path.join(PATCH, m + '.sites.tsv')
    ok = all(os.path.exists(p) for p in (binp, fixp, gatp, sitp))
    check('%-14s 4 个产物齐全' % m, ok)
    if not ok: continue
    # fixups 里的 key page / patch 区不与映像重叠（第 11 轮修过的那个 bug）
    fo = fo_key = None
    for line in open(fixp, encoding='utf-8'):
        if line.startswith('#'): continue
        f = line.rstrip('\n').split('\t')
        if len(f) >= 2:
            if f[0] == 'image_size': fo = int(f[1], 0)
            if f[0] == 'key_page_off': fo_key = int(f[1], 0)
    if fo is not None and fo_key is not None:
        check('%-14s key_page 在映像之后' % m, fo_key >= fo, 'image=%#x key_page=%#x' % (fo, fo_key))
    # sites.tsv 条数
    ns = sum(1 for l in open(sitp, encoding='utf-8') if l.strip() and not l.startswith('#'))
    tot_sites += ns
    # gates 条数
    ng = sum(1 for l in open(gatp, encoding='utf-8') if l.strip() and not l.startswith('#'))
    check('%-14s sites=%d gates=%d' % (m, ns, ng), ns > 0)

check('改写站点总数 = 22,067', tot_sites == 22067, '实际 %d' % tot_sites)

print()
print('=' * 74)
print('阶段 3~4：宿主源码与验证产物')
print('=' * 74)
src = [f for f in os.listdir(HOST) if f.endswith(('.c', '.h', '.m'))]
check('宿主源码存在', len(src) >= 40, '%d 个文件' % len(src))
# 关键文件
for f in ('hle.c', 'loader.c', 'hle_nvn.c', 'hle_nvn_obj.c', 'hle_nvn_replay.c',
          'hle_nvn_mtl.c', 'hle_nvn_fmt.c', 'hle_nvn_glslc.c', 'hle_nvn_boot.c',
          'nn_impl_table.c', 'nvn_names.c', 'nvn_thunks.bin', 'hle_nvn_mtl_apply.m'):
    check('  %s' % f, os.path.exists(os.path.join(HOST, f)))
# nvn_names 的表长要与跳板个数一致
nn = os.path.join(HOST, 'nvn_names.c')
if os.path.exists(nn):
    n = sum(1 for l in open(nn, encoding='utf-8') if l.strip().startswith('"nvn'))
    check('nvn_names.c 有 538 条', n == 538, '实际 %d' % n)
tb = os.path.join(HOST, 'nvn_thunks.bin')
if os.path.exists(tb):
    check('nvn_thunks.bin = 538 × 32', size(tb) == 538 * 32, '%d 字节' % size(tb))
    # 尾数一致：跳板数 == 名字表条数
    check('跳板数与名字表一致', size(tb) // 32 == n)

print()
print('=' * 74)
print('阶段 5：出包准备')
print('=' * 74)
for f in ('main.m', 'front_ios.m', 'Info.plist'):
    check('work/ios/%s' % f, os.path.exists(os.path.join(IOS, f)))
check('work/tools/dmpack.py', os.path.exists(os.path.join(ROOT, r'work\tools\dmpack.py')))
check('work/tools/build_all.ps1', os.path.exists(os.path.join(ROOT, r'work\tools\build_all.ps1')))
# 壳 ipa 是否存在（这一步是已知缺口）
shells = []
for d in (os.path.expanduser('~/Downloads'), os.path.expanduser('~/Desktop'), ROOT):
    if os.path.isdir(d):
        shells += [os.path.join(d, f) for f in os.listdir(d) if f.endswith('.ipa')]
check('壳 ipa 存在', len(shells) > 0,
      ('找到 %d 个: %s' % (len(shells), ', '.join(os.path.basename(s) for s in shells))) if shells else
      '**没有** —— 这一步需要 macOS + Xcode')

print()
print('=' * 74)
if fails:
    print('审计结果：%d 项不符 ->' % len(fails))
    for f in fails: print('    ' + f)
else:
    print('审计结果：全部核对通过')
print('=' * 74)
sys.exit(1 if fails else 0)
