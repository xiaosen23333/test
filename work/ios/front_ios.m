/* 本游戏 iOS 外壳 —— Metal 视图与呈现循环。
 *
 * 职责边界（与宿主的关系）：
 *   宿主（work/host/）已经负责：装载 5 个模块、链接、填键页、登记门、分发 nn::/NVN。
 *   本文件负责**只有 iOS 能做的事**：
 *     ① 建 CAMetalLayer 与 drawable
 *     ② 驱动呈现循环（CADisplayLink）
 *     ③ 把触摸事件转成 dm_input_set_touch（宿主已实现）
 *     ④ 生命周期（进入后台/回前台/内存警告）
 *     ⑤ 提供 RomFS 目录给 nn::fs
 *
 * !! 重要 !! 本文件**只能在 macOS + Xcode 上编译**，Windows 上无法验证。
 *   所以逻辑尽量简单、少用 Apple 的边角 API，降低「在 Mac 上才发现编译错」的风险。
 *   里面对宿主接口的调用都可以在 Windows 上静态核对（函数名与签名见 work/host/*.h）。
 */
#import <UIKit/UIKit.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
/* 下面用了 snprintf —— 显式包含，不要指望 UIKit 间接带进来 */
#include <stdio.h>
#include <stdarg.h>     /* va_list —— status_set 用它（第 43 轮加状态文字时引入） */
#include <stdint.h>     /* uintptr_t —— 崩溃处理里把 si_addr 转成整数 */
#include <stdlib.h>     /* malloc / free —— log_tail 读日志尾部时用（第 45 轮） */
#include <string.h>
#include <signal.h>     /* 崩溃捕获（第 46 轮） */
#include <ucontext.h>   /* ucontext_t：从信号上下文里取 PC/LR/SP/寄存器 */

#include "hle.h"
#include "hle_nn.h"
#include "hle_nn_more.h"
#include "hle_nvn.h"
#include "hle_nvn_obj.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_metal.h"
#include "hle_diag.h"     /* 诊断通道（hle_diag.c）：日志文件 + 一行总览 */

/* 宿主侧入口（由 work/host/ 提供） */
extern int   dm_host_boot(const char *romfs_dir, const char *save_dir);  /* 装载 + 初始化 */
extern void  dm_host_frame(void);                                        /* 走一帧 */
extern void  dm_host_shutdown(void);

/* 宿主输入接口（hle_nn_more.c 已实现，Windows 上已单测过） */
extern void dm_input_set_buttons(uint32_t b);
extern void dm_input_set_stick(int32_t lx, int32_t ly, int32_t rx, int32_t ry);
extern void dm_input_set_touch(int active, uint32_t x, uint32_t y);

/* NVN 呈现（nvn 对象层） */
extern void dm_nvn_QueueSubmitCommands(void *q, int n, const void *cmds);
extern void dm_nvn_QueuePresentTexture(void *q, void *win, int slot);

/* ------------------------------------------------------------------ */
/* Metal 视图                                                          */
/* ------------------------------------------------------------------ */

/* 用 +layerClass 返回 CAMetalLayer —— 这样 UIKit 自己管 layer 的生命周期，
   比手工 addSublayer 少一类出错可能（D2R 也是这么做的）。 */
@interface DmView : UIView
@end

@implementation DmView
+ (Class)layerClass { return CAMetalLayer.class; }
@end

static CAMetalLayer    *g_layer = nil;
static id<MTLDevice>    g_device = nil;
static CADisplayLink   *g_link = nil;
static int              g_running = 0;
static uint64_t         g_frames = 0;      /* 帧计数（状态文字显示用，见 frame_tick） */

