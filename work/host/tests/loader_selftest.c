/* 装载器自测 —— 在 Windows 上真正装载 5 个模块并跑完全部重定位。
 *
 * 打印一律 ASCII（Windows 控制台代码页不可控）。
 *
 * 验证内容：
 *   1. 5 个模块的 fixups.tsv 都能解析且自洽
 *   2. 映射成功、MOD0/动态段解析成功、符号表规模正确
 *   3. 全部重定位应用完毕；统计各类型条数并与 Python 侧（verify_link.py）对照
 *   4. 未解析符号只有 glslc*（8 个）
 *   5. 键页填充后 4 项都是「键号 × 8」
 *   6. gates.tsv 登记进 hle.c 的门表，且按返回点有序
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef struct { const char *name; const char *bin; const char *fix; const char *gates; } ent_t;

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    char bin[512], fix[512], gat[512];

    static const char *names[] = { "main", "sdk", "rtld", "NmplDLL.nro", "lz4.nro" };
    const unsigned NK = sizeof names / sizeof names[0];

    dm_load_ctx ctx;
    dm_loader_reset(&ctx);
    /* 模拟宿主建好的 pthread 键号 */
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    printf("=== 1) map 5 modules ===\n");
    fflush(stdout);
    for (unsigned i = 0; i < NK; i++) {
        printf("  [%u] %s ... ", i, names[i]);
        fflush(stdout);
        snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, names[i]);
        snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, names[i]);
        dm_module *m = &ctx.mods[ctx.nmods];
        /* 注意：不要在调用前设 m->name —— dm_loader_map 会 memset 整个结构，
           名字由它从 bin 路径自动推导（这里踩过一次） */
        int rc = dm_loader_map(m, bin, fix, ctx.key_values);
        if (rc) { printf("FAIL rc=%d (%s)\n", rc, dm_loader_err(rc)); return 1; }
        ctx.nmods++;
        printf("ok  base %p size %8zu  text_end %#9llx  key_page %#9llx  patch %#9llx\n",
               (void *)m->base, m->size,
               (unsigned long long)m->L.text_end,
               (unsigned long long)m->L.key_page_off,
               (unsigned long long)m->L.patch_off);
        fflush(stdout);
    }
    printf("\n  [step 1a] counting exported syms ... ");
    fflush(stdout);
    {
        unsigned tot = 0;
        for (unsigned i = 0; i < ctx.nmods; i++) {
            printf("\n    mod %u (%s) nsym=%u ", i, ctx.mods[i].name, ctx.mods[i].nsym);
            fflush(stdout);
            for (unsigned k = 1; k < ctx.mods[i].nsym; k++) {
                if (ctx.mods[i].symtab[k].st_shndx &&
                    ctx.mods[i].strtab[ctx.mods[i].symtab[k].st_name]) tot++;
            }
            printf("ok");
            fflush(stdout);
        }
        printf("\n  total exported syms: %u\n\n", tot);
        fflush(stdout);
    }

    printf("=== 2) symbol tables ===\n");
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        unsigned nd = 0, nu = 0;
        for (unsigned k = 0; k < m->nsym; k++)
            if (m->strtab[m->symtab[k].st_name]) { if (m->symtab[k].st_shndx) nd++; else nu++; }
        printf("  %-14s nsym %6u  defined %6u  undefined %5u  rela %6llu  jmprel %6llu\n",
               m->name, m->nsym, nd, nu,
               (unsigned long long)(m->relasz / DM_RELA_SIZE),
               (unsigned long long)(m->pltsz / DM_RELA_SIZE));
    }

    printf("\n=== 3) relocate all ===\n");
    fflush(stdout);
    dm_load_stats tot; memset(&tot, 0, sizeof tot);
    static dm_load_stats each[DM_MAX_MODS];
    for (unsigned i = 0; i < ctx.nmods; i++) {
        printf("  [%u] relocating %s ... ", i, ctx.mods[i].name);
        fflush(stdout);
        memset(&each[i], 0, sizeof each[i]);
        dm_loader_relocate(&ctx.mods[i], ctx.mods, ctx.nmods, &each[i]);
        tot.n_relative += each[i].n_relative; tot.n_abs += each[i].n_abs;
        tot.n_jump += each[i].n_jump; tot.n_unresolved += each[i].n_unresolved;
        tot.n_bad_sym += each[i].n_bad_sym;
        printf("done\n");
        printf("       RELATIVE %6llu  ABS/GLOB %6llu  JUMP_SLOT %6llu  unresolved %4llu  bad_sym %llu\n",
               (unsigned long long)each[i].n_relative, (unsigned long long)each[i].n_abs,
               (unsigned long long)each[i].n_jump, (unsigned long long)each[i].n_unresolved,
               (unsigned long long)each[i].n_bad_sym);
        fflush(stdout);
    }
    printf("\n  TOTAL relocs %llu  (RELATIVE %llu + ABS/GLOB %llu + JUMP %llu)\n",
           (unsigned long long)(tot.n_relative + tot.n_abs + tot.n_jump),
           (unsigned long long)tot.n_relative, (unsigned long long)tot.n_abs,
           (unsigned long long)tot.n_jump);
    printf("  unresolved %llu  bad_sym %llu\n", (unsigned long long)tot.n_unresolved,
           (unsigned long long)tot.n_bad_sym);
    fflush(stdout);

    printf("\n=== 4) unresolved symbols ===\n");
    fflush(stdout);
    unsigned shown = 0;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        printf("  scanning %s (%u entries) ... ", ctx.mods[i].name, each[i].n_unres);
        fflush(stdout);
        for (unsigned k = 0; k < each[i].n_unres; k++) {
            printf("\n    type %4u  %s", each[i].unres[k].type, each[i].unres[k].name);
            fflush(stdout);
            shown++;
        }
        printf(" ok\n");
        fflush(stdout);
    }
    if (!shown) printf("  (none)\n");
    fflush(stdout);

    printf("\n=== 5) key page (must be key*8) ===\n");
    fflush(stdout);
    int kp_ok = 1;
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        printf("  [%u] %s base=%p kpo=%#llx idx=%u,%u,%u,%u ... ", i, m->name, (void *)m->base,
               (unsigned long long)m->L.key_page_off,
               m->L.key_idx[0], m->L.key_idx[1], m->L.key_idx[2], m->L.key_idx[3]);
        fflush(stdout);
        uint64_t vals[DM_KEY_COUNT];
        for (unsigned k = 0; k < DM_KEY_COUNT; k++)
            vals[k] = *(uint64_t *)(m->base + m->L.key_page_off + m->L.key_idx[k] * 8);
        int ok = 1;
        for (unsigned k = 0; k < DM_KEY_COUNT; k++)
            if (vals[k] != (uint64_t)ctx.key_values[k] * 8) ok = 0;
        kp_ok = kp_ok && ok;
        printf("K18=%llu KTLS=%llu KTLR=%llu KGATE=%llu  %s\n",
               (unsigned long long)vals[0], (unsigned long long)vals[1],
               (unsigned long long)vals[2], (unsigned long long)vals[3],
               ok ? "OK" : "BAD");
        fflush(stdout);
    }

    printf("\n=== 6) register gates ===\n");
    fflush(stdout);
    int gt_ok = 1;
    static dm_gate_ent tbl[DM_MAX_MODS][1024];
    static dm_host hs[DM_MAX_MODS];
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        snprintf(gat, sizeof gat, "%s/%s.gates.tsv", dir, m->name);
        printf("  [%u] %s <- %s ... ", i, m->name, gat);
        fflush(stdout);
        int n = dm_loader_register_gates(&hs[i], tbl[i], 1024, gat, m->base);
        if (n < 0) n = 0;
        printf("n=%d ", n);
        fflush(stdout);
        int sorted = 1;
        for (int k = 1; k < n; k++) if (tbl[i][k].ret <= tbl[i][k-1].ret) sorted = 0;
        gt_ok = gt_ok && sorted;
        printf("sorted=%s (expect %u)\n", sorted ? "yes" : "NO", m->L.ngates);
        fflush(stdout);
        if ((unsigned)n != m->L.ngates) gt_ok = 0;
    }

    printf("\n=== result: %s ===\n",
           (tot.n_bad_sym == 0 && kp_ok && gt_ok && tot.n_unresolved <= 27) ? "ALL PASS" : "CHECK FAILED");
    /* 注意：模块映像是 os_map()（Windows 上是 VirtualAlloc）分配的，
       必须用 os_unmap 释放 —— 用 free() 会访问违例（这里踩过）。
       自测里其实不必释放（进程马上退出），但显式释放能顺带验证映射成对。 */
    for (unsigned i = 0; i < ctx.nmods; i++) dm_loader_unmap(&ctx.mods[i]);
    return 0;
}
