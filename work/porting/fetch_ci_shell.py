# 从 GitHub Actions 把 CI 编好的「壳」下载到本机。
#
# 为什么要有它：
#   第 43 轮前的路子是「把 dm-mac-build-kit.zip 发给朋友 → 他用 Mac 编 → 传回来」，
#   一次来回至少一天。而这个仓库本来就配了 GitHub Actions（.github/workflows/ios-shell.yml），
#   push 到 main 就会自动出一份壳 —— **只是产物要登录才能下载**（匿名列得出来，
#   下的时候 401）。本脚本用本机 git 里已经存好的凭据（就是 push 用的那个）把产物取回来。
#
# 安全说明：
#   · 凭据只在内存里用一次（git credential fill），不打印、不落盘、不写进任何文件
#   · 只用在你自己的仓库、你自己的构建产物上
#   · 不信这个做法就用 `--no-token` 跳过，改从 Actions 页面手动下载
import json, os, sys, subprocess, urllib.request, urllib.error, zipfile, shutil, hashlib
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

REPO   = 'xiaosen23333/test'
API    = 'https://api.github.com'
WORK   = r'D:\d2r-mobile\work'
OUTDIR = os.path.join(WORK, 'ipastage', 'ci')
SHELL  = os.path.join(WORK, 'DmGame-shell.ipa')
UA     = {'User-Agent': 'dm-port-fetch', 'Accept': 'application/vnd.github+json'}

def token():
    """从本机 git 凭据里取 token（非交互；取不到返回 None）"""
    p = subprocess.run(['git', 'credential', 'fill'],
                       input='protocol=https\nhost=github.com\n\n',
                       capture_output=True, text=True, encoding='utf-8', errors='replace',
                       env=dict(os.environ, GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='never'),
                       cwd=os.path.join(WORK, 'd2r-mobile'))
    if p.returncode != 0:
        return None
    for line in (p.stdout or '').splitlines():
        if line.startswith('password='):
            return line.split('=', 1)[1].strip()
    return None

def api(path, tok=None):
    h = dict(UA)
    if tok:
        h['Authorization'] = 'Bearer ' + tok
    req = urllib.request.Request(API + path, headers=h)
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)

class _DropAuthOnRedirect(urllib.request.HTTPRedirectHandler):
    """产物下载会 302 到 Azure Blob（blob.core.windows.net）上的**预签名**地址。
       那个地址自带签名，**不能**再带我们的 Authorization ——
       带了 Azure 会回 401「Server failed to authenticate the request」。
       （我第一次就是这么失败的：urllib 默认会把 Authorization 原样转给重定向目标。）"""
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new is not None:
            for k in ('Authorization', 'authorization'):
                new.headers.pop(k, None)
                new.unredirected_hdrs.pop(k, None)
        return new

_OPENER = urllib.request.build_opener(_DropAuthOnRedirect)

def want_run(sha=None, tok=None):
    """找要下载的 run：给了 sha 就找那个 commit 的，否则取 main 上最新一次"""
    if sha:
        d = api('/repos/%s/actions/runs?head_sha=%s&per_page=10' % (REPO, sha), tok)
        runs = [r for r in d['workflow_runs'] if r['name'] == 'Build iOS shell']
        if runs:
            return max(runs, key=lambda r: r['run_number'])
    d = api('/repos/%s/actions/runs?branch=main&per_page=20' % REPO, tok)
    runs = [r for r in d['workflow_runs'] if r['name'] == 'Build iOS shell']
    if not runs:
        return None
    return max(runs, key=lambda r: r['run_number'])

