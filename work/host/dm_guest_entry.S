// 宿主入口 —— aarch64 汇编部分（切栈进客体）。
//
// 与 dm/host/entry.S 的 _dm_thread_enter 同一套做法（那份是 D2R 的实现，本题照抄其形态）：
//   dm_guest_enter(entry, arg, stack_top)
//     x0 = 要调的函数（guest_main）
//     x1 = 传给它的参数
//     x2 = 客体栈顶（高地址端）
//
// 为什么要切栈：
//   ① 客体以为自己在它自己的栈上 —— 它的栈检查、栈上对象布局都按那个范围算；
//     留在宿主线程的栈上，客体一碰栈边界就会踩到宿主的东西。
//   ② x30 必须是「返回即错误」的哨兵 —— Horizon 的线程必须经 ExitThread 退出，
//     入口函数返回就是 bug。所以 adr x30, 1f; 让 1: 去报告。
//
// !! 本文件只有 aarch64 目标会汇编 !!（macOS/iOS 上就是 arm64）
#if defined(__aarch64__) || defined(_M_ARM64)

    .text
    .globl _dm_guest_enter
    .p2align 2
_dm_guest_enter:
    mov     sp, x2                  // 切到客体栈（高地址端）
    mov     x29, #0                 // 帧指针清零：客体的栈回溯到此为止
    mov     x16, x0                 // x16 = 要调的函数（IP0，可自由使用）
    mov     x0, x1                  // 第一个参数
    adr     x30, 1f                 // 返回点 = 哨兵（返回即错误）
    br      x16                     // 跳进客体入口
1:  bl      _dm_thread_returned     // 客体入口返回了 —— 报告
    brk     #0                      // 真的不该到这里

#endif
