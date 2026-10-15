# 检查「先调用、后声明」——clang 16 起这在 C99+ 是**硬错误**（不是警告）：
#     error: call to undeclared function 'xxx';
#            ISO C99 and later do not support implicit function declarations
#
# 为什么需要这个脚本：
#   本机是 Windows，**编不了 ObjC（front_ios.m / main.m）**，也没法跑 clang。
#   这类错误只有朋友的 Mac 上才会暴露 —— 一次来回就是一天。
#   第 43 轮我已经因此漏了两处（front_ios.m 里的 status_set 与 dm_ios_start），
#   所以把「人工逐行看」变成「脚本扫」，推给对方之前先跑一遍。
#
# 做法（启发式，够用）：
#   ① 去掉注释与字符串字面量（保留行号）
#   ② 找出本文件里**定义/声明**的函数名：标识符出现在行首（前面只有返回类型/修饰符）
#   ③ 找出这些名字的全部 `名字(` 出现位置
#   ④ 若某次出现早于「最早的行首声明」，就是「先调用后声明」——报出来
import os, re, sys
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

# 根目录 = 本脚本所在目录的上一级（即 work/）—— 这样在 Windows、Mac、CI 上都能跑
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

TARGETS = [
    r'ios\main.m', r'ios\front_ios.m',
]
HOST = r'host'
SKIP = {'nn_impl_table.c', 'nvn_names.c', 'nn_expected.h'}   # 生成的表，含大量数据

# 行首声明的样子：**至少一个**返回类型/修饰符 token + 名字 + 紧跟 '('
# 例： static void status_set(NSString *fmt, ...) {
#      int dm_ios_start(void) {
#      const char *dm_host_res_dir(void) {
#      static dm_box *box_of(void *obj, int is_event) {      <- 星号紧贴名字也要认
#
# 两个刻意的取舍（都是为了压掉误报）：
#   · 类型部分要求「非空」→ `printf("...")` 这种行首调用不会被当成声明
#     （行首调用前面没有返回类型，只有它自己那个标识符）
#   · 名字前允许 `*` 紧贴（`dm_box *box_of` 里 `*` 与名字之间没空格）
DECL_RE = re.compile(
    r'^\s*'                                  # 行首（含缩进）
    r'(?!(?:else|return|case|default|do|goto|sizeof|static_assert)\b)'  # 不是控制关键字开头
    r'(?:[\w]+\s+)+'                         # ≥1 个返回类型/修饰符 token
    r'[\s\*]*'                               # 指针星号（可紧贴名字）
    r'([A-Za-z_]\w*)\s*\('                   # 函数名 + 左括号
)

