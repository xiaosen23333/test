#!/usr/bin/env python3
# 把改写后的模块映像写成一个苹果格式的动态库 Guest.framework/Guest：**只装段**，
# 没有符号、没有重定位、没有任何游戏之外的代码。
#
# ============================================================================
# 为什么必须这么做（第 50 轮 —— 真机实测得出的结论）
# ============================================================================
# iOS **不允许**普通 App 执行「运行时写进内存」的代码。我们实测过：
#   · 匿名页 mmap(RW) → mprotect(R+X)：mprotect 返回成功，但**一执行进程就被内核干掉**
#     （无信号、无看门狗、日志停在那一行）
#   · 这正是「客体第一条指令就没了」的真因（与调 _init 还是 _init_libc1 无关）
# 正确做法（照 D2R 的做法，见本仓库 dm/tools/guest_dylib.py 与 dm/host/loader.c 的注释）：
#   **把模块映像放进一个会被代码签名的 Mach-O 里**（iOS 上是一个 dylib；用户侧的
#   自签工具会给整包签名），运行时只做 `vm_remap`：把**已签名**的代码页 remap 到
#   原来的相对位置（copy=FALSE，共享同一批 r-x 页），把可写部分 remap 成写时复制。
#   于是：代码页从没被写过、本来就是签名的可执行页 → 可以执行。
#
# 节布局（与装载器的约定**逐字节对应**，改一边必须改另一边）：
#   __TEXT,__dm<i>x0     原 text：[0, 圆整16K(TEXT_END))
#   __DM<i>W,__img       可写段：[圆整16K(TEXT_END), PATCH_OFF)
#   __TEXT,__dm<i>x1     补丁区：[PATCH_OFF, 圆整16K(映像尾))
# 其中 <i> 是模块下标（0..4），顺序必须与 work/host/hle_nvn_boot.c 里
# g_mod_names[] 的顺序**完全一致**；装载器会用节长度与 fixups 对照，不一致就报错。
#
# 文件格式上要注意的三点（照抄 D2R 实测结论）：
#   · 必须声明依赖 /usr/lib/libSystem.B.dylib，否则系统拒绝加载；
#   · 每个节按 16K 对齐、整页大小（装载器按整页 vm_remap）；
#   · __LINKEDIT 放最后并留在文件末尾 —— 签名工具把签名追加在它后面。
#
# 用法：guest_dylib.py <改写产物目录> <输出目录> [模块名...]
#       输出 <输出目录>/Guest.framework/{Guest,Info.plist}
#       模块名省略时用默认的 5 个（顺序即节下标）
import hashlib, os, struct, sys

PG = 0x4000                      # Apple 的页大小（arm64 iOS 就是 16 KB）
MIN_IOS = 15 << 16               # 与本工程的最低系统版本一致（ios/project.yml: iOS 15.0）

# 默认模块顺序 —— !! 必须与 hle_nvn_boot.c 的 g_mod_names[] 一致 !!
DEFAULT_MODS = ['rtld', 'sdk', 'main', 'NmplDLL.nro', 'lz4.nro']


def r16(x): return (x + PG - 1) & ~(PG - 1)


def s16(s):
    b = s.encode()
    assert len(b) <= 16, '节/段名超 16 字节: %s' % s
    return b.ljust(16, b'\0')


def segment(name, addr, size, fileoff, filesize, prot, sects):
    b = struct.pack('<II16sQQQQiiII', 0x19, 72 + 80 * len(sects), s16(name),
                    addr, size, fileoff, filesize, prot, prot, len(sects), 0)
    for sn, a, sz, flags in sects:
        # 文件偏移 = 地址；对齐 2^14（16 KB）
        b += struct.pack('<16s16sQQIIIIIIII', s16(sn), s16(name), a, sz, a, 14,
                         0, 0, flags, 0, 0, 0)
    return b


def dylib_cmd(cmd, name):
    name += b'\0' * (8 - len(name) % 8)
    return struct.pack('<IIIIII', cmd, 24 + len(name), 24, 2, 0x10000, 0x10000) + name


