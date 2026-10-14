/* 宿主 —— 资源目录接口（平台无关的调用方用这个）。
 *
 * 为什么单独一个头：原来 hle_nvn_boot.c 直接 getenv("DM_MODULE_DIR")，
 * 而 iOS 上那个变量没人设，默认的 "dm" 相对路径在 iOS 上解析不到 bundle 里的文件。
 */
#ifndef DM_HLE_HOST_DIR_H
#define DM_HLE_HOST_DIR_H

/* 返回「模块与着色器数据所在的目录」。
 *   Windows/Linux: 环境变量 DM_MODULE_DIR，否则 "dm"（相对当前目录）
 *   Apple:         环境变量优先，否则 App bundle 路径（由 iOS 外壳提供）
 * 返回的是静态存储，调用方不必释放。 */
const char *dm_host_res_dir(void);

#endif /* DM_HLE_HOST_DIR_H */
