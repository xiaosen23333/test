/* 找客体入口点 —— 正确的方法：按名字在 dynsym 里找（而不是在 MOD0 里猜）。
 *
 * NintendoSDK 的约定：游戏模块导出 `nnMain`，由 SDK 的 nn::init::Start 调用。
 * D2R 的 main.c 也是这么写的（「…初始化 SDK 运行时，再进 nnMain」）。
 *
 * 本自测：
 *   1. 装载 sdk 与 main
 *   2. 用 dm_loader_sym 找 nnMain，打印它的地址
 *   3. 核对 `nnMain` 确实来自 main 模块（而不是 sdk）
 *   4. 再找几个 SDK 运行时符号（nninitStartup / nnosInitialize / __nnmusl_init_locks）
 *      —— 这些是 dm_host_boot 之后要依次调的
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    static const char *names[] = { "rtld", "sdk", "main" };
    const unsigned NK = sizeof names / sizeof names[0];

    dm_load_ctx ctx;
    dm_loader_reset(&ctx);
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    char bin[512], fix[512];
    for (unsigned i = 0; i < NK; i++) {
        snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, names[i]);
        snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, names[i]);
        dm_module *m = &ctx.mods[ctx.nmods];
        int rc = dm_loader_map(m, bin, fix, ctx.key_values);
        if (rc) { printf("map %s FAIL rc=%d\n", names[i], rc); return 1; }
        ctx.nmods++;
    }
    /* 必须先重定位，符号值才是最终地址 */
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_load_stats st; memset(&st, 0, sizeof st);
        dm_loader_relocate(&ctx.mods[i], ctx.mods, ctx.nmods, &st);
    }

    printf("=== 按名字找入口与 SDK 运行时符号 ===\n");
    static const char *probe[] = {
        "nnMain",                  /* 游戏主函数（客体入口） */
        "nninitStartup",           /* SDK 启动 */
        "nnosInitialize",          /* nn::os 初始化 */
        "__nnmusl_init_locks",     /* libc 锁初始化（D2R 特别提到：略过会自锁死） */
        "_start",                  /* 标准入口 */
    };
    int found = 0, gg = 0;
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        const void *p = dm_loader_sym(ctx.mods, ctx.nmods, probe[i]);
        printf("  %-24s -> %s", probe[i], p ? "" : "(未找到)\n");
        if (!p) continue;
        found++;
        /* 找出它属于哪个模块 */
        uintptr_t a = (uintptr_t)p;
        for (unsigned k = 0; k < ctx.nmods; k++) {
            uintptr_t b = (uintptr_t)ctx.mods[k].base;
            if (a >= b && a < b + ctx.mods[k].size) {
                printf("@%p  （模块 %s + %#llx）%s\n", p, ctx.mods[k].name,
                       (unsigned long long)(a - b),
                       !strcmp(probe[i], "nnMain") ? "  <== 客体入口" : "");
                if (!strcmp(probe[i], "nnMain")) gg = 1;
                break;
            }
        }
    }
    printf("\n  找到 %d / %u 个\n", found, (unsigned)(sizeof probe / sizeof probe[0]));
    printf("  nnMain 定位成功: %s\n", gg ? "是（这就是 dm_host_set_guest_entry 要的东西）" : "否");

    printf("\n=== 顺带核对：nnMain 所在的模块 ===\n");
    printf("  main 模块 base=%p size=%zu\n", (void *)ctx.mods[2].base, ctx.mods[2].size);
    printf("  sdk  模块 base=%p size=%zu\n", (void *)ctx.mods[1].base, ctx.mods[1].size);

    printf("\n=== result: %s ===\n", gg ? "ALL PASS" : "CHECK FAILED");
    return gg ? 0 : 1;
}
