/* NVN 对象层自测 —— 验证 device / memory pool / buffer / texture / window 的语义。
 *
 * 这是图形层里**能被真正验证**的那部分：对象创建、属性查询、地址计算。
 * 「画到屏幕上」要等 Metal 后端，但「对象是真的、地址算得对」现在就能验。
 */
#include "../hle_nvn.h"
#include "../hle_nvn_obj.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void) {
    printf("=== 1) 设备属性（Tegra X1 值）===\n");
    printf("  API 版本 major/minor: %d / %d\n", dm_nvn_devinfo(0x00), dm_nvn_devinfo(0x01));
    printf("  最大纹理尺寸 0x03 : %d\n", dm_nvn_devinfo(0x03));
    printf("  纹理对齐   0x0E : %d\n", dm_nvn_devinfo(0x0E));
    printf("  缓冲对齐   0x15 : %d\n", dm_nvn_devinfo(0x15));
    int api_ok = (dm_nvn_devinfo(0x00) == 53 && dm_nvn_devinfo(0x01) == 311);

    printf("\n=== 2) 设备创建 ===\n");
    struct { void *p; } dev; memset(&dev, 0, sizeof dev);
    int r = dm_nvn_DeviceInitialize(&dev, NULL);
    printf("  DeviceInitialize -> %d (expect 1)\n", r);
    printf("  DeviceGetInteger(API major) -> %d\n", dm_nvn_DeviceGetInteger(&dev, 0x00));

    printf("\n=== 3) 内存池 + 缓冲（地址计算是关键）===\n");
    /* 游戏给一块真内存当池 */
    static unsigned char pool_mem[1 << 20];
    struct { void *p; } pool; memset(&pool, 0, sizeof pool);
    dm_nvn_MemoryPoolInitialize(&pool, &dev, pool_mem, sizeof pool_mem, 0);
    printf("  MemoryPoolInitialize(addr=%p, size=%zu)\n", (void *)pool_mem, sizeof pool_mem);
    printf("  GetAddress -> %s (expect == pool_mem)\n",
           dm_nvn_MemoryPoolGetAddress(&pool) == pool_mem ? "相同 OK" : "不同 <-- 不符");
    printf("  GetSize    -> %llu (expect %zu)\n",
           (unsigned long long)dm_nvn_MemoryPoolGetSize(&pool), sizeof pool_mem);

    struct { void *p; } buf; memset(&buf, 0, sizeof buf);
    dm_nvn_BufferInitialize(&buf, &dev, &pool, 0x1000, 0x2000);
    uint64_t addr = dm_nvn_BufferGetAddress(&buf);
    uint64_t want = (uint64_t)(uintptr_t)pool_mem + 0x1000;
    printf("  BufferInitialize(offset=0x1000, size=0x2000)\n");
    printf("  GetAddress -> %#llx (expect %#llx)  %s\n",
           (unsigned long long)addr, (unsigned long long)want,
           addr == want ? "OK" : "<-- 地址算错");
    printf("  GetSize    -> %llu (expect 8192)\n", (unsigned long long)dm_nvn_BufferGetSize(&buf));
    int buf_ok = (addr == want && dm_nvn_BufferGetSize(&buf) == 8192);

    printf("\n=== 4) 纹理 ===\n");
    struct { void *p; } tex; memset(&tex, 0, sizeof tex);
    dm_nvn_TextureInitialize(&tex, &dev, 0 /*TARGET_2D*/, 1920, 1080, 1, 1, 0x8D /*RGBA8*/, 0, 0);
    printf("  TextureInitialize(1920x1080, levels=1, fmt=0x8D)\n");
    printf("  GetWidth/Height/Depth/Levels/Format -> %d / %d / %d / %d / %#x\n",
           dm_nvn_TextureGetWidth(&tex), dm_nvn_TextureGetHeight(&tex),
           dm_nvn_TextureGetDepth(&tex), dm_nvn_TextureGetLevels(&tex),
           dm_nvn_TextureGetFormat(&tex));
    int tex_ok = (dm_nvn_TextureGetWidth(&tex) == 1920 && dm_nvn_TextureGetHeight(&tex) == 1080 &&
                  dm_nvn_TextureGetFormat(&tex) == 0x8D);

    printf("\n=== 5) 窗口 ===\n");
    struct { void *p; } win; memset(&win, 0, sizeof win);
    dm_nvn_WindowInitialize(&win, &dev, NULL);
    dm_nvn_WindowSetCrop(&win, 0, 0, 1280, 720);
    printf("  WindowSetCrop(1280x720) -> GetWidth/Height = %d / %d\n",
           dm_nvn_WindowGetWidth(&win), dm_nvn_WindowGetHeight(&win));
    int win_ok = (dm_nvn_WindowGetWidth(&win) == 1280 && dm_nvn_WindowGetHeight(&win) == 720);

    printf("\n=== 6) 注册进分发表 ===\n");
    dm_nvn_obj_register_all();
    unsigned reg = dm_nvn_registered();
    printf("  已注册 NVN 实现: %u (expect 27)\n", reg);
    /* 用分发链验证：按名字调用应进实现而不是未实现入口 */
    uint64_t got_api = dm_nvn_dispatch("nvnDeviceGetInteger", (uint64_t)(uintptr_t)&dev, 0x00, 0, 0, 0);
    printf("  经分发调用 nvnDeviceGetInteger(0x00) -> %llu (expect 53)  %s\n",
           (unsigned long long)got_api, got_api == 53 ? "OK" : "<-- 没进实现");
    int dispatch_ok = (got_api == 53 && reg == 27);

    printf("\n=== 7) bootstrap 应能按名给出实现 ===\n");
    void *p = dm_nvn_bootstrap("nvnBufferGetAddress");
    printf("  bootstrap(\"nvnBufferGetAddress\") -> %s\n",
           p == (void *)dm_nvn_BufferGetAddress ? "等于实现 OK" : "不符");

    printf("\n=== 8) 清理 ===\n");
    dm_nvn_TextureFinalize(&tex);
    dm_nvn_BufferFinalize(&buf);
    dm_nvn_MemoryPoolFinalize(&pool);
    dm_nvn_WindowFinalize(&win);
    dm_nvn_DeviceFinalize(&dev);
    /* 重复 Finalize 不应崩（对象已被置空） */
    dm_nvn_TextureFinalize(&tex);
    printf("  全部 Finalize + 重复 Finalize 未崩 OK\n");

    printf("\n=== result: %s ===\n",
           (api_ok && r == 1 && buf_ok && tex_ok && win_ok && dispatch_ok) ? "ALL PASS" : "CHECK FAILED");
    return 0;
}
