# 量一下：客体到底需要多大的「线程区」。
#
# 背景（第 46 轮）：
#   改写器把客体的 x18 / tpidr_el0 / tpidrro_el0 三种访问都改成「读宿主线程槽」：
#       mrs x18, tpidrro_el0   →  x18 = *(宿主tpidrro_el0 + 键号K18*8)
#   也就是说：**宿主必须往这 4 个槽里填「指针」**，客体才能拿到它的线程区。
#   而宿主从来没填过（全仓库只有 hle_nn_thread.c 里给 nn::os 线程簿记用的 tls_set，
#   用的是另一批键）—— 于是客体第一次用 x18 就会读到 NULL → 闪退。
#
#   那"该填什么"呢？本脚本从**未改写的原模块**里量出客体实际访问的偏移范围：
#     · x18-相对访问的偏移 → 客体想要的 ThreadLocalRegion 大小
#     · tpidr_el0-相对访问的偏移 → 客体想要的 TLS 块大小
#   这样定大小就不是猜的。
import os, re, struct, sys, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

EXE = r'D:\d2r-mobile\work\extract\game\ExeFS'
MODS = [('main', os.path.join(EXE, 'main')),
        ('sdk',  os.path.join(EXE, 'sdk')),
        ('rtld', os.path.join(EXE, 'rtld'))]

MRS_TPIDRRO = 0xD53BD060        # mrs xN, tpidrro_el0
MRS_TPIDR   = 0xD53BD040        # mrs xN, tpidr_el0
X18         = 18

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

def regs_in(s):
    return set(int(m) for m in re.findall(r'[xw](\d+)', s or ''))

def scan(path):
    data = open(path, 'rb').read()
    rec = {
        'tpidrro_reads': 0, 'tpidr_reads': 0, 'x18_mrs': 0,
        'off_x18': collections.Counter(),      # x18-相对访存的立即数偏移
        'off_tls': collections.Counter(),      # tpidr_el0-相对
        'idx_x18': 0, 'idx_tls': 0,            # [x18, xM] 这种索引访问（需要知道数组大小）
        'add_x18': collections.Counter(),
        'size': len(data),
    }
    n = len(data) // 4
    for i in range(n):
        p = i * 4
        w = struct.unpack_from('<I', data, p)[0]
        kind = None
        if (w & 0xFFFFFFE0) == MRS_TPIDRRO:
            kind = 'tpidrro'; base = w & 0x1F; rec['tpidrro_reads'] += 1
            if base == X18: rec['x18_mrs'] += 1
        elif (w & 0xFFFFFFE0) == MRS_TPIDR:
            kind = 'tpidr';   base = w & 0x1F; rec['tpidr_reads'] += 1
        else:
            continue
        # 往后看 24 条指令，找用这个基址的访存
        code = data[p:p + 4 * 24]
        for ins in md.disasm(code, p):
            s = ins.op_str or ''
            if not s.startswith('[') and '[' not in s:
                continue
            # 形如 ldr x0, [x18, #0x10] / str x1, [x8, x9, lsl #3]
            m = re.search(r'\[(x\d+)([^\]]*)\]', s)
            if not m:
                continue
            rb = int(m.group(1)[1:])
            if rb != base:
                continue
            rest = m.group(2)
            key = 'off_x18' if kind == 'tpidrro' else 'off_tls'
            if '#' in rest:
                imm = int(re.search(r'#(-?0x[0-9a-fA-F]+|-?\d+)', rest).group(1).replace('0x', ''), 16) \
                      if '0x' in rest else int(re.search(r'#(-?\d+)', rest).group(1))
                rec[key][imm] += 1
            else:
                rec['idx_x18' if kind == 'tpidrro' else 'idx_tls'] += 1
            if ins.mnemonic in ('add', 'sub'):
                mm = re.search(r'#(-?0x[0-9a-fA-F]+|-?\d+)', s)
                if mm and re.search(r'\[x%d' % base, s) is None:
                    pass
    return rec

print('=' * 78)
print('客体线程区的实际需求（从未改写的原模块里量）')
print('=' * 78)
for name, path in MODS:
    if not os.path.exists(path):
        print('  %s 不存在，跳过' % path); continue
    r = scan(path)
    print()
    print('--- %s（%d 字节）---' % (name, r['size']))
    print('  mrs tpidrro_el0: %d 处（其中直接写 x18 的 %d 处）' % (r['tpidrro_reads'], r['x18_mrs']))
    print('  mrs tpidr_el0  : %d 处' % r['tpidr_reads'])
    for key, label in (('off_x18', 'x18/ThreadLocalRegion 相对访存'), ('off_tls', 'tpidr_el0/TLS 相对访存')):
        c = r[key]
        if not c:
            print('  %s: 没量到' % label); continue
        mx = max(c); mn = min(c)
        show = ', '.join('%+d×%d' % (k, v) for k, v in sorted(c.items())[:12])
        print('  %s: %d 处，偏移范围 %+d .. %+d' % (label, sum(c.values()), mn, mx))
        print('        常见: %s' % show)
    if r['idx_x18'] or r['idx_tls']:
        print('  索引式访问（[base, xM]）: x18 %d 处 / tpidr %d 处' % (r['idx_x18'], r['idx_tls']))

print()
print('=' * 78)
print('结论')
print('=' * 78)
print('  宿主必须给每个线程准备两块内存并把**指针**写进 4 个线程槽：')
print('    K18 / KTLR -> ThreadLocalRegion（大小按上面量到的最大偏移向上取整）')
print('    KTLS       -> TLS 块（同上）')
print('    KGATE      -> &_dm_gate（entry.S 里的门入口；不填则第一次 svc 就跳 NULL）')
