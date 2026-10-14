/* 自动生成：NVN 函数名表（取自 NmplDLL.nro 的真实字符串）。
   由 work/porting/gen_nvn_names.py 生成，不要手改。
   本头文件只放**声明**；定义在 nvn_names.c。
   （上一版把表定义成 static 放在头里，多个 .c 包含时会各自一份、且与
     hle_nvn.c 的引用对不上 —— 链接期 LNK2019。） */
#ifndef DM_NVN_NAMES_H
#define DM_NVN_NAMES_H

#define NVN_NFUNC 538

extern const char *nvn_names[NVN_NFUNC];

#endif
