/* 门入口 _dm_gate —— 与 dm/host/entry.S 同构。
 *
 * 客体执行到门块 → 门块把宿主门地址取到 x30 并 blr → 进入这里。
 * 进来时 lr（= x30）= 门块的返回点（ldp 的地址），它就是**门的唯一标识**。
 *
 * 本文件把 x0..x30 / q0..q31 / nzcv,fpsr,fpcr 全部保存到栈上连续区，
 * 然后以「x0 = 返回点，x1 = regs 指针」调用 C 侧的 dm_gate_dispatch_c，
 * 返回后把所有寄存器恢复（门可以改任意通用寄存器 —— svc 出参在 x0..x7，
 * cntpct 的目标寄存器实测有 x19..x23，所以 x0..x29 都要回读）。
 *
 * 注意 x18：苹果平台把它当保留寄存器，客体的 x18 已被改写器中立化到线程槽，
 * 所以这里**跳过 x18 不回读**（与 D2R 一致）。
 *
 * 平台：macOS / iOS（aarch64）。Windows 上没有 aarch64 执行环境，
 * 因此本文件不参与 Windows 侧的自测（自测直接喂 regs 给 C 侧分发函数）。
 *
 * !! 链接期注意 !! 这里调的两个名字都带下划线前缀（Mach-O 的 C 符号约定），
 * 而 C 侧的实现叫 dm_gate_dispatch / dm_thread_enter。适配层在
 * hle_plat_aarch64.c 里（dm_gate_dispatch_c 就是适配）。
 * 我第一版没有这个适配层 —— Mac 上链接会报 `_dm_gate_dispatch_c` 未定义。
 */

    .text
    .globl  _dm_gate
    .p2align 2