/* ------------------------------------------------------------------ */
/* 前置声明 —— 这两行**不能删**                                        */
/* ------------------------------------------------------------------ */
/* !! 第 44 轮补的一笔账 !!
 *   clang 16 起（Xcode 16 自带的就是 16）「先调用、后声明」在 C99 及以后是
 *   **硬错误**，不是警告：
 *       error: call to undeclared function 'status_set';
 *              ISO C99 and later do not support implicit function declarations
 *   第 43 轮我给 frame_tick 加状态文字、给装载加后台线程时，把
 *   status_set 的定义写在了 frame_tick **后面**、dm_ios_start 的定义写在了
 *   调用点**后面** —— 这两处**在 Windows 上完全看不出来**（本机没有 ObjC 编译器，
 *   也没有 clang），只会在朋友的 Mac 上炸掉一整个来回。
 *   现在把它们提到最前面声明，并加了 work/porting/check_use_before_decl.py
 *   做静态扫描（推给对方之前先跑一遍）。 */
static void status_set(NSString *fmt, ...);
int         dm_ios_start(void);
const char *dm_ios_docs_dir(void);   /* 本文件下面实现；日志路径要用它 */
static void refresh_status(NSString *head);

/* ProMotion：iPhone 上 App 自己调 nextDrawable 时系统默认按 60 Hz 给画布。
   挂一个空回调的 CADisplayLink 来「申请」高刷（D2R 实测：还要在 Info.plist 里
   加 CADisableMinimumFrameDurationOnPhone）。iPad 上同样适用。

   !! 第 43 轮修正 !! 原来 CADisplayLink 的**目标**就是这个 DmRateHint，
   而它的 tick: 是空实现 —— 于是真正的 frame_tick() 从来没被调用，宿主一帧都没跑。
   高刷「申请」与「驱动帧循环」是两件事，不能共用一个空回调。
   现在帧循环由 DmFrameTick 驱动（见下），本类只保留作说明；不再创建
   （原版那条 g_rate_link 已删 —— 它是死代码，创建了却没人用它的回调）。 */
@interface DmRateHint : NSObject
- (void)tick:(CADisplayLink *)l;
@end
@implementation DmRateHint
- (void)tick:(CADisplayLink *)l { (void)l; }   /* 只表态，不做事 */
@end

/* ------------------------------------------------------------------ */
/* 触摸 → 宿主输入                                                     */
/* ------------------------------------------------------------------ */

/* 触摸坐标换算：UIKit 的点 → 渲染像素。
   注意 contentsScale 与 drawableSize 的关系：drawableSize = bounds * contentsScale。 */
static void touch_xy(UITouch *t, UIView *v, uint32_t *ox, uint32_t *oy) {
    CGPoint p = [t locationInView:v];
    CGFloat s = v.layer.contentsScale;
    if (s <= 0) s = 1.0;
    int x = (int)(p.x * s);
    int y = (int)(p.y * s);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    *ox = (uint32_t)x;
    *oy = (uint32_t)y;
}

@interface DmTouchView : DmView
@end

@implementation DmTouchView

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    (void)e;
    UITouch *t = touches.anyObject;
    if (!t) return;
    uint32_t x = 0, y = 0;
    touch_xy(t, self, &x, &y);
    dm_input_set_touch(1, x, y);
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    (void)e;
    UITouch *t = touches.anyObject;
    if (!t) return;
    uint32_t x = 0, y = 0;
    touch_xy(t, self, &x, &y);
    dm_input_set_touch(1, x, y);
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    (void)touches; (void)e;
    dm_input_set_touch(0, 0, 0);
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)e {
    (void)touches; (void)e;
    dm_input_set_touch(0, 0, 0);
}

@end

/* ------------------------------------------------------------------ */
/* 呈现循环                                                            */
/* ------------------------------------------------------------------ */

/* 一帧：让宿主跑（它会调用 NVN，命令录进录音机；并且第一次会进客体）。
   真机上 dm_host_frame 内部会：
     · 第一次调用时 dm_host_enter_guest() → 起客体启动序列
     · 之后经 gate 分发跑客体的一帧
     · 在 nvnQueueSubmitCommands 时把录音回放成 MTLCommandBuffer
     · 在 nvnQueuePresentTexture 时 presentDrawable */