def build(out, names):
    text, wsegs, plan = [], [], []
    for i, n in enumerate(names):
        hdr = dict(l[2:].split() for l in open(os.path.join(out, n + '.fixups.tsv'))
                   if l.startswith('# '))
        # !! 本工程的 fixups.tsv 里这些值是**十进制**（与 loader.c 的解析一致）；
        #    D2R 的版本用十六进制 —— 照抄过来时这里会撞 assert，别改错方向。 */
        num = lambda v: int(v, 16) if v.lower().startswith('0x') else int(v)
        te, po = num(hdr['TEXT_END']), num(hdr['PATCH_OFF'])
        img = open(os.path.join(out, n + '.patched.bin'), 'rb').read()
        sz = len(img)
        a, end = r16(te), r16(sz)
        assert po % PG == 0 and a <= po <= end, (n, hex(te), hex(po), hex(sz))
        cut = lambda lo, hi: img[lo:min(hi, sz)].ljust(hi - lo, b'\0')
        # 下标命名：模块名太长（NmplDLL.nro 就 11 字节）放不进 16 字节的节名
        text += [('__dm%dx0' % i, cut(0, a)), ('__dm%dx1' % i, cut(po, end))]
        wsegs.append(('__DM%dW' % i, cut(a, po)))
        plan.append((i, n, te, a, po, end))
    off = PG                       # 第一页：文件头 + 装载命令
    tsects = []; body = b''
    for sn, b in text:
        tsects.append((sn, off, len(b), 0x80000400))    # 纯指令节
        body += b; off += len(b)
    cmds = [segment('__TEXT', 0, off, 0, off, 5, tsects)]
    for sn, b in wsegs:
        cmds.append(segment(sn, off, len(b), off, len(b), 3, [('__img', off, len(b), 0)]))
        body += b; off += len(b)
    strtab = b'\0' * 8
    uuid = hashlib.sha256(body).digest()[:16]   # 由内容定：同输入在哪台机器上都逐字节相同
    cmds += [
        segment('__LINKEDIT', off, PG, off, len(strtab), 1, []),
        dylib_cmd(0xd, b'@rpath/Guest.framework/Guest'),                 # LC_ID_DYLIB
        struct.pack('<II10I', 0x80000022, 48, *([0] * 10)),              # LC_DYLD_INFO_ONLY
        struct.pack('<IIIIII', 0x2, 24, off, 0, off, len(strtab)),       # LC_SYMTAB（0 符号）
        struct.pack('<II18I', 0xb, 80, *([0] * 18)),                     # LC_DYSYMTAB
        struct.pack('<II16s', 0x1b, 24, uuid),                           # LC_UUID
        struct.pack('<IIIIII', 0x32, 24, 2, MIN_IOS, MIN_IOS, 0),        # LC_BUILD_VERSION（platform 2 = iOS）
        dylib_cmd(0xc, b'/usr/lib/libSystem.B.dylib'),                   # LC_LOAD_DYLIB
    ]
    lc = b''.join(cmds)
    assert 32 + len(lc) <= PG, '装载命令放不进第一页'
    mh = struct.pack('<IiiIIIII', 0xfeedfacf, 0x0100000c, 0, 6,
                     len(cmds), len(lc), 0x00100085, 0)              # arm64 动态库
    return (mh + lc).ljust(PG, b'\0') + body + strtab, plan


INFO_PLIST = '''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>Guest</string>
<key>CFBundleIdentifier</key><string>%s</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>Guest</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>1.0</string>
<key>CFBundleVersion</key><string>1</string>
<key>MinimumOSVersion</key><string>%s</string>
<key>CFBundleSupportedPlatforms</key><array><string>iPhoneOS</string></array>
</dict></plist>
'''


def write(out, dst, names):
    fw = os.path.join(dst, 'Guest.framework')
    os.makedirs(fw, exist_ok=True)
    data, plan = build(out, names)
    with open(os.path.join(fw, 'Guest'), 'wb') as f:
        f.write(data)
    with open(os.path.join(fw, 'Info.plist'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(INFO_PLIST % ('local.dm.guest', '15.0'))
    # 顺带写一份清单：人工核对「节下标 ↔ 模块名 ↔ 段边界」时不用去猜
    with open(os.path.join(fw, 'sections.tsv'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('# idx\tmodule\ttext_end\tr16_text_end\tpatch_off\tr16_size\n')
        for i, n, te, a, po, end in plan:
            f.write('%d\t%s\t0x%x\t0x%x\t0x%x\t0x%x\n' % (i, n, te, a, po, end))
    return fw, len(data), plan


if __name__ == '__main__':
    args = sys.argv[1:]
    if len(args) < 2:
        sys.exit(__doc__ or '用法：guest_dylib.py <改写产物目录> <输出目录> [模块名...]')
    src, dst = args[0], args[1]
    mods = args[2:] or DEFAULT_MODS
    fw, n, plan = write(src, dst, mods)
    print('写出 %s：%d 字节（%.2f MB），%d 个模块' % (fw, n, n / 1048576.0, len(plan)))
    for i, name, te, a, po, end in plan:
        print('   #%d %-12s text[0,0x%x) W[0x%x,0x%x) patch[0x%x,0x%x)'
              % (i, name, a, a, po, po, end))
