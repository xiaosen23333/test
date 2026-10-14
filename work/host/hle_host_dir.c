/* 宿主入口 —— 平台无关的「资源目录」接口（宿主侧）。
 *
 * 需要两类目录，它们**可以不同**：
 *   · 模块目录  dm_host_res_dir()   —— 5 个改写后的模块 + 元数据 + 着色器表 + metal/
 *                                      打包时放在 App 包内的 dm/
 *   · 游戏数据目录 dm_host_data_dir() —— 客体的 RomFS（/data/... 那些）
 *                                      可能在包里（内置），也可能在 Documents（用户拷的）
 *
 * 为什么分开：
 *   模块（36 MB）我们本来就打进包；而游戏数据是 4 GB，**不该进包**，
 *   只能靠用户拷进 Documents。但为了「第一次试跑不用手工拷文件」，
 *   我们支持把一小部分数据（40 MB 的启动集）也打进包。
 *   所以数据目录**要在包里和 Documents 之间二选一**，而模块目录固定是包里。
 *
 * !! 踩过的坑 !!（第 41~42 轮）
 *   · 最初宿主用相对路径 "dm" 找模块 —— Windows 上能过（当前目录就是工作目录），
 *     但 **iOS 上 App 的当前工作目录不是 bundle 目录**，`fopen("dm/...")` 必失败。
 *   · 所以 Apple 上必须用 NSBundle 给出的**绝对路径**（由 iOS 外壳提供）。
 */
#include "hle_host_dir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__APPLE__)
/* iOS 外壳（front_ios.m）提供：
 *   dm_ios_res_dir()  -> <bundle>/dm          （模块与着色器）
 *   dm_ios_docs_dir() -> <Documents>          （游戏数据；用户拷进这里） */
extern const char *dm_ios_res_dir(void);
extern const char *dm_ios_docs_dir(void);
#endif

/* 判断路径是否是一个**存在**的目录。用于「包里有没有内置 data/」这个判断。 */
static int dir_exists(const char *p) {
    if (!p || !p[0]) return 0;
    struct stat st;
#if defined(_WIN32)
    if (stat(p, &st) != 0) return 0;
    return (st.st_mode & _S_IFDIR) != 0;
#else
    if (stat(p, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
#endif
}

const char *dm_host_res_dir(void) {
    /* 环境变量优先（开发期在 Mac 上用 DM_MODULE_DIR 指向别处调试时方便） */
    const char *env = getenv("DM_MODULE_DIR");
    if (env && env[0]) return env;

#if defined(__APPLE__)
    {
        const char *p = dm_ios_res_dir();
        if (p && p[0]) return p;      /* 形如 /var/containers/.../DmGame.app/dm */
    }
#endif

    /* 非 Apple：保持原来的相对路径行为（Windows 自测与命令行都靠它） */
    return "dm";
}

const char *dm_host_data_dir(void) {
    /* 环境变量优先（开发期用 DM_ROMFS_DIR 指向 work/extract/game 这类目录） */
    const char *env = getenv("DM_ROMFS_DIR");
    if (env && env[0]) return env;

#if defined(__APPLE__)
    {
        /* ① 先在**包里**找内置的 data/ —— 打包时若把启动集放进去，就走这条。
         *    好处：用户装上就能跑，不用手工拷文件（少一个易错的环节）。 */
        static char bundled[1200];
        if (!bundled[0]) {
            const char *res = dm_ios_res_dir();          /* <bundle>/dm */
            if (res && res[0]) {
                /* 内置数据放在包根的 data/（不是 <bundle>/dm/data）——
                   与 dmpack.py 的布局一致 */
                char root[1100];
                snprintf(root, sizeof root, "%s", res);
                char *slash = strrchr(root, '/');
                if (slash) *slash = 0;                    /* 去掉结尾的 /dm */
                snprintf(bundled, sizeof bundled, "%s/data", root);
            }
        }
        if (dir_exists(bundled)) return bundled;

        /* ② 包里没有 → 用 Documents（用户拷进去的） */
        const char *docs = dm_ios_docs_dir();
        if (docs && docs[0]) return docs;
    }
#endif

    /* 非 Apple：开发期用 work/extract/game 那一层；自测里靠 DM_ROMFS_DIR 指定 */
    return ".";
}
