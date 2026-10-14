/* 找 D2R 特别警告过的 `__nnmusl_init_locks` —— 略过会在 putc 嵌套加锁时自锁死。
 *
 * D2R 的 main.c 第 60 行原文：
 *   逐条照抄 Start：_init_libc1 = __nnmusl_init_locks（把 stdio 等锁初始化成递归锁，
 *   略过会在 putc 嵌套加锁时自锁死）
 *
 * 本自测：
 *   1. 在 5 个模块的 dynsym 里按名字搜（含多种可能的名字形态）
 *   2. 顺带列出 rtld 的**全部导出符号** —— rtld 很小（10 KB），符号表可以整张看
 *   3. 再列 main 的导出符号里含 musl / libc / init / lock / stdio 的
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    static const char *names[] = { "rtld", "sdk", "main", "NmplDLL.nro", "lz4.nro" };
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
        if (rc) { printf("map %s FAIL rc=%d (%s)\n", names[i], rc, dm_loader_err(rc)); return 1; }
        ctx.nmods++;
    }

    printf("=== 1) 按名字搜 __nnmusl_init_locks 的多种形态 ===\n");
    static const char *probe[] = {
        "__nnmusl_init_locks", "_init_libc1", "nnmusl_init_locks",
        "init_locks", "__libc_init_locks", "__init_libc",
    };
    int found = 0;
    for (unsigned i = 0; i < sizeof probe / sizeof probe[0]; i++) {
        const void *p = dm_loader_sym(ctx.mods, ctx.nmods, probe[i]);
        printf("  %-24s -> %s\n", probe[i], p ? "找到" : "(没有)");
        if (p) found++;
    }

    printf("\n=== 2) rtld 的全部导出符号（它只有 10 KB，整张表可以看）===\n");
    {
        dm_module *m = &ctx.mods[0];   /* rtld */
        unsigned nd = 0;
        for (unsigned k = 0; k < m->nsym && nd < 60; k++) {
            const char *nm = m->strtab + m->symtab[k].st_name;
            if (!nm[0] || !m->symtab[k].st_shndx) continue;
            printf("    %-46s value=%#llx\n", nm, (unsigned long long)m->symtab[k].st_value);
            nd++;
        }
        printf("    （rtld 导出 %u 个）\n", nd);
    }

    printf("\n=== 3) 各模块里含 musl / libc / lock / stdio / init 的导出符号 ===\n");
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        unsigned n = 0;
        printf("  --- %s ---\n", m->name);
        for (unsigned k = 0; k < m->nsym; k++) {
            const char *nm = m->strtab + m->symtab[k].st_name;
            if (!nm[0] || !m->symtab[k].st_shndx) continue;
            if (strstr(nm, "musl") || strstr(nm, "libc") || strstr(nm, "lock") ||
                strstr(nm, "stdio") || strstr(nm, "_init")) {
                if (n < 25) printf("      %-46s value=%#llx\n", nm,
                                   (unsigned long long)m->symtab[k].st_value);
                n++;
            }
        }
        printf("      共 %u 个匹配\n", n);
    }

    printf("\n=== 4) 名字里带 nnmusl 的（不限导出）===\n");
    int tot = 0;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        for (unsigned k = 0; k < m->nsym; k++) {
            const char *nm = m->strtab + m->symtab[k].st_name;
            if (strstr(nm, "nnmusl")) {
                if (tot < 30)
                    printf("    [%s] %-44s value=%#llx %s\n", m->name, nm,
                           (unsigned long long)m->symtab[k].st_value,
                           m->symtab[k].st_shndx ? "导出" : "未定义");
                tot++;
            }
        }
    }
    printf("    共 %d 个\n", tot);

    printf("\n=== result: %s ===\n", found ? "找到 init_locks" : "没找到（见下面分析）");
    return 0;
}
