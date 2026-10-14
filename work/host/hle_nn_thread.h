/* 宿主 HLE —— nn::os 的线程 / TLS / 信号量 / 系统事件 / 计时（本轮新增 23 个）。 */
#ifndef DM_HLE_NN_THREAD_H
#define DM_HLE_NN_THREAD_H

#include <stdint.h>

/* 线程 */
void     dm_nn_CreateThread(void *t, void *entry, void *arg, void *stack,
                            uint64_t stack_size, int prio, int core);
void     dm_nn_StartThread(void *t);
void     dm_nn_WaitThread(void *t);
void     dm_nn_DestroyThread(void *t);
void     dm_nn_SetThreadName(void *t, const char *name);
int      dm_nn_GetThreadPriority(const void *t);
void     dm_nn_ChangeThreadPriority(void *t, int prio);
void     dm_nn_SetThreadCoreMask(void *t, int ideal, uint64_t mask);
uint64_t dm_nn_GetThreadAvailableCoreMask(void);
const void *dm_nn_thread_stats_ptr(void);

/* 客体线程入口（真机上是 entry.S 的 _dm_thread_enter，切到客体栈） */
void     dm_thread_enter(void *entry, void *arg, void *sp);
void     dm_thread_returned(void);

/* 计时 */
uint64_t dm_nn_GetSystemTick(void);
uint64_t dm_nn_ConvertToTick(int64_t ns);

/* TLS */
int      dm_nn_AllocateTlsSlot(uint32_t *out, void (*dtor)(uint64_t));
void     dm_nn_FreeTlsSlot(uint32_t slot);
void     dm_nn_SetTlsValue(uint32_t slot, uint64_t v);
uint64_t dm_nn_GetTlsValue(uint32_t slot);
const void *dm_nn_tls_stats_ptr(void);

/* 信号量 */
void dm_nn_InitializeSemaphore(void *s, int initial, int maxcount);
void dm_nn_FinalizeSemaphore(void *s);
void dm_nn_AcquireSemaphore(void *s);
void dm_nn_ReleaseSemaphore(void *s, int n);

/* 系统事件 */
void dm_nn_WaitSystemEvent(void *e);
void dm_nn_DestroySystemEvent(void *e);
void dm_nn_TimedWaitLightEvent(void *e, int64_t ns);

/* 异常 */
void dm_nn_detail_UserExceptionHandler(void);

#endif /* DM_HLE_NN_THREAD_H */