def main():
    sha = sys.argv[1] if len(sys.argv) > 1 and not sys.argv[1].startswith('-') else None
    # 早点取 token：匿名调 API 每小时只有 60 次，连着跑几轮就会被 403 rate limit 挡住
    # （本轮真被挡过一次）。带上凭据就是 5000 次/小时。
    tok = token()
    run = want_run(sha, tok)
    if not run:
        print('没找到任何 "Build iOS shell" 的运行'); return 2
    print('=' * 74)
    print('取 CI 产物')
    print('=' * 74)
    print('  run #%s  %s  %s' % (run['run_number'], run['status'], run['conclusion']))
    print('  commit %s  %s' % (run['head_sha'][:8], run['head_commit']['message'].splitlines()[0][:60]))
    print('  %s' % run['html_url'])
    if run['status'] != 'completed':
        print('  还在跑（%s）—— 等它跑完再来' % run['status']); return 3
    if run['conclusion'] != 'success':
        print('  这次**没成功**（%s）—— 先看日志' % run['conclusion']); return 4

    arts = api('/repos/%s/actions/runs/%s/artifacts' % (REPO, run['id']), tok)['artifacts']
    pick = None
    for a in arts:
        if a['name'] == 'DmGame-shell-ipa' and not a['expired']:
            pick = a
    if not pick:
        print('  没找到 DmGame-shell-ipa 产物'); return 5
    print('  产物 %s（%d 字节）' % (pick['name'], pick['size_in_bytes']))

    if not tok:
        print('  !! 取不到本机 git 凭据 —— 请从上面的 Actions 页面手动下载产物')
        return 6
    print('  用本机 git 凭据下载（不打印、不落盘）…')

    os.makedirs(OUTDIR, exist_ok=True)
    zpath = os.path.join(OUTDIR, 'DmGame-shell-ipa.zip')
    h = dict(UA); h['Authorization'] = 'Bearer ' + tok
    req = urllib.request.Request(pick['archive_download_url'], headers=h)
    try:
        with _OPENER.open(req, timeout=180) as r, open(zpath, 'wb') as f:
            shutil.copyfileobj(r, f)
    except urllib.error.HTTPError as e:
        print('  下载失败: HTTP %s %s' % (e.code, e.reason)); return 7
    print('  下载完成: %s（%d 字节）' % (zpath, os.path.getsize(zpath)))

    with zipfile.ZipFile(zpath) as z:
        names = z.namelist()
        ipas = [n for n in names if n.endswith('.ipa')]
        print('  压缩包内: %s' % ', '.join(names))
        if not ipas:
            print('  !! 包里没有 .ipa'); return 8
        tmp = os.path.join(OUTDIR, '_new.ipa')
        with z.open(ipas[0]) as src, open(tmp, 'wb') as dst:
            shutil.copyfileobj(src, dst)

    # 备份旧的壳（不要覆盖掉，出问题时能回退）
    if os.path.exists(SHELL):
        bak = SHELL[:-4] + '.prev.ipa'
        shutil.copy2(SHELL, bak)
        print('  旧壳已备份 -> %s' % os.path.basename(bak))
    shutil.move(tmp, SHELL)
    print('  新壳已就位 -> %s（%d 字节）' % (SHELL, os.path.getsize(SHELL)))

    # ---- 关键核对：这壳到底是不是用新代码编的 ----
    print()
    print('=' * 74)
    print('核对：这壳是新代码编的吗（第 43/44 轮的修复在不在里面）')
    print('=' * 74)
    with zipfile.ZipFile(SHELL) as z:
        exe = z.read('Payload/DmGame.app/DmGame')
    print('  可执行文件 %d 字节' % len(exe))
    MARKS = [
        (b'DmFrameTick',            '第 43 轮：真正驱动帧循环的那个类'),
        (b'dm_ios_boot_async',      '第 43 轮：后台装载'),
        (b'dm_ios_make_window',     '第 43 轮：建窗口'),
        (b'dm_diag_set_log',        '第 45 轮：诊断通道（日志文件）'),
        (b'dm_host_diag',           '第 45 轮：诊断总览（客体走到哪 + NVN 计数）'),
        (b'dm_ios_res_dir',         '第 41 轮：iOS 上给绝对资源路径'),
        (b'dm_host_res_dir',        '第 41 轮：宿主侧资源目录'),
    ]
    bad = 0
    for pat, why in MARKS:
        n = exe.count(pat)
        ok = n > 0
        if not ok:
            bad += 1
        print('  %-24s %-4s %s' % (pat.decode(), '在' if ok else '**不在**', why))
    bigger = len(exe) > 144808
    print('  %-24s %-4s %s' % ('大小 > 144,808 字节', '是' if bigger else '**否**',
                               '上一版就是 144,808 —— 等于它说明没重编'))
    if bad:
        print()
        print('  !! 有 %d 个标记不在 —— 这壳很可能还是旧代码，先别装机' % bad)
        return 9
    print()
    print('  ==> 判定：这是**新代码**编出来的壳，可以进下一步（塞游戏内容）')
    print('      下一步：python work\\tools\\dmpack.py --shell work\\DmGame-shell.ipa '
          '--out work\\DmGame.ipa --data-mode min')
    print('=' * 74)
    return 0

if __name__ == '__main__':
    sys.exit(main())
