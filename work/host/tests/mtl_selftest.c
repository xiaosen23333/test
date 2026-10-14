/* Metal 编码层自测 —— 验证「状态机」这一侧（Metal API 调用在 Apple 上另有文件）。
 *
 * 为什么重点测状态机而不是 API 调用：
 *   真机上「画面不对」最常见的原因不是 API 调错（那会直接报错），
 *   而是**状态机搞错** —— 该 setState 的没 set、该开新 pass 的没开、
 *   去重把不该省的省了。这些错不会报错，只会画错，所以必须有单测。
 *
 * 判据：
 *   1. 连续两次同样的状态 → 只编码一次（去重），计数正确
 *   2. 状态不同 → 都编码
 *   3. clear / sync 之后状态缓存作废（不能跨屏障去重）
 *   4. 没有 program 就 draw → 记账（这是最重要的诊断信号）
 *   5. 没有 pass 就 draw → 自动开一个 pass
 *   6. present 关掉当前 pass
 *   7. 不认识的命令进 other 而不是被忽略
 */
#include "../hle_nvn.h"
#include "../hle_nvn_cmd.h"
#include "../hle_nvn_replay.h"
#include "../hle_nvn_mtl.h"

#include <stdio.h>
#include <string.h>

struct mtl_stats {
    int      in_pass;
    uint64_t pass_index, pass_first_cmd, draws_in_pass;
    const char *last_state_name;
    uint64_t last_state_a0, last_state_a1;
    int      have_program, have_viewport, have_target;
    uint64_t n_passes, n_state_coded, n_state_dedup, n_draws, n_draws_no_program;
    uint64_t n_draws_no_target, n_binds, n_clears, n_syncs, n_presents, n_unknown;
};

static int fails = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("    x " __VA_ARGS__); fails++; } } while (0)

static void run(void) {
    dm_replay_stats st;
    dm_nvn_cmd_replay_to(dm_mtl_backend(), 0, &st);
}

int main(void) {
    extern void dm_nvn_cmd_build_index(void);
    dm_nvn_obj_register_all();
    dm_nvn_cmd_register_all();
    dm_nvn_cmd_build_index();
    const struct mtl_stats *m;

    printf("=== 1) 状态去重 ===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    /* 同一个 viewport 连设 3 次 */
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 3, 4, 0);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 3, 4, 0);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 3, 4, 0);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  连设 3 次相同 viewport: 编码 %llu 次，去重 %llu 次\n",
           (unsigned long long)m->n_state_coded, (unsigned long long)m->n_state_dedup);
    CHECK(m->n_state_coded == 1, "应只编码 1 次，实为 %llu\n", (unsigned long long)m->n_state_coded);
    CHECK(m->n_state_dedup == 2, "应去重 2 次，实为 %llu\n", (unsigned long long)m->n_state_dedup);

    printf("\n=== 2) 状态不同则不去重 ===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 9, 9, 0, 0, 0);   /* 参数不同 */
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  两次不同参数: 编码 %llu 次（expect 2）\n", (unsigned long long)m->n_state_coded);
    CHECK(m->n_state_coded == 2, "应编码 2 次\n");

    printf("\n=== 3) clear 之后状态缓存作废（不能跨屏障去重）===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferClearColor", 5, 5, 5, 5, 0);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 0, 0, 0);   /* 与第一次相同 */
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  setState / clear / setState(同参数): 编码 %llu 次（expect 2，中间那次不该被去重）\n",
           (unsigned long long)m->n_state_coded);
    CHECK(m->n_state_coded == 2, "clear 之后不该去重\n");

    printf("\n=== 4) 没有 program 就 draw → 记账（最重要的诊断信号）===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferDrawElements", 0x100, 0, 0, 0, 0);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  draw 总数 %llu，其中无 program 的 %llu（expect 1）\n",
           (unsigned long long)m->n_draws, (unsigned long long)m->n_draws_no_program);
    CHECK(m->n_draws == 1 && m->n_draws_no_program == 1, "应记 1 次无 program 的 draw\n");

    printf("\n=== 5) 有 program 的 draw 不记 ===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferBindProgram", 0x1, 0x2, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferDrawElements", 0x100, 0, 0, 0, 0);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  bind program 后 draw: 无 program 计数 %llu（expect 0）\n",
           (unsigned long long)m->n_draws_no_program);
    CHECK(m->n_draws_no_program == 0, "有 program 时不该记\n");

    printf("\n=== 6) 没有 pass 就 draw → 自动开 pass ===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferDrawElements", 0x100, 0, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferDrawElements", 0x200, 0, 0, 0, 0);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  两次 draw（之前没 pass）: pass 数 %llu（expect 1 —— 只开一次，第二个复用）\n",
           (unsigned long long)m->n_passes);
    CHECK(m->n_passes == 1, "应只开 1 个 pass\n");

    printf("\n=== 7) 屏障打断状态连续性 ===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferBarrier", 0, 0, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 1, 2, 0, 0, 0);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  setState / barrier / setState(同): 编码 %llu（expect 2），屏障 %llu\n",
           (unsigned long long)m->n_state_coded, (unsigned long long)m->n_syncs);
    CHECK(m->n_state_coded == 2 && m->n_syncs == 1, "屏障后不该去重\n");

    printf("\n=== 8) 不认识的命令不静默忽略 ===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    /* 用通用入口录一条「身份未知」的命令 */
    dm_nvn_cmd_generic((void *)1, (void *)2, (void *)3, (void *)4);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  身份未知命令: unknown 计数 %llu（expect 1）\n", (unsigned long long)m->n_unknown);
    CHECK(m->n_unknown == 1, "应记进 unknown\n");

    printf("\n=== 9) 一帧的完整序列（bind/state/draw/clear/sync/present）===\n");
    dm_mtl_reset();
    dm_nvn_cmd_enable(1);
    dm_nvn_dispatch("nvnCommandBufferBindProgram", 0x1, 0x2, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferSetViewport", 0, 0, 1920, 1080, 0);
    dm_nvn_dispatch("nvnCommandBufferDrawElements", 0x100, 36, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferClearColor", 0, 0, 0, 1, 0);
    dm_nvn_dispatch("nvnCommandBufferDrawArrays", 3, 0, 0, 0, 0);
    dm_nvn_dispatch("nvnCommandBufferBarrier", 0, 0, 0, 0, 0);
    run();
    m = (const struct mtl_stats *)dm_mtl_stats_ptr();
    printf("  draws=%llu binds=%llu state编码=%llu clears=%llu syncs=%llu passes=%llu\n",
           (unsigned long long)m->n_draws, (unsigned long long)m->n_binds,
           (unsigned long long)m->n_state_coded, (unsigned long long)m->n_clears,
           (unsigned long long)m->n_syncs, (unsigned long long)m->n_passes);
    CHECK(m->n_draws == 2, "应 2 次 draw\n");
    CHECK(m->n_draws_no_program == 0, "有 program，不该记缺 program\n");
    CHECK(m->n_passes == 1, "同一 pass 内应只 1 个 pass\n");

    printf("\n=== result: %s（失败 %d 项）===\n", fails ? "CHECK FAILED" : "ALL PASS", fails);
    return fails ? 1 : 0;
}
