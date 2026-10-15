# 量「客体到底需要什么」—— 在**解压后的真实映像**上做（上一次扫的是压缩数据，白扫了）。
#
# 为什么需要：改写器把客体的三种线程相关访问都改成「读宿主填的线程槽」：
#     x18（musl 的线程指针）      → 槽 K18
#     mrs/msr tpidr_el0           → 槽 KTLS
#     mrs tpidrro_el0             → 槽 KTLR
#   槽里必须放**指针**，指向宿主为每个线程准备的内存。宿主从来没填过 → 客体读到 NULL → 崩。
#
# 本脚本给出两个数字（定大小不能靠猜）：
#   · x18 / tpidr_el0 相对访存的最大偏移 → TLS 块要多大
#   · tpidrro_el0 相对访存的最大偏移     → ThreadLocalRegion 要多大
# 外加一个关键确认：客体取 TLS 变量时**会不会去调 TLSDESC 的 resolver**
# （loader.c 把 resolver 填成 0；若会调，那就是 `blr 0`，必崩）。
import os, re, struct, sys, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
import lz4.block
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

EXE = r'D:\d2r-mobile\work\extract\game\ExeFS'
EXE2 = r'D:\d2r-mobile\work\extract\game'          # NRO 在别处时备用
TARGETS = [('main', os.path.join(EXE, 'main')),
           ('sdk',  os.path.join(EXE, 'sdk')),
           ('rtld', os.path.join(EXE, 'rtld'))]

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
# !! 必须开 skipdata !!
# 映像开头是 ELF/NSO 头，不是代码。Capstone 的 disasm() 遇到第一条**无法解码**的字
# 就**停下**（不报错、也不继续）—— 上一版就是这样：整段扫出来 0 条指令，
# 我还以为「客体不用 x18」。开 skipdata 后它会跳过一个字继续扫。
md.skipdata = True
MRS_TPIDRRO = 0xD53BD060
MRS_TPIDR   = 0xD53BD040

def load_nso(path):
    d = open(path, 'rb').read()
    if d[:4] == b'NSO0':
        flags = struct.unpack_from('<I', d, 0x0C)[0]
        segs = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
        bss = struct.unpack_from('<I', d, 0x3C)[0]
        csz = struct.unpack_from('<III', d, 0x60)
        img = bytearray(segs[2][1] + segs[2][2] + bss)
        for i, (fo, mo, sz) in enumerate(segs):
            raw = lz4.block.decompress(d[fo:fo + csz[i]], uncompressed_size=sz) if (flags >> i & 1) else d[fo:fo + sz]
            img[mo:mo + sz] = raw
        return bytes(img), segs[0][2]
    return d, len(d)

def imm_of(s):
    m = re.search(r'#(-?0x[0-9a-fA-F]+|-?\d+)', s)
    if not m:
        return None
    t = m.group(1)
    return int(t, 16) if t.startswith(('0x', '-0x')) else int(t)

