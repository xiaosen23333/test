# 确认「TLSDESC 会不会真的去调 resolver」。
#
# 为什么关键：loader.c 把 TLSDESC 描述符填成
#       desc[0] = 0        ← resolver 地址
#       desc[1] = 偏移
# 如果客体走的是 AArch64 TLSDESC 的**标准调用序列**，它就会 `blr desc[0]` ——
# 也就是 `blr 0` → 跳到地址 0 → 立刻崩。
# 如果客体只是**直接读 desc[1]**（不经 resolver），那 desc[0]=0 就没有影响。
#
# 这两种情况都"说得通"，所以不能猜 —— 直接在模块里找那个指令形状：
#       adrp xN, <页>            ← 描述符所在页
#       ldr  xM, [xN, #lo12]     ← M = desc[0] = resolver
#       add  xN, xN, #lo12       ← N = &desc[1]
#       blr  xM                  ← 调 resolver
import os, re, struct, sys, collections
sys.stdout.reconfigure(encoding='utf-8', errors='replace')
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

EXE = r'D:\d2r-mobile\work\extract\game\ExeFS'
MODS = [('main', os.path.join(EXE, 'main')), ('sdk', os.path.join(EXE, 'sdk'))]
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)

def analyze(path, name):
    data = open(path, 'rb').read()
    # 只扫可执行段：用 boot_syms_selftest 报的 text_end 近似（这里直接扫全文件）
    hits = []
    for p in range(0, len(data) - 16, 4):
        blob = data[p:p + 4 * 8]
        ins = list(md.disasm(blob, p))
        if len(ins) < 4:
            continue
        a, b, c, d = ins[0], ins[1], ins[2], ins[3]
        if (a.mnemonic == 'adrp' and b.mnemonic in ('ldr', 'ldur') and
                c.mnemonic == 'add' and d.mnemonic in ('blr', 'br')):
            # 形状对了；再看寄存器是否对得上：adrp 的 Rd == ldr 的 base == add 的 Rn
            m1 = re.match(r'(x\d+), ', a.op_str)
            m2 = re.search(r'\[(x\d+)', b.op_str)
            m3 = re.search(r'(x\d+), (x\d+), ', c.op_str)
            m4 = re.match(r'(x\d+)', d.op_str)
            if m1 and m2 and m3 and m4:
                if m1.group(1) == m2.group(1) == m3.group(1):
                    ldr_rt = re.match(r'(x\d+)', b.op_str)
                    if ldr_rt and ldr_rt.group(1) == m4.group(1):
                        hits.append(p)
    return hits

print('=' * 78)
print('找 TLSDESC 的「标准调用序列」形状（adrp / ldr / add / blr）')
print('=' * 78)
total = 0
for name, path in MODS:
    if not os.path.exists(path):
        print('  %s 不存在' % path); continue
    hits = analyze(path, name)
    total += len(hits)
    print('  %-6s %d 处' % (name, len(hits)))
    for p in hits[:12]:
        print('      offset 0x%x' % p)
    if len(hits) > 12:
        print('      …（还有 %d 处）' % (len(hits) - 12))

print()
print('=' * 78)
if total:
    print('结论：**客体确实会调 resolver** —— desc[0]=0 时就是 `blr 0`，必崩。')
    print('      必须给 desc[0] 填一个真的 resolver（返回 desc[1] 的值即可）。')
else:
    print('结论：没找到这个形状 —— 客体可能用别的方式取 TLS（需要再查）。')