static void frame_tick(void) {
    if (!g_running) return;
    @autoreleasepool {
        dm_host_frame();
        /* 帧计数：状态文字上能看出「帧循环有没有在跑」。
         * 黑屏时这一条最关键 —— 数字不动 = 帧循环没起来；数字在动 = 起来了但画面是黑的。
         * 只每 30 帧刷一次 UI（别每帧都改 UILabel，那会拖慢渲染）。
         *
         * !! 注意：如果客体（游戏）跑在主线程上不返回，**连这一句也刷不出来** ——
         *   那时候唯一的线索是 <Documents>/dm-log.txt（下次启动会显示在屏幕上）。 */
        g_frames++;
        if (g_frames % 30 == 1)
            status_set(@"帧循环运行中，第 %llu 帧", (unsigned long long)g_frames);
    }
}

/* !! 第 43 轮修正 !! 这里原来是 [DmRateHint new] 作为 CADisplayLink 的目标，
 *   而 DmRateHint 的 tick: 是**空实现**（注释写「只表态，不做事」）——
 *   于是 frame_tick() 从来没被调用，宿主一帧都没跑。
 *   现在用一个**真正驱动帧循环**的目标。
 *   （CADisplayLink 的 selector 不能带参数，所以只能这样包一层。） */
@interface DmFrameTick : NSObject
- (void)tick:(CADisplayLink *)link;
@end
@implementation DmFrameTick
- (void)tick:(CADisplayLink *)link { (void)link; frame_tick(); }
@end
static DmFrameTick *g_tick_target = nil;

/* ------------------------------------------------------------------ */
/* 状态文字（让「黑屏」变成「能看懂的提示」）                          */
/* ------------------------------------------------------------------ */
/* 为什么要它：这一版 Metal 层还没实现，画面本来就是黑的。
 * 加一行状态字能把「黑屏」这个无信息现象变成可诊断的线索：
 *   · 显示「正在装载…」→ 卡在装载
 *   · 显示「装载失败 rc=-10」→ 明确是哪个模块
 *   · 显示「已进客体，等待画面」→ 宿主起来了，问题在图形层
 * 真机排障时这比看控制台方便（不用连电脑）。
 *
 * !! 第 45 轮补的一个关键认识 !!
 *   光有屏幕文字是**不够**的：客体（游戏）跑在**主线程**上
 *   （CADisplayLink → frame_tick → dm_host_frame → dm_host_enter_guest → nnMain），
 *   它一旦不返回，主线程就卡在里面 —— 屏幕上的字**再也不会更新**，
 *   而我们最想知道的就是「它卡在哪一步」。崩溃时更是什么都看不到。
 *   所以现在加了**日志文件**（<Documents>/dm-log.txt，每条都立刻落盘），
 *   并且**每次启动会把上次运行日志的尾部直接显示在屏幕上** ——
 *   装一次、跑一次、再打开就能看到上次崩在哪，不用连电脑、不用拷文件。 */
static UILabel *g_status = nil;
static NSString *g_status_head = nil;   /* 第一行的「人话状态」 */
static NSString *g_prev_tail = nil;     /* 上次运行日志的尾部（启动时读一次） */

static const char *dm_ios_log_path(void) {
    static char buf[1300];
    if (!buf[0]) snprintf(buf, sizeof buf, "%s/dm-log.txt", dm_ios_docs_dir());
    return buf;
}

/* 读日志文件的尾部若干行。**必须在本次运行开始写日志之前调用**，
   否则读到的就是本次已经写进去的内容。 */
static NSString *log_tail(int want_lines) {
    const char *path = dm_ios_log_path();
    FILE *f = fopen(path, "rb");
    if (!f) return @"（上次没有留下日志 —— 这是第一次运行）";
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return @"（日志是空的）"; }
    long take = (sz > 4096) ? 4096 : sz;
    fseek(f, sz - take, SEEK_SET);
    char *buf = (char *)malloc((size_t)take + 1);
    if (!buf) { fclose(f); return @"（内存不足，读不了日志）"; }
    size_t got = fread(buf, 1, (size_t)take, f);
    fclose(f);
    buf[got] = 0;

    /* 从尾部往前数 want_lines 个换行 */
    long start = (long)got;
    int seen = 0;
    for (long i = (long)got - 1; i >= 0; i--) {
        if (buf[i] == '\n') { if (++seen > want_lines) break; start = i + 1; }
    }
    NSString *s = [NSString stringWithUTF8String:buf + start];
    free(buf);
    return s ?: @"（日志不是合法 UTF-8）";
}

