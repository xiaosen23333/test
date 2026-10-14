/* NVN 接口层自测 —— 验证「按名字找实现 → 调用 → 计数 → 未实现报名字」这条链。
 *
 * 这是图形层能被接管的最小验证：只要这条链通了，后面把
 * nvn_mtl_*（Metal 后端）逐个注册进来就等于「实现了图形层」。
 */
#include "../hle_nvn.h"

#include <stdio.h>
#include <string.h>

/* 假实现：让 nvnDeviceInitialize / nvnDeviceFinalize 有实现，其余走未实现入口 */
static uint64_t fake_device_init(uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    *(int *)a0 = 0x5EED;         /* 往「设备对象」写个标记，证明真的被调到了 */
    return 1;                    /* NVN 的 NVNboolean = true */
}
static uint64_t fake_device_fini(uint64_t a0, uint64_t a1, uint64_t a2,
                                 uint64_t a3, uint64_t a4) {
    (void)a0; (void)a1; (void)a2; (void)a3; (void)a4;
    return 0;
}

static int g_unimpl_count = 0;
static void on_unimpl(const char *name, void *ud) {
    (void)ud;
    if (g_unimpl_count < 8) printf("    未实现: %s\n", name);
    g_unimpl_count++;
}

int main(void) {
    extern const void *dm_nvn_stats_ptr(void);
    struct { uint64_t n_calls, n_unimpl_calls, n_distinct_called, n_distinct_unimpl; } *st;

    printf("=== NVN 名字表 ===\n");
    printf("  表长: %d\n", NVN_NFUNC);
    printf("  前 5 个: ");
    for (int i = 0; i < 5; i++) printf("%s ", nvn_names[i]);
    printf("\n");

    /* 1. 名字解析 */
    printf("\n=== 1) 名字 → 下标 ===\n");
    const char *probe[] = { "nvnDeviceInitialize", "nvnCommandBufferDrawElements",
                            "nvnTextureBuilderSetDefaults", "nvnBogusName" };
    int idx_ok = 0;
    for (int i = 0; i < 4; i++) {
        int k = dm_nvn_index_of(probe[i]);
        printf("  %-34s -> %d\n", probe[i], k);
        if (i < 3 && k >= 0) idx_ok++;
        if (i == 3 && k < 0) idx_ok++;
    }
    printf("  解析正确: %d / 4 %s\n", idx_ok, idx_ok == 4 ? "OK" : "<-- 不符");

    /* 2. 注册实现 */
    printf("\n=== 2) 注册实现 ===\n");
    printf("  注册前已注册数: %u\n", dm_nvn_registered());
    dm_nvn_register("nvnDeviceInitialize", (void *)fake_device_init);
    dm_nvn_register("nvnDeviceFinalize",   (void *)fake_device_fini);
    printf("  注册后已注册数: %u (expect 2)\n", dm_nvn_registered());

    /* 3. 分发：有实现的走实现 */
    printf("\n=== 3) 分发到实现 ===\n");
    int dev = 0;
    uint64_t r = dm_nvn_dispatch("nvnDeviceInitialize", (uint64_t)(uintptr_t)&dev, 0, 0, 0, 0);
    printf("  nvnDeviceInitialize -> 返回 %llu, 设备对象被写为 %#x  %s\n",
           (unsigned long long)r, dev, dev == 0x5EED ? "OK" : "<-- 没被调到");
    printf("  该槽计数: %llu (expect 1)\n", (unsigned long long)nvn_slot[dm_nvn_index_of("nvnDeviceInitialize")].cnt);

    /* 4. 分发：没实现的走未实现入口并计数 */
    printf("\n=== 4) 分发到未实现 ===\n");
    int before = g_unimpl_count;
    for (int i = 0; i < 3; i++)
        dm_nvn_dispatch("nvnCommandBufferDrawElements", 0, 0, 0, 0, 0);
    int after = g_unimpl_count;
    printf("  连调 3 次 nvnCommandBufferDrawElements -> 未实现种类增加 %d (expect 1，去重)\n",
           after - before);

    /* 5. bootstrap：客体拿到的入口 */
    printf("\n=== 5) nvnBootstrapLoader ===\n");
    void *p_lib  = dm_nvn_bootstrap("nvn");
    void *p_fn   = dm_nvn_bootstrap("nvnDeviceInitialize");
    void *p_none = dm_nvn_bootstrap("nvnNotExist");
    printf("  bootstrap(\"nvn\")                 -> %s\n", p_lib ? "非空 OK" : "NULL <-- 不符");
    printf("  bootstrap(\"nvnDeviceInitialize\")  -> %s\n",
           p_fn == (void *)fake_device_init ? "等于已注册实现 OK" : "不符");
    printf("  bootstrap(\"nvnNotExist\")          -> %s\n", p_none ? "非空" : "NULL OK");

    /* 6. 统计与未实现清单 */
    printf("\n=== 6) 统计 ===\n");
    dm_nvn_dump_unimpl(on_unimpl, NULL);
    printf("  未实现种类合计: %d\n", g_unimpl_count);
    printf("  至少被调到一次的 NVN 种类: %u\n", 2 + (unsigned)g_unimpl_count);

    printf("\n=== result: %s ===\n",
           (idx_ok == 4 && dev == 0x5EED && g_unimpl_count >= 1) ? "ALL PASS" : "CHECK FAILED");
    return 0;
}
