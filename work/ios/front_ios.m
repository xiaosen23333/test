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

#include "hle.h"
#include "hle_nn.h"
#include "hle_nn_more.h"
#include "hle_nvn.h"
#include "hle_nvn_obj.h"
#include "hle_nvn_cmd.h"
#include "hle_nvn_metal.h"

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
static UIViewController *g_vc = nil;
static int              g_running = 0;

/* ProMotion：iPhone 上 App 自己调 nextDrawable 时系统默认按 60 Hz 给画布。
   挂一个空回调的 CADisplayLink 来「申请」高刷（D2R 实测：还要在 Info.plist 里
   加 CADisableMinimumFrameDurationOnPhone）。iPad 上同样适用。 */
@interface DmRateHint : NSObject
- (void)tick:(CADisplayLink *)l;
@end
@implementation DmRateHint
- (void)tick:(CADisplayLink *)l { (void)l; }   /* 只表态，不做事 */
@end
static CADisplayLink *g_rate_link = nil;

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

/* 一帧：先让宿主跑（它会调用 NVN，命令录进录音机），再提交。
   真机上 dm_host_frame 内部应：
     · 经 gate 分发跑客体的一帧
     · 在 nvnQueueSubmitCommands 时把录音回放成 MTLCommandBuffer
     · 在 nvnQueuePresentTexture 时 presentDrawable */
static void frame_tick(void) {
    if (!g_running) return;
    @autoreleasepool {
        dm_host_frame();
    }
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
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    if (g_link) return;
    g_running = 1;
    /* 60 Hz 起步；真机侧若要 ProMotion，把 preferredFrameRateRange 调到 120 */
    g_link = [CADisplayLink displayLinkWithTarget:[DmRateHint new]
                                         selector:@selector(tick:)];
    if (@available(iOS 15.0, *)) {
        g_link.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 60);
    }
    [g_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
    /* 另起一个稳定的 tick 驱动宿主帧（与刷新率解耦，便于限帧） */
    g_rate_link = [CADisplayLink displayLinkWithTarget:[DmRateHint new]
                                              selector:@selector(tick:)];
    [g_rate_link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (BOOL)prefersStatusBarHidden { return YES; }
- (BOOL)prefersHomeIndicatorAutoHidden { return YES; }
- (UIInterfaceOrientationMask)supportedInterfaceOrientations {
    /* 本游戏按横屏设计（引擎的 ViewProjectionMtx 与 drawWH 都是横向） */
    return UIInterfaceOrientationMaskLandscape;
}

@end

/* ------------------------------------------------------------------ */
/* 生命周期与内存警告                                                  */
/* ------------------------------------------------------------------ */

/* 进入后台：暂停帧循环（iOS 不允许后台持续渲染）。
   注意：不做「释放资源」—— 游戏状态在客体内存里，重建代价高。 */
void dm_ios_enter_background(void) {
    g_running = 0;
    if (g_link) { [g_link invalidate]; g_link = nil; }
    if (g_rate_link) { [g_rate_link invalidate]; g_rate_link = nil; }
}

void dm_ios_enter_foreground(void) {
    if (g_running) return;
    if (!g_vc) return;
    g_running = 1;
    g_link = [CADisplayLink displayLinkWithTarget:[DmRateHint new]
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