/* 组装并刷新屏幕上那块文字。
 * 三部分：人话状态（head）/ 一行总览（客体走到哪 + NVN 计数）/ 上次运行的日志尾部 */
static void refresh_status(NSString *head) {
    if (head) g_status_head = head;
    char d[400];
    dm_host_diag(d, (int)sizeof d);
    NSString *diag = [NSString stringWithUTF8String:d];
    if (!diag) diag = @"(诊断行组装失败)";

    NSString *txt = [NSString stringWithFormat:@"%@\n%@\n—— 上次运行的最后几行 ——\n%@",
                     g_status_head ?: @"", diag, g_prev_tail ?: @""];

    /* 必须回主线程改 UI */
    dispatch_async(dispatch_get_main_queue(), ^{
        if (g_status) { g_status.text = txt; }
        NSLog(@"[dm] %@", txt);
    });
}

static void status_set(NSString *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
    va_end(ap);
    refresh_status(s);
}

/* ------------------------------------------------------------------ */
/* 崩溃捕获（让「闪退」也留下线索）                                    */
/* ------------------------------------------------------------------ */
/* 为什么需要（第 46 轮加）：
 *   客体跑在**主线程**上。它一旦踩到坏指针，App 直接消失 —— 屏幕上的字跟着没了，
 *   而那正是我们最需要的信息（"崩在哪条指令"）。
 *   这里装一组信号处理：崩的时候只做一件事 —— 把故障地址 / PC / LR / SP / 参数
 *   **写进日志文件**（走 dm_diag_crash_line，内部只用 write(2)）。
 *   下次启动时屏幕会把日志尾部显示出来（见 log_tail），于是不用连电脑就能看到。
 *   再配合 dm_diag_set_module 登记的各模块基址，PC 会被翻成「模块 + 偏移」，
 *   拿这个偏移去反汇编就知道是哪一条指令炸的。
 *
 * 信号处理里**只能用 write 这一类**：snprintf/malloc 都可能踩到锁或分配器而二次崩溃
 * （那就一点痕迹都留不下了）。所以格式化在 hle_diag.c 里手写、只 write 一次。 */
static void dm_crash_handler(int sig, siginfo_t *info, void *uctx) {
    unsigned long long pc = 0, lr = 0, sp = 0, xs[4] = { 0, 0, 0, 0 };
    unsigned long long fault = info ? (unsigned long long)(uintptr_t)info->si_addr : 0;
#if defined(__aarch64__)
    ucontext_t *uc = (ucontext_t *)uctx;
    if (uc) {
        pc = (unsigned long long)uc->uc_mcontext->__ss.__pc;
        lr = (unsigned long long)uc->uc_mcontext->__ss.__lr;
        sp = (unsigned long long)uc->uc_mcontext->__ss.__sp;
        for (int i = 0; i < 4; i++)
            xs[i] = (unsigned long long)uc->uc_mcontext->__ss.__x[i];
    }
#else
    (void)uctx;
#endif
    dm_diag_crash_line(sig, fault, pc, lr, sp, xs, 4);

    /* 恢复默认处理再重发 —— 让系统照常生成崩溃报告，别把进程留在半死状态 */
    signal(sig, SIG_DFL);
    raise(sig);
}

static void dm_install_crash_handlers(void) {
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGSYS,
                                SIGFPE, SIGABRT, SIGTRAP };
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = dm_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for (unsigned i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
    dm_diag_mark("崩溃捕获已装好（%u 个信号）", (unsigned)(sizeof sigs / sizeof sigs[0]));
}

