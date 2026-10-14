/* 宿主入口 —— 平台无关的「资源目录」接口（宿主侧）。
 *
 * 为什么需要这个文件：
 *   宿主原来用**相对路径** "dm" 找模块（`DM_MODULE_DIR` 为空时）。
 *   Windows 上开发时当前目录就是工作目录，所以自测能过；
 *   但 **iOS 上 App 的当前工作目录不是 bundle 目录** ——
 *   `fopen("dm/main.patched.bin")` 会直接失败，
 *   表现为「装上了但一起就报找不到游戏程序」。
 *
 * 做法：宿主在需要目录时调 dm_host_res_dir()：
 *   · Windows / Linux：环境变量 DM_MODULE_DIR，否则 "dm"（保持原行为，自测不受影响）
 *   · Apple 平台：由 iOS 外壳提供 dm_ios_res_dir()（它用 NSBundle 取 bundle 路径）
 *
 * 与 entry.S / hle_plat_aarch64.c 同样的分层思路：
 *   平台无关的调用方在宿主里，平台相关的实现在各自那边。
 */
#include "hle_host_dir.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
/* iOS 外壳（front_ios.m）提供：返回 App bundle 内的资源目录（绝对路径）。
   它内部用 [NSBundle mainBundle].bundlePath，所以不依赖当前工作目录。 */
extern const char *dm_ios_res_dir(void);
#endif

const char *dm_host_res_dir(void) {
    /* 环境变量优先（开发期在 Mac 上用 DM_MODULE_DIR 指向别处调试时方便） */
    const char *env = getenv("DM_MODULE_DIR");
    if (env && env[0]) return env;

#if defined(__APPLE__)
    {
        const char *p = dm_ios_res_dir();
        if (p && p[0]) return p;      /* 形如 /var/containers/.../DmGame.app */
    }
#endif

    /* 非 Apple：保持原来的相对路径行为（Windows 自测与命令行都靠它） */
    return "dm";
}
