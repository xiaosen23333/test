/* 用装载器打印启动序列符号的真实地址，并核对它们落在哪个段。
 *
 * 为什么不再自己写 Python 解析：我对 sdk 的动态段手动解析失败了（symtab 报 0），
 * 而**装载器是可信的** —— 24,660 条重定位都用它的 dm_loader_sym 解析成功，
 * 且 27 个未解析全部解释清楚了（glslc + TLSDESC）。所以以它为准。
 *
 * 本自测要回答一个对「能不能启动」至关重要的问题：
 *   启动序列的地址是否**落在可执行段里**？（落在 data 段说明地址错了 → 跳过去必崩）
 */
#include "../hle.h"
#include "../loader.h"

#include <stdio.h>
#include <string.h>

static dm_load_ctx ctx;

/* 注意：**不要**在这里定义 dm_host_ticks ——
   build_all.ps1 会链接全部宿主源码（含 hle_nvn_boot.c，那里有真实现），
   在这里再定义一个会 LNK2005 重复定义。单文件手工编译时才需要桩。 */

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    static const char *names[] = { "rtld", "sdk", "main" };
    dm_loader_reset(&ctx);
    ctx.key_values[DM_KEY_K18] = 10; ctx.key_values[DM_KEY_KTLS] = 11;
    ctx.key_values[DM_KEY_KTLR] = 12; ctx.key_values[DM_KEY_KGATE] = 13;

    char bin[512], fix[512];
    for (unsigned i = 0; i < 3; i++) {
        snprintf(bin, sizeof bin, "%s/%s.patched.bin", dir, names[i]);
        snprintf(fix, sizeof fix, "%s/%s.fixups.tsv", dir, names[i]);
        dm_module *m = &ctx.mods[ctx.nmods];
        int rc = dm_loader_map(m, bin, fix, ctx.key_values);
        if (rc) { printf("map %s FAIL rc=%d (%s)\n", names[i], rc, dm_loader_err(rc)); return 1; }
        ctx.nmods++;
    }
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_load_stats st; memset(&st, 0, sizeof st);
        dm_loader_relocate(&ctx.mods[i], ctx.mods, ctx.nmods, &st);
    }

    printf("=== 各模块的段边界（来自 fixups.tsv）===\n");
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        printf("  %-6s base=%p size=%zu  text_end=%#llx ro_off=%#llx data_off=%#llx image=%#llx\n",
               m->name, (void *)m->base, m->size,
               (unsigned long long)m->L.text_end, (unsigned long long)m->L.ro_off,
               (unsigned long long)m->L.data_off, (unsigned long long)m->L.image_size);
    }

    printf("\n=== 启动序列符号的地址与所在段 ===\n");
    int ok = 0, nfound = 0;
    static const char *seq[] = { "_init_libc1", "nnosInitialize", "nninitStartup", "nnMain", "_init" };
    for (unsigned i = 0; i < sizeof seq / sizeof seq[0]; i++) {
        const void *p = dm_loader_sym(ctx.mods, ctx.nmods, seq[i]);
        if (!p) { printf("  %-16s (未找到)\n", seq[i]); continue; }
        uintptr_t a = (uintptr_t)p;
        for (unsigned k = 0; k < ctx.nmods; k++) {
            dm_module *m = &ctx.mods[k];
            uintptr_t b = (uintptr_t)m->base;
            if (a < b || a >= b + m->size) continue;
            uint64_t off = (uint64_t)(a - b);
            const char *seg = (off < m->L.text_end) ? "text"
                            : (off < m->L.ro_off)   ? "ro"
                            : (off < m->L.data_off) ? "data" : "?";
            printf("  %-16s -> %s + %#-8llx  段=%s   %s\n", seq[i], m->name,
                   (unsigned long long)off, seg,
                   (off < m->L.ro_off) ? "**可执行，OK**" : "**在只读/数据段 —— 地址可疑！**");
            nfound++; if (off < m->L.ro_off) ok++;
            break;
        }
    }

    printf("\n=== 交叉核对：%s 的导出符号总数（作为「符号表读对了」的旁证）===\n", "main");
    for (unsigned i = 0; i < ctx.nmods; i++) {
        dm_module *m = &ctx.mods[i];
        unsigned nd = 0;
        for (unsigned k = 1; k < m->nsym; k++)
            if (m->symtab[k].st_shndx && m->strtab[m->symtab[k].st_name]) nd++;
        printf("  %-6s nsym=%6u 已定义且有名=%6u\n", m->name, m->nsym, nd);
    }

    printf("\n=== result: %s ===\n", (ok == nfound && nfound >= 4) ? "ALL PASS（启动序列符号全在 text 段）" : "CHECK FAILED");
    return 0;
}
