/* 宿主 —— 资源目录接口（平台无关的调用方用这个）。
 *
 * 为什么单独一个头：原来 hle_nvn_boot.c 直接 getenv("DM_MODULE_DIR")，
 * 而 iOS 上那个变量没人设，默认的 "dm" 相对路径在 iOS 上解析不到 bundle 里的文件。
 */
#ifndef DM_HLE_HOST_DIR_H
#define DM_HLE_HOST_DIR_H

/* 返回「模块与着色器数据所在的目录」。
 *   Windows/Linux: 环境变量 DM_MODULE_DIR，否则 "dm"（相对当前目录）
 *   Apple:         环境变量优先，否则 App bundle 内的 dm/（由 iOS 外壳提供）
 * 返回的是静态存储，调用方不必释放。 */
const char *dm_host_res_dir(void);

/* 返回「客体的 RomFS 根目录」（客体 /data/xxx 会映射到 <这里>/data/xxx）。
 *
 * 查找顺序：
 *   Windows/Linux: 环境变量 DM_ROMFS_DIR，否则 "."（相对当前目录）
 *   Apple:         DM_ROMFS_DIR 优先
 *                  → App 包里内置的 data/（若打包时放了，用户就不用手工拷）
 *                  → App 的 Documents 目录（用户用 iTunes「文件共享」拷进去的）
 *
 * 为什么要有两级：模块（36 MB）我们本来就打进包；游戏数据 4 GB **不该进包**，
 * 但为了「第一次试跑不用手工拷文件」，可以只把 40 MB 的启动集打进包。 */
const char *dm_host_data_dir(void);

#endif /* DM_HLE_HOST_DIR_H */
