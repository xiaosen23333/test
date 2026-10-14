/* 验证 SDK 运行时初始化序列所需的符号，重点是 `_init_libc1`。
 *
 * 背景（D2R 的 main.c 第 60 行原文）：
 *   逐条照抄 Start：_init_libc1 = __nnmusl_init_locks（把 stdio 等锁初始化成递归锁，
 *   略过会在 putc 嵌套加锁时自锁死）
 *
 * 本自测回答三个问题：
 *   1. `_init_libc1` 在哪个模块、什么地址（D2R 注释里的 __nnmusl_init_locks 名字在本游戏不存在）
 *   2. SDK 的 libc 是不是**客体自带**的（若是，则锁由客体自己初始化，宿主不必代劳）
 *   3. 宿主需要替客体做的 libc 相关符号有哪些（未定义的那些）
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static dm_load_ctx ctx;
static const char *names[] = { "rtld", "sdk", "main", "NmplDLL.nro", "lz4.nro" };

static void where(const char *sym) {
    const void *p = dm_loader_sym(ctx.mods, ctx.nmods, sym);
    if (!p) { printf("  %-24s (未找到)\n", sym); return; }
    uintptr_t a = (uintptr_t)p;
    for (unsigned k = 0; k < ctx.nmods; k++) {
        uintptr_t b = (uintptr_t)ctx.mods[k].base;
        if (a >= b && a < b + ctx.mods[k].size) {
            printf("  %-24s -> %s + %#llx\n", sym, ctx.mods[k].name,
                   (unsigned long long)(a - b));
            return;
        }
    }
    printf("  %-24s -> %p（不在任何模块内？）\n", sym, p);
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    dm_loader_reset(&ctx);
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    char bin[512], fix[512];
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, names[i]);
        snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, names[i]);
        dm_module *m = &ctx.mods[ctx.nmods];
        int rc = dm_loader_map(m, bin, fix, ctx.key_values);
        if (rc) { printf("map %s FAIL rc=%d\n", names[i], rc); return 1; }
        ctx.nmods++;
    }
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_load_stats st; memset(&st, 0, sizeof st);
        dm_loader_relocate(&ctx.mods[i], ctx.mods, ctx.nmods, &st);
    }

    printf("=== 1) SDK 运行时初始化序列的符号 ===\n");
    static const char *seq[] = {
        "_init_libc1", "__nnmusl_init_locks", "nnosInitialize", "nninitStartup", "nnMain",
        "__libc_init_array", "_init", "__init_array_start",
    };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) where(seq[i]);

    printf("\n=== 2) SDK 的 libc 是客体自带还是宿主提供？ ===\n");
    printf("  （判据：这些名字在哪个模块里**有定义**（st_shndx != 0））\n");
    static const char *libc[] = {
        "putchar_unlocked", "putc", "fputc", "fwrite", "fflush",
        "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_init",
        "malloc", "free", "memcpy", "printf",
    };
    for (unsigned i = 0; i < sizeof libc / sizeof libc[0]; i++) {
        int defined_in = -1, undefined_in = -1;
        for (unsigned k = 0; k < ctx.nmods; k++) {
            dm_module *m = &ctx.mods[k];
            for (unsigned s = 0; s < m->nsym; s++) {
                const char *nm = m->strtab + m->symtab[s].st_name;
                if (strcmp(nm, libc[i])) continue;
                if (m->symtab[s].st_shndx) { if (defined_in < 0) defined_in = (int)k; }
                else if (undefined_in < 0) undefined_in = (int)k;
            }
        }
        printf("  %-22s ", libc[i]);
        if (defined_in >= 0)  printf("定义于 %s  ", ctx.mods[defined_in].name);
        if (undefined_in >= 0) printf("被 %s 引用", ctx.mods[undefined_in].name);
        if (defined_in < 0 && undefined_in < 0) printf("(都没有)");
        printf("\n");
    }

    printf("\n=== 3) 宿主需要替客体实现的 libc 符号（被引用但无定义）===\n");
    unsigned need = 0;
    for (unsigned k = 0; k < ctx.nmods; k++) {
        dm_module *m = &ctx.mods[k];
        for (unsigned s = 1; s < m->nsym; s++) {
            const char *nm = m->strtab + m->symtab[s].st_name;
            if (!nm[0] || m->symtab[s].st_shndx) continue;
            /* 未定义：看别处有没有定义 */
            const void *d = dm_loader_sym(ctx.mods, ctx.nmods, nm);
            if (!d && need < 40) {
                printf("    [%s] %s\n", m->name, nm);
                need++;
            }
        }
    }
    printf("    （前 40 个；总数见下）\n");

    unsigned tot = 0;
    for (unsigned k = 0; k < ctx.nmods; k++) {
        dm_module *m = &ctx.mods[k];
        for (unsigned s = 1; s < m->nsym; s++) {
            const char *nm = m->strtab + m->symtab[s].st_name;
            if (!nm[0] || m->symtab[s].st_shndx) continue;
            if (!dm_loader_sym(ctx.mods, ctx.nmods, nm)) tot++;
        }
    }
    printf("    实际未解析总数: %u\n", tot);

    printf("\n=== result: %s ===\n", (tot == 8) ? "ALL PASS" : "CHECK: unresolved != 8");
    return 0;
}
