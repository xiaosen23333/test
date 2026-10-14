/* 宿主图形层 —— NVN 接口层的声明。 */
#ifndef DM_HLE_NVN_H
#define DM_HLE_NVN_H

#include <stdint.h>

/* 名字表规模（由 nvn_names.h 生成，取自 NmplDLL.nro 的真实字符串） */
#define NVN_NFUNC 538

typedef struct { uint64_t cnt; void *fn; } nvn_slot_t;

/* 名字 → 下标 */
int dm_nvn_index_of(const char *name);

/* 分发（Windows 自测用；真机侧由 entry.S 的跳板完成同样的跳转） */
uint64_t dm_nvn_dispatch(const char *name, uint64_t a0, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4);

/* 注册实现 */
int      dm_nvn_register(const char *name, void *impl);   /* 1 = 成功，0 = 名字不在表里 */
unsigned dm_nvn_registered(void);

/* nvnBootstrapLoader */
void *dm_nvn_bootstrap(const char *name);

/* 未实现入口与诊断 */
uint64_t dm_nvn_unimpl(int idx, uint64_t x0, uint64_t x1);
void     dm_nvn_dump_unimpl(void (*emit)(const char *name, void *ud), void *ud);
const void *dm_nvn_stats_ptr(void);

/* 名字表（由 nvn_names.h 提供） */
extern const char *nvn_names[NVN_NFUNC];
extern nvn_slot_t nvn_slot[NVN_NFUNC];

#endif /* DM_HLE_NVN_H */
