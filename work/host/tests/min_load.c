/* 最小复现：只装载 main，逐步检查模块结构是否被写坏。 */
#include "../hle.h"
#include "../loader.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

uint64_t dm_host_ticks(void) { return 0; }

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";
    char bin[512], fix[512];
    unsigned keys[DM_KEY_COUNT] = { 10, 11, 12, 13 };

    dm_module m;
    printf("[1] before: &m=%p sizeof(dm_module)=%zu\n", (void*)&m, sizeof(dm_module));
    fflush(stdout);
    memset(&m, 0, sizeof m);
    snprintf(m.name, sizeof m.name, "%s", "main");
    printf("[2] name set: '%s'\n", m.name);
    fflush(stdout);

    snprintf(bin, sizeof bin, "%s/main.patched.bin", dir);
    snprintf(fix, sizeof fix, "%s/main.fixups.tsv", dir);
    printf("[3] mapping %s\n", bin);
    fflush(stdout);

    int rc = dm_loader_map(&m, bin, fix, keys);
    printf("[4] rc=%d  name='%s'  base=%p  size=%zu\n", rc, m.name, (void*)m.base, m.size);
    fflush(stdout);

    printf("[5] layout: text_end=%#llx key_page=%#llx patch=%#llx img=%llu\n",
           (unsigned long long)m.L.text_end, (unsigned long long)m.L.key_page_off,
           (unsigned long long)m.L.patch_off, (unsigned long long)m.L.image_size);
    printf("[6] nsym=%u symtab=%p strtab=%p\n", m.nsym, (void*)m.symtab, (void*)m.strtab);
    fflush(stdout);

    /* 键页核对 */
    for (unsigned k = 0; k < DM_KEY_COUNT; k++) {
        uint64_t v = *(uint64_t *)(m.base + m.L.key_page_off + m.L.key_idx[k] * 8);
        printf("[7] key[%u] idx=%u val=%llu (expect %llu)\n", k, m.L.key_idx[k],
               (unsigned long long)v, (unsigned long long)keys[k] * 8);
    }
    fflush(stdout);

    /* 符号表抽检 */
    printf("[8] first 3 defined syms:\n");
    unsigned shown = 0;
    for (unsigned k = 1; k < m.nsym && shown < 3; k++) {
        if (m.symtab[k].st_shndx) {
            printf("      %s\n", m.strtab + m.symtab[k].st_name);
            shown++;
        }
    }
    fflush(stdout);

    /* 重定位 */
    dm_load_stats st;
    memset(&st, 0, sizeof st);
    printf("[9] relocating ...\n");
    fflush(stdout);
    dm_loader_relocate(&m, &m, 1, &st);
    printf("[10] relative=%llu abs=%llu jump=%llu unresolved=%llu bad=%llu\n",
           (unsigned long long)st.n_relative, (unsigned long long)st.n_abs,
           (unsigned long long)st.n_jump, (unsigned long long)st.n_unresolved,
           (unsigned long long)st.n_bad_sym);
    printf("[11] name after relocate: '%s'\n", m.name);
    fflush(stdout);
    return 0;
}
