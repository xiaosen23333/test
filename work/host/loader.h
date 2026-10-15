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

/* 页保护位（os_protect 用；值与 POSIX 的 PROT_* 一致，Windows 侧在 loader.c 里映射） */
enum { DM_PROT_R = 1, DM_PROT_W = 2, DM_PROT_X = 4 };

/* 动态段 tag（实测确认；上一轮我把 0x17 记成 INIT_ARRAY 是错的） */
enum {
    DM_DT_PLTRELSZ = 0x02, DM_DT_STRTAB = 0x05, DM_DT_SYMTAB = 0x06,
    DM_DT_RELA = 0x07, DM_DT_RELASZ = 0x08, DM_DT_JMPREL = 0x17,
    DM_DT_INIT = 0x0c, DM_DT_INIT_ARRAY = 0x19, DM_DT_INIT_ARRAYSZ = 0x1b,
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
    void          *dt_init;        /* DT_INIT（模块自身的 _init） */
    void         **init_array;     /* DT_INIT_ARRAY：C++ 全局构造函数数组（第 61 轮加） */
    size_t         init_array_n;   /* 条目数（DT_INIT_ARRAYSZ / 8） */
    int            embedded;      /* 1 = 从已签名的 Guest.framework vm_remap 而来（iOS 真机） */
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

/* TLSDESC 的 resolver（第 46 轮加；实现在 loader.c）。
 * 装载器会把它写进每个 TLSDESC 描述符的 [0] 槽 —— 客体取 TLS 变量时**真的会 blr 它**
 * （从解压模块反汇编确认过），所以这一格**不能是 0**，否则就是 `blr 0` 立刻崩。
 * 入参 = 描述符地址；返回 desc[1]（已填好的 TLS 偏移）。 */
uint64_t dm_tlsdesc_resolver(uint64_t *desc);

/* 给一个模块的映像设好页保护（第 48 轮加，**必须在跳进客体之前调**）。
 *
 * 为什么必须有它：
 *   dm_loader_map 只给了「可读可写」的匿名映射（RW），而**内核不会执行不可执行的页**。
 *   于是客体代码的第一条指令就会炸 —— 症状极具误导性：宿主侧的诊断标记
 *   （"调用 _init…"）都正常打出来，之后**永远没有下文**，既没有崩溃行也没有看门狗行
 *   （在 iOS 上执行不可执行页往往是直接被内核处理掉，用户看到的只是"没了"）。
 *   本项目 loader.c 的文件头一直写着「用 mmap + mprotect」，但实现里从来没有
 *   mprotect —— 这就是真机上「卡在启动序列第 1 步」的真因。
 *
 * 设的区域（用 fixups 里的段边界）：
 *   代码 [0, RO_OFF)                  → R+X
 *   rodata [RO_OFF, DATA_OFF)         → R
 *   data/bss/键页                     → R+W
 *   **改写器补丁区** [PATCH_OFF, +LEN) → R+X   ← 客体的改写代码全在这儿
 * 返回 0 表示全部成功；>0 = 失败的区段数（失败原因由 loader.c 记进诊断日志）。 */
int dm_loader_finalize(dm_module *m);
/* 第 61 轮：把 .init_array 条目加上模块基址（必须在 finalize 之前、relocate 之后调）。
 * 背景：本游戏 SDK 的 init_array 与 nn::sf 服务描述表没有 RELATIVE 重定位条目，
 * 真机由 rtld 按「基址相对」约定处理；我们不补的话 SDK 一用就是裸偏移。 */
void dm_loader_fix_init_arrays(dm_module *m);

/* iOS 真机专用：从**会被代码签名的** Guest.framework/Guest 里取段，
 * vm_remap 回原始相对布局（代码页共享已签名页，可写段写时复制）。
 * 为什么必须这样：iOS 不允许普通 App 执行运行时写进内存的代码 ——
 * 匿名页 mmap(RW)→mprotect(R+X) 返回成功但一执行就被内核干掉（第 50 轮实测）。
 * 节布局与 work/tools/guest_dylib.py 的约定一致；两者对不上会直接报错。 */
int dm_loader_map_embedded(dm_module *m, unsigned idx, const char *name,
                           unsigned key_values[DM_KEY_COUNT]);

/* ---- 执行自检（第 49 轮加）----
 *
 * 回答一个此前只能靠猜的问题：**这台设备上，「映射成可读写 → 再 mprotect 成可执行」
 * 的匿名页，到底能不能真的执行？**
 * 做法：mmap 一页 RW → 写入 `ret` → mprotect R+X → 调用它。
 *   · 能正常返回  → 返回 0（客体代码页的机制是成立的）
 *   · 返回 <0     → 返回负的 errno（mprotect/mmap 失败，失败原因已写进诊断日志）
 *   · 进程直接没了 → 那就说明这条路在真机上走不通，**日志最后一行**
 *                   「正在测试匿名页 RW→RX 能否执行」就是证据 —— 下次启动看得到
 * 放在进客体**之前**跑，所以它能把「客体页不可执行」与其它问题彻底分开。 */
int dm_loader_exec_selftest(void);

/* 查一个模块代码页当前的**实际**保护位（不是我们"以为"设了什么）。
 * 写进 out（形如 "RX" / "R" / "RW" / "RWX" / "?"），返回 0 成功、<0 失败。
 * Apple 上用 mach_vm_region；其它平台上返回 -1（不支持）。 */
int dm_loader_query_prot(const dm_module *m, char *out, int n);

#endif /* DM_LOADER_H */