@interface DmVC : UIViewController
@end
@implementation DmVC

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view = [[DmTouchView alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.view.multipleTouchEnabled = YES;

    g_device = MTLCreateSystemDefaultDevice();
    g_layer = (CAMetalLayer *)self.view.layer;
    g_layer.device = g_device;
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = NO;      /* 允许从 drawable 读回（真机侧可能用于截图） */

    /* 状态文字叠在 Metal 层上（左上角），黑屏时也能看到进度。
     * 行数给足 8 行：本次状态 + 一行总览 + 「上次运行的最后几行」+ 日志尾部。
     * 字体用等宽 —— 数字对齐，帧计数与 NVN 计数好读。 */
    g_status = [[UILabel alloc] initWithFrame:CGRectMake(16, 30, self.view.bounds.size.width - 32, 200)];
    g_status.numberOfLines = 12;   /* 状态 + 总览 + 「上次运行」+ 最多 8 行日志尾部 */
    g_status.textColor = [UIColor whiteColor];
    g_status.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
    g_status.text = @"正在启动…";
    g_status.autoresizingMask = UIViewAutoresizingFlexibleWidth;
    [self.view addSubview:g_status];

    /* 读**上次运行**日志的尾部（必须在本次开始写日志之前读，否则读到的是本次的）。
     * 这是崩溃后的主要线索来源：上次跑到哪一步、崩在哪 —— 开机就能看到。 */
    g_prev_tail = log_tail(8);   /* 崩溃那块一次写 4~5 行，给足 8 行才不会截掉 */
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (g_link) return;
    g_running = 1;
    if (!g_tick_target) g_tick_target = [DmFrameTick new];
    /* 60 Hz 起步；本游戏引擎按横屏 60 fps 设计，高刷先不申请 */
    g_link = [CADisplayLink displayLinkWithTarget:g_tick_target
                                         selector:@selector(tick:)];
    [g_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    /* 本游戏按横屏设计（引擎的 ViewProjectionMtx 与 drawWH 都是横向） */
    return UIInterfaceOrientationMaskLandscape;
}

@end

/* ------------------------------------------------------------------ */
/* 供 main.m 调：建窗口 + 起 VC，并把装载放到后台线程                  */
/* ------------------------------------------------------------------ */

/* 在哪里装载、以及要不要起帧循环 —— 都由这里决定，main.m 不掺和 UI 细节。
 *
 * !! 第 43 轮修正 !!
 *   原来 main.m 只是声明了 @property UIWindow *window 而**从来没建过窗口**，
 *   于是 rootViewController 为 nil → DmVC 永不加载 → 黑屏。
 *   同时 dm_ios_start() 是**同步**跑在 didFinishLaunching 里的 ——
 *   装载 5 个模块（36 MB）+ 24,660 条重定位会拖住主线程，
 *   iOS 的启动看门狗（约 20 秒）会杀进程。
 *   现在：先让窗口可见（用户立刻看到状态文字），装载放后台线程。 */
UIWindow *dm_ios_make_window(void) {
    UIWindow *w = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    w.rootViewController = [[DmVC alloc] init];
    w.backgroundColor = [UIColor blackColor];
    [w makeKeyAndVisible];
    /* ↑ 这一句会触发 viewDidLoad，viewDidLoad 里读「上次运行日志的尾部」。
       所以**日志要在这之后才打开**（先读旧的，再开始写新的）。 */
    dm_diag_set_log(dm_ios_log_path());
    dm_diag_mark("=== 本次启动（窗口已建）—— 日志 %s ===", dm_ios_log_path());
    /* 崩溃捕获要在装载之前装好 —— 装载与进客体都可能崩 */
    dm_install_crash_handlers();
    return w;
}

void dm_ios_boot_async(void) {
    status_set(@"正在装载游戏模块…");
    dm_diag_mark("开始后台装载（dm_host_boot）");

    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        /* 主线程已经起了，装载放这里跑 */
        int rc = dm_ios_start();
        if (rc != 0) {
            dm_diag_mark("装载**失败** rc=%d（-10-i = 第 i 个模块）", rc);
            status_set(@"装载失败 rc=%d\n（-10-i = 第 i 个模块）\n请看控制台或崩溃报告", rc);
        } else {
            dm_diag_mark("装载成功：%s", dm_diag_boot_text());
            status_set(@"已装载。等待启动序列…");
        }
    });
}