def strip_code(text):
    """去掉注释与字符串，保留换行与字符位置（用空格填充）"""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i+1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        elif c == '/' and i + 1 < n and text[i+1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c == '"' or c == "'":
            q = c; j = i + 1
            while j < n and text[j] != q:
                if text[j] == '\\': j += 1
                j += 1
            j = min(j + 1, n)
            out.append(re.sub(r'[^\n]', ' ', text[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)

def check(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    code = strip_code(text)
    lines = code.split('\n')
    # 宏名不算函数（`#define WD(n) ...` 那种会被当成「先调用后声明」）
    macros = set(re.findall(r'^\s*#\s*define\s+([A-Za-z_]\w*)', text, re.M))
    # ① 行首声明：名字 -> 最早声明行
    declared = {}
    for ln, line in enumerate(lines, 1):
        m = DECL_RE.match(line)
        if m:
            name = m.group(1)
            if name in macros:
                continue
            if name in ('if', 'for', 'while', 'switch', 'return', 'sizeof', 'else',
                        'do', 'case', 'goto', 'defined', 'typeof'):
                continue
            declared.setdefault(name, ln)
    # ② 全部出现位置
    hits = {}
    for ln, line in enumerate(lines, 1):
        for m in re.finditer(r'\b([A-Za-z_]\w*)\s*\(', line):
            hits.setdefault(m.group(1), []).append(ln)
    # ③ 比对
    bad = []
    for name, first_decl in declared.items():
        for ln in hits.get(name, []):
            if ln < first_decl:
                # 排除：同一行的宏/typedef 之类，只报「明显在表达式里调用」的
                bad.append((name, ln, first_decl))
                break
    return bad, declared


# 文件作用域的 static 变量：C 里变量**没有前向声明**，用在声明之前就是硬错误。
# （第 47 轮真踩过：g_heartbeat 声明写在了 frame_tick 后面 —— 函数检查抓不到这一类。）
STATIC_VAR_RE = re.compile(r'^static\s+[\w\s\*]*?\b([A-Za-z_]\w*)\s*(?:=|\[|;)')

def check_static_vars(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    code = strip_code(text)
    lines = code.split('\n')
    decl = {}
    for ln, line in enumerate(lines, 1):
        if not line.startswith('static'):
            continue                      # 只认**顶格**写的（文件作用域）
        m = STATIC_VAR_RE.match(line)
        if m:
            decl.setdefault(m.group(1), ln)
    bad = []
    for name, dln in decl.items():
        for ln, line in enumerate(lines, 1):
            if ln >= dln:
                break
            if re.search(r'\b%s\b' % re.escape(name), line):
                bad.append((name, ln, dln))
                break
    return bad, decl

def main():
    files = []
    for t in TARGETS:
        p = os.path.join(ROOT, t)
        if os.path.exists(p): files.append(p)
    hd = os.path.join(ROOT, HOST)
    if os.path.isdir(hd):
        for f in sorted(os.listdir(hd)):
            if f.endswith(('.c', '.m')) and f not in SKIP:
                files.append(os.path.join(hd, f))
    td = os.path.join(hd, 'tests')
    if os.path.isdir(td):
        for f in sorted(os.listdir(td)):
            if f.endswith('.c'):
                files.append(os.path.join(td, f))

    print('=' * 78)
    print('检查「先调用、后声明」（clang 16 起为硬错误）—— 共 %d 个文件' % len(files))
    print('=' * 78)
    total = 0
    for p in files:
        bad, declared = check(p)
        rel = os.path.relpath(p, os.path.dirname(ROOT)).replace('\\', '/')
        if bad:
            total += len(bad)
            print('\n!! %s  —— %d 处' % (rel, len(bad)))
            for name, call_ln, decl_ln in bad:
                print('     第 %4d 行调用 %-28s 但第 %4d 行才声明' % (call_ln, name, decl_ln))
        else:
            print('   OK  %-56s (%d 个本文件内函数)' % (rel, len(declared)))

    print()
    print('=' * 78)
    print('检查「文件作用域 static 变量：用在声明之前」—— 变量**没有**前向声明')
    print('=' * 78)
    vtotal = 0
    for p in files:
        bad, decl = check_static_vars(p)
        rel = os.path.relpath(p, os.path.dirname(ROOT)).replace('\\', '/')
        if bad:
            vtotal += len(bad)
            print('\n!! %s  —— %d 处' % (rel, len(bad)))
            for name, use_ln, decl_ln in bad:
                print('     第 %4d 行用了 %-28s 但第 %4d 行才声明' % (use_ln, name, decl_ln))
        else:
            print('   OK  %-56s (%d 个顶格 static 变量)' % (rel, len(decl)))
    total += vtotal

    # 括号配平（第 50 轮加的：我抽公共函数时漏掉一个 `}`，
    # 结果 17 个自测全部编译失败 —— 而且报错指向后面每个函数，很难一眼看出真因）
    print()
    print('=' * 78)
    print('检查：花括号/圆括号配平（用**去掉注释与字符串**后的文本数）')
    print('=' * 78)
    btotal = 0
    for p in files:
        code = strip_code(open(p, encoding='utf-8', errors='replace').read())
        cb, ob = code.count('{') - code.count('}'), code.count('(') - code.count(')')
        rel = os.path.relpath(p, os.path.dirname(ROOT)).replace('\\', '/')
        if cb or ob:
            btotal += 1
            print('   !! %-56s {} 差 %+d  () 差 %+d' % (rel, cb, ob))
    if not btotal:
        print('   OK  所有文件配平')
    total += btotal

    print()
    print('=' * 78)
    if total:
        print('结果：**%d 处需要修** —— 在 Mac 上会编译失败' % total)
    else:
        print('结果：全部通过（没有「先调用后声明」，也没有「变量先用后声明」）')
    print('=' * 78)
    return 1 if total else 0

if __name__ == '__main__':
    sys.exit(main())