_dm_gate:
    /* 800 字节帧：x0..x30（32 槽 256 字节）+ q0..q31（32 槽 256 字节）
       + nzcv/fpsr/fpcr（3 槽 24 字节）—— 合计 536，取 800 留余量 */
    sub     sp, sp, #800
    stp     x0,  x1,  [sp, #0]
    stp     x2,  x3,  [sp, #16]
    stp     x4,  x5,  [sp, #32]
    stp     x6,  x7,  [sp, #48]
    stp     x8,  x9,  [sp, #64]
    stp     x10, x11, [sp, #80]
    stp     x12, x13, [sp, #96]
    stp     x14, x15, [sp, #112]
    stp     x16, x17, [sp, #128]
    stp     x18, x19, [sp, #144]
    stp     x20, x21, [sp, #160]
    stp     x22, x23, [sp, #176]
    stp     x24, x25, [sp, #192]
    stp     x26, x27, [sp, #208]
    stp     x28, x29, [sp, #224]
    str     x30,      [sp, #240]
    stp     q0,  q1,  [sp, #256]
    stp     q2,  q3,  [sp, #288]
    stp     q4,  q5,  [sp, #320]
    stp     q6,  q7,  [sp, #352]
    stp     q8,  q9,  [sp, #384]
    stp     q10, q11, [sp, #416]
    stp     q12, q13, [sp, #448]
    stp     q14, q15, [sp, #480]
    stp     q16, q17, [sp, #512]
    stp     q18, q19, [sp, #544]
    stp     q20, q21, [sp, #576]
    stp     q22, q23, [sp, #608]
    stp     q24, q25, [sp, #640]
    stp     q26, q27, [sp, #672]
    stp     q28, q29, [sp, #704]
    stp     q30, q31, [sp, #736]
    mrs     x9, nzcv
    str     x9, [sp, #768]
    mrs     x9, fpsr
    str     x9, [sp, #776]
    mrs     x9, fpcr
    str     x9, [sp, #784]

    /* 第 79 轮：把「进门那一刻的客体 SP」存进 regs[31]（= [sp,#248]，
     * 也就是 x30 之后那个空槽；800 字节帧里这一段原本没人用）。
     *
     * !! 为什么非存不可 !!
     * 客体调门走的是**门 thunk**（改写器生成的 12 条指令），它长这样：
     *     stp x29,x30,[sp,#-16]!     ← 真正的 x29/x30 存在这里
     *     mrs x30, tpidrro_el0 / adrp x29 / ... / ldr x30,[x30] / ldr x29,[x29]
     *     blr x30                    ← 进 _dm_gate；这一步还把 x30 改成了 thunk 内部地址
     * 也就是说：**进到 _dm_gate 时 x29/x30 都已经被 thunk 改写过了**，
     * regs[29] 不是帧指针、regs[30] 是 thunk 自己的地址（实测一直显示
     * sdk+0xc79558，就是这个原因）—— 于是「谁调的 Abort」永远查不出来。
     *
     * 存下进门 SP 后，宿主就能从 [SP+0] / [SP+8] 取回 thunk 保存的**真 x29/x30**，
     * 再顺着帧链回溯出完整调用链（hle.c 的 svc 0x26 分支用）。
     *
     * 放在所有寄存器保存**之后**：这时 x9 已经存进栈了，可以随便用。 */
    add     x9, sp, #800
    str     x9, [sp, #248]

    mov     x0, x30             /* 返回点 = 门标识 */
    mov     x1, sp              /* regs */
    bl      _dm_gate_dispatch_c

    ldr     x9, [sp, #784]
    msr     fpcr, x9
    ldr     x9, [sp, #776]
    msr     fpsr, x9
    ldr     x9, [sp, #768]
    msr     nzcv, x9
    ldp     q0,  q1,  [sp, #256]
    ldp     q2,  q3,  [sp, #288]
    ldp     q4,  q5,  [sp, #320]
    ldp     q6,  q7,  [sp, #352]
    ldp     q8,  q9,  [sp, #384]
    ldp     q10, q11, [sp, #416]
    ldp     q12, q13, [sp, #448]
    ldp     q14, q15, [sp, #480]
    ldp     q16, q17, [sp, #512]
    ldp     q18, q19, [sp, #544]
    ldp     q20, q21, [sp, #576]
    ldp     q22, q23, [sp, #608]
    ldp     q24, q25, [sp, #640]
    ldp     q26, q27, [sp, #672]
    ldp     q28, q29, [sp, #704]
    ldp     q30, q31, [sp, #736]

    /* 回读 x0..x17、x19..x29（跳过 x18：苹果平台保留，客体已中立化） */
    ldp     x0,  x1,  [sp, #0]
    ldp     x2,  x3,  [sp, #16]
    ldp     x4,  x5,  [sp, #32]
    ldp     x6,  x7,  [sp, #48]
    ldp     x8,  x9,  [sp, #64]
    ldp     x10, x11, [sp, #80]
    ldp     x12, x13, [sp, #96]
    ldp     x14, x15, [sp, #112]
    ldp     x16, x17, [sp, #128]
    ldr     x19,      [sp, #152]
    ldp     x20, x21, [sp, #160]
    ldp     x22, x23, [sp, #176]
    ldp     x24, x25, [sp, #192]
    ldp     x26, x27, [sp, #208]
    ldp     x28, x29, [sp, #224]
    ldr     x30,      [sp, #240]
    add     sp, sp, #800
    ret

/* ------------------------------------------------------------------ */
/* 客体线程起点 —— 切到 SDK 给的客体栈，以 x0 = 参数调入口。            */
/* ------------------------------------------------------------------ */
/* dm_thread_enter(entry, arg, sp)：
 *     x0 = 客体线程入口
 *     x1 = 参数
 *     x2 = 客体栈顶（高地址端）
 *
 * 与 dm/host/entry.S 的 _dm_thread_enter 同构：
 *   · 切栈（客体以为自己在它自己的栈上）
 *   · x29 清零（栈回溯到此为止）
 *   · x30 设成哨兵 —— 客体线程必须走 ExitThread，入口返回就是错误
 */
    .globl  _dm_thread_enter
    .p2align 2
_dm_thread_enter:
    mov     sp, x2
    mov     x29, #0
    mov     x16, x0
    mov     x0, x1
    adr     x30, 1f
    br      x16
1:  bl      _dm_thread_returned
    brk     #0