def probe(name, path):
    img, text_end = load_nso(path)
    print()
    print('--- %s（解压后 %d 字节，text_end=0x%x）---' % (name, len(img), text_end))
    # ① 全量扫 x18 相对访存
    x18_offs = collections.Counter()
    x18_idx = 0
    mrs_tls = []
    mrs_ro = []
    blr_from_mem = 0
    seq_hits = []
    code = img[:text_end]
    ins_list = list(md.disasm(code, 0))          # 一次性反汇编整段（内存够）
    for ins in ins_list:
        s = ins.op_str or ''
        if '[' in s:
            m = re.search(r'\[(x\d+)([^\]]*)\]', s)
            if m and m.group(1) == 'x18':
                rest = m.group(2)
                if '#' in rest:
                    v = imm_of(rest)
                    if v is not None:
                        x18_offs[v] += 1
                else:
                    x18_idx += 1
    # ② 找 mrs
    for i, ins in enumerate(ins_list):
        w = struct.unpack_from('<I', code, ins.address)[0] if ins.address + 4 <= len(code) else 0
        if (w & 0xFFFFFFE0) == MRS_TPIDRRO:
            mrs_ro.append((ins.address, w & 0x1F))
        elif (w & 0xFFFFFFE0) == MRS_TPIDR:
            mrs_tls.append((ins.address, w & 0x1F))
    # ③ TLSDESC 的标准调用形状（adrp/ldr/add/blr），允许中间夹 0~1 条无关指令
    for i in range(len(ins_list) - 5):
        a, b, c, d = ins_list[i], ins_list[i+1], ins_list[i+2], ins_list[i+3]
        if not (a.mnemonic == 'adrp' and b.mnemonic in ('ldr','ldur')
                and c.mnemonic == 'add' and d.mnemonic in ('blr','br')):
            continue
        ra = re.match(r'(x\d+)', a.op_str)
        rb = re.search(r'\[(x\d+)', b.op_str)
        rc = re.search(r'(x\d+), (x\d+)', c.op_str)
        rd = re.match(r'(x\d+)', d.op_str)
        if not (ra and rb and rc and rd):
            continue
        if ra.group(1) == rb.group(1) == rc.group(1) and rd.group(1) == re.match(r'(x\d+)', b.op_str).group(1):
            seq_hits.append(a.address)

    print('  mrs tpidrro_el0 : %d 处；mrs tpidr_el0 : %d 处' % (len(mrs_ro), len(mrs_tls)))
    print('  x18 相对访存     : %d 处（其中索引式 [x18, xM] %d 处）' % (sum(x18_offs.values()), x18_idx))
    if x18_offs:
        mx = max(x18_offs); mn = min(x18_offs)
        print('     偏移范围 %+d .. %+d ；最常见的 10 个: %s'
              % (mn, mx, ', '.join('%+d×%d' % kv for kv in x18_offs.most_common(10))))
    # tpidrro 相对：只看紧跟 mrs 的窗口
    ro_offs = collections.Counter()
    for addr, rt in mrs_ro:
        idx = addr // 4
        for ins in ins_list[idx+1: idx+25]:
            s = ins.op_str or ''
            m = re.search(r'\[(x\d+)([^\]]*)\]', s)
            if not m:
                continue
            base = int(m.group(1)[1:])
            if base != rt:
                continue
            if '#' in m.group(2):
                v = imm_of(m.group(2))
                if v is not None:
                    ro_offs[v] += 1
    if ro_offs:
        print('  tpidrro 相对访存 : 偏移范围 %+d .. %+d（%d 处）'
              % (min(ro_offs), max(ro_offs), sum(ro_offs.values())))
    tp_offs = collections.Counter()
    for addr, rt in mrs_tls:
        idx = addr // 4
        for ins in ins_list[idx+1: idx+25]:
            s = ins.op_str or ''
            m = re.search(r'\[(x\d+)([^\]]*)\]', s)
            if not m:
                continue
            base = int(m.group(1)[1:])
            if base != rt:
                continue
            if '#' in m.group(2):
                v = imm_of(m.group(2))
                if v is not None:
                    tp_offs[v] += 1
    if tp_offs:
        print('  tpidr_el0 相对访存: 偏移范围 %+d .. %+d（%d 处）'
              % (min(tp_offs), max(tp_offs), sum(tp_offs.values())))
    print('  TLSDESC 标准调用序列（adrp/ldr/add/blr）: %d 处' % len(seq_hits))
    if seq_hits[:5]:
        print('     例如 0x%x' % seq_hits[0])
    return dict(x18_offs=x18_offs, ro=ro_offs, tp=tp_offs, seq=len(seq_hits))

print('=' * 78)
print('客体线程区需求（**解压后**的真实映像）')
print('=' * 78)
res = {}
for name, path in TARGETS:
    if not os.path.exists(path):
        print('  %s 不存在' % path); continue
    try:
        res[name] = probe(name, path)
    except Exception as e:
        print('  %s 失败: %s: %s' % (name, type(e).__name__, e))

print()
print('=' * 78)
print('结论')
print('=' * 78)
allx = collections.Counter()
for r in res.values():
    allx.update(r['x18_offs'])
if allx:
    n = max(allx) + 64
    n = (n + 0xFFF) & ~0xFFF
    print('  TLS 块（K18/KTLS）至少要覆盖到 %+d → 建议分配 %d (0x%x) 字节' % (max(allx), n, n))
tot_seq = sum(r['seq'] for r in res.values())
print('  TLSDESC 标准调用序列: 合计 %d 处 → %s'
      % (tot_seq, '**客体确实会 blr 描述符里的 resolver**，desc[0]=0 就是 blr 0，必崩'
         if tot_seq else '没找到 —— 需另找取值方式'))
