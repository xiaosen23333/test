/* 本游戏 iOS 外壳 —— 应用入口与生命周期。
 *
 * !! 只能在 macOS + Xcode 上编译 !!  Windows 上无法验证，故逻辑刻意保持最简。
 *
 * 与宿主的分工：宿主（work/host/）负责客体的一切；本文件只做「iOS 才有的事」。
 *
 * ============================================================================
 * !! 第 43 轮修正 —— 黑屏的两个原因（都在这个文件/配对文件里）!!
 *
 * 【原因 1】窗口从来没建过
 *   原来这里只写了 @property UIWindow *window; 而**没有任何代码创建它**。
 *   于是 rootViewController 为 nil → front_ios.m 里的 DmVC 永不加载 → 黑屏。
 *
 * 【原因 2】装载同步跑在 didFinishLaunching 里
 *   dm_ios_start() 要装载 5 个模块（36 MB）+ 24,660 条重定位。
 *   这些在主线程上做会拖住启动，iOS 的看门狗（约 20 秒）会杀进程。
 *
 * 现在：先建窗口让界面可见（用户立刻看到状态文字），装载放后台线程。
 * 具体做法都在 front_ios.m 的 dm_ios_make_window() / dm_ios_boot_async() 里 ——
 * 本文件不掺和 UI 细节，只按顺序调它们。
 * ============================================================================
 */
#import <UIKit/UIKit.h>

/* front_ios.m 提供 */
extern int  dm_ios_start(void);
extern void dm_ios_enter_background(void);
extern void dm_ios_enter_foreground(void);
extern void dm_ios_memory_warning(void);
extern void dm_host_shutdown(void);
extern UIWindow *dm_ios_make_window(void);      /* 建窗口 + 起 DmVC */
extern void      dm_ios_boot_async(void);       /* 后台装载 */

@interface DmAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation DmAppDelegate

- (BOOL)application:(UIApplication *)app
        didFinishLaunchingWithOptions:(NSDictionary *)opts {
    (void)app; (void)opts;

    /* ① 先把窗口立起来 —— 用户立刻能看到界面（含状态文字），而不是黑屏等着。
     *    注意顺序：窗口先于装载。原版反过来了（先同步装载再指望 UI），
     *    结果既黑屏又可能被看门狗杀掉。 */
    self.window = dm_ios_make_window();

    /* ② 装载放后台线程（不阻塞主线程与看门狗） */
    dm_ios_boot_async();

    return YES;
}

- (void)applicationDidBecomeActive:(UIApplication *)app {
    (void)app;
    dm_ios_enter_foreground();
}

- (void)applicationWillResignActive:(UIApplication *)app {
    (void)app;
    dm_ios_enter_background();
}

- (void)applicationDidEnterBackground:(UIApplication *)app {
    (void)app;
    dm_ios_enter_background();
}

- (void)applicationWillTerminate:(UIApplication *)app {
    (void)app;
    dm_host_shutdown();
}

- (void)applicationDidReceiveMemoryWarning:(UIApplication *)app {
    (void)app;
    dm_ios_memory_warning();
}

@end

int main(int argc, char *argv[]) {
    @autoreleasepool {
        /* 宿主装载**不在这里**做 —— 放在 application:didFinishLaunching 之后
           的后台线程里（见上）。原因：UIApplicationMain 之前 UIKit 还没起好，
           而宿主会建线程、用 pthread 键、跑 TSD 校验。 */
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(DmAppDelegate.class));
    }
}
