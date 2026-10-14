/* 本游戏 iOS 外壳 —— 应用入口与生命周期。
 *
 * !! 只能在 macOS + Xcode 上编译 !!  Windows 上无法验证，故逻辑刻意保持最简。
 *
 * 与宿主的分工：宿主（work/host/）负责客体的一切；本文件只做「iOS 才有的事」。
 * 启动顺序（顺序有依赖，不要调换）：
 *   1. 建 Documents 目录（RomFS 与存档都放这里）
 *   2. dm_ios_start() -> 宿主装载 5 个模块、填键页、登记门
 *   3. UIApplicationMain -> 起界面与 Metal 层
 *   4. viewDidAppear 起帧循环
 */
#import <UIKit/UIKit.h>

/* front_ios.m 提供 */
extern int  dm_ios_start(void);
extern void dm_ios_enter_background(void);
extern void dm_ios_enter_foreground(void);
extern void dm_ios_memory_warning(void);
extern void dm_host_shutdown(void);

@interface DmAppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation DmAppDelegate

- (BOOL)application:(UIApplication *)app
        didFinishLaunchingWithOptions:(NSDictionary *)opts {
    (void)app; (void)opts;
    /* 宿主装载放在这里，而不是 main() 里 UIApplicationMain 之前 ——
       UIKit 起好之后线程与 runloop 才是「正常运行状态」，
       而宿主会建线程、用 pthread 键、跑 TSD 校验，在 UIApplicationMain 之前做
       容易撞上 UIKit 自己的初始化。（D2R 把一部分前置放在 main 开头、一部分放在这里，
       我们简化为全放这里。） */
    int rc = dm_ios_start();
    if (rc != 0) {
        /* 不要让界面起一个「什么都没有」的窗口 —— 直接给出可读的原因。
           最常见：游戏文件的 18 个 .data 档案没拷进本 App 的文件夹。 */
        UIAlertController *a =
            [UIAlertController alertControllerWithTitle:@"还没准备好"
                                                message:@"请把游戏 RomFS/data 里的 .data 档案拷进本 App 的文件夹，然后重新打开。"
                                         preferredStyle:UIAlertControllerStyleAlert];
        /* 此时窗口还没建，延到主队列里弹 */
        dispatch_async(dispatch_get_main_queue(), ^{
            UIWindow *w = self.window ?: UIApplication.sharedApplication.windows.firstObject;
            [w.rootViewController presentViewController:a animated:YES completion:nil];
        });
    }
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
        /* 注意：宿主装载**不在这里**做 —— 放在 application:didFinishLaunching 里。
           原因：UIApplicationMain 之前 UIKit 还没起好，而宿主会建线程、用 pthread 键、
           跑 TSD 校验，放在之前容易撞上 UIKit 自己的初始化。 */
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(DmAppDelegate.class));
    }
}
