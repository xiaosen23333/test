/* 装载器的接口与数据结构。
 *
 * 与 hle.h 配对：loader 负责「映射 + 链接 + 填键页 + 登记门」，
 * hle 负责「门的分发与系统调用实现」。
 */
#ifndef DM_LOADER_H
#define DM_LOADER_H

#include <stdint.h>
#include <stddef.h>
#include "hle.h"

/* 键槽（与改写器的 KEY_ORDER 一致） */
enum { DM_KEY_K18 = 0, DM_KEY_KTLS, DM_KEY_KTLR, DM_KEY_KGATE, DM_KEY_COUNT };

#define DM_RELA_SIZE 24
#define DM_SYM_SIZE  24
#define DM_MAX_MODS  8
#define DM_MAX_UNRES 64

/* ELF64 动态段 / 符号 / 重定位（与 Horizon 一致） */
typedef struct { uint64_t d_tag; union { uint64_t d_val; uint64_t d_ptr; } d_un; } dm_dyn;
typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx;
                 uint64_t st_value, st_size; } dm_sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } dm_rela;

/* 动态段 tag（实测确认；上一轮我把 0x17 记成 INIT_ARRAY 是错的） */
enum {
    DM_DT_PLTRELSZ = 0x02, DM_DT_STRTAB = 0x05, DM_DT_SYMTAB = 0x06,
    DM_DT_RELA = 0x07, DM_DT_RELASZ = 0x08, DM_DT_JMPREL = 0x17,
};

/* 重定位类型 */
enum {
    DM_R_AARCH64_ABS64     = 257,
    DM_R_AARCH64_GLOB_DAT  = 1025,
    DM_R_AARCH64_JUMP_SLOT = 1026,
    DM_R_AARCH64_RELATIVE  = 1027,
    DM_R_AARCH64_TLSDESC   = 1031,
};

/* fixups.tsv 解析结果 */
typedef struct {
    uint64_t patch_off, key_page_off, key_page_size;
    uint64_t text_end, ro_off, data_off;
    uint64_t image_size, patch_len;
    uint32_t ngates;
    uint32_t key_idx[DM_KEY_COUNT];
    char     build_id[80];
} dm_layout;

typedef struct {
    char      name[32];
    uint8_t  *base;
    size_t    size;
    dm_layout L;
    const dm_dyn  *dyn;
    const dm_sym  *symtab;
    const char    *strtab;
    unsigned       nsym;
    const dm_rela *rela, *jmprel;
    uint64_t       relasz, pltsz;
} dm_module;

typedef struct {
    uint64_t n_relative, n_abs, n_jump, n_unresolved, n_bad_sym, n_tlsdesc;
    struct { const char *name; unsigned type; } unres[DM_MAX_UNRES];
    unsigned n_unres;
} dm_load_stats;

typedef struct {
    dm_module mods[DM_MAX_MODS];
    unsigned  nmods;
    unsigned  key_values[DM_KEY_COUNT];   /* 宿主建好的 pthread 键号 */
} dm_load_ctx;

/* --- 接口 --- */
void dm_loader_reset(dm_load_ctx *c);
int  dm_loader_read_fixups(dm_layout *L, const char *path);
int  dm_loader_map(dm_module *m, const char *bin_path, const char *fixups_path,
                   unsigned key_values[DM_KEY_COUNT]);
int  dm_loader_relocate(dm_module *m, const dm_module *mods, unsigned nmods,
                        dm_load_stats *st);
const void *dm_loader_sym(const dm_module *mods, unsigned nmods, const char *name);
/* 兜底解析器：客体引用但不在任何模块里的符号（本游戏只有 glslc*） */
void dm_loader_set_fallback(void *(*fn)(const char *name));
int  dm_loader_register_gates(dm_host *h, dm_gate_ent *tbl, size_t cap,
                              const char *gates_path, uint8_t *base);
const char *dm_loader_err(int rc);
/* 释放 dm_loader_map 分配的映像（必须用它，不能用 free —— 见 loader.c 的平台抽象） */
void dm_loader_unmap(dm_module *m);

#endif /* DM_LOADER_H */