/* ------------------------------------------------------------------ */
/* 生命周期与内存警告                                                  */
/* ------------------------------------------------------------------ */

/* 进入后台：暂停帧循环（iOS 不允许后台持续渲染）。
   注意：不做「释放资源」—— 游戏状态在客体内存里，重建代价高。 */
void dm_ios_enter_background(void) {
    g_running = 0;
    if (g_link) { [g_link invalidate]; g_link = nil; }
}

/* 回到前台：重建帧循环。
 * !! 第 43 轮修正 !! 原版这里也用了 [DmRateHint new]（空回调）——
 *   而且 g_vc 从来没被赋值过，所以 if (!g_vc) return; 会让这个函数**永远直接返回**，
 *   于是从后台回来后再也不会渲染。现在用真正的帧驱动目标。 */
void dm_ios_enter_foreground(void) {
    if (g_running) return;
    if (g_link) return;
    g_running = 1;
    if (!g_tick_target) g_tick_target = [DmFrameTick new];
    g_link = [CADisplayLink displayLinkWithTarget:g_tick_target
                                         selector:@selector(tick:)];
    [g_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

/* 内存警告：宿主侧最该丢的是「已翻译但还没用的着色器源码缓存」与纹理暂存。
   这里只做记录 —— 真正的清理策略要等真机跑起来看内存曲线再定。 */
void dm_ios_memory_warning(void) { }

/* ------------------------------------------------------------------ */
/* RomFS / 资源目录                                                    */
/* ------------------------------------------------------------------ */

/* Documents 目录的绝对路径。
 * App 的「文稿」目录 —— 用户用 iTunes「文件共享」或「文件」App 往里拷东西。
 * （Info.plist 里开了 UIFileSharingEnabled，所以它可见。） */
const char *dm_ios_docs_dir(void) {
    static char buf[1024];
    if (buf[0]) return buf;                            /* 缓存一次 */
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                         NSUserDomainMask, YES);
    NSString *docs = paths.count ? paths[0] : NSTemporaryDirectory();
    const char *c = docs.UTF8String;
    snprintf(buf, sizeof buf, "%s", (c && c[0]) ? c : "/tmp");
    return buf;
}

/* 模块与着色器所在的目录：<bundle>/dm
 *
 * 为什么需要它：iOS 上 App 的**当前工作目录不是 bundle 目录**，
 * 所以宿主里 `fopen("dm/main.patched.bin")` 这种相对路径会失败 ——
 * 表现为「装上了但一起就报找不到游戏程序」。
 * 这里用 NSBundle 给出**绝对路径**，与 dmpack.py 的包内布局对应。 */
const char *dm_ios_res_dir(void) {
    static char buf[1024];
    if (buf[0]) return buf;                            /* 缓存一次 */
    NSString *p = [NSBundle mainBundle].bundlePath;
    const char *c = p.UTF8String;
    if (c && c[0]) snprintf(buf, sizeof buf, "%s/dm", c);   /* 内容在 bundle 下的 dm/ */
    else           snprintf(buf, sizeof buf, "dm");         /* 兜底：退回相对路径 */
    return buf;
}

/* ------------------------------------------------------------------ */
/* 启动                                                                */
/* ------------------------------------------------------------------ */

int dm_ios_start(void) {
    /* 游戏数据目录由宿主自己决定（dm_host_data_dir）：
     *   包里内置的 data/ 优先 → 没有再退回 Documents。
     * 这样「打包时放了启动集」的情况下，用户装上就能跑，不用手工拷文件。 */
    NSString *docs = [NSString stringWithUTF8String:dm_ios_docs_dir()];
    char save[1200];
    snprintf(save, sizeof save, "%s/../dm-save", docs.UTF8String ? docs.UTF8String : "/tmp");

    /* romfs 参数传 NULL 让宿主用 dm_host_data_dir() 的判定；
       但为兼容旧签名，这里直接传我们算好的路径 —— 与宿主同一套逻辑。 */
    extern const char *dm_host_data_dir(void);
    const char *romfs = dm_host_data_dir();
    return dm_host_boot(romfs, save);   /* 0 = 成功 */
}
