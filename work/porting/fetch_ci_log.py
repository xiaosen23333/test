# 把 CI 的 build-log 取下来，只打印有用的部分（编译错误）。
#
# 为什么需要：Mac 上才有的错误（ObjC / clang 特有）在本机完全看不到。
# 以前是「去 Actions 页面手动下 artifact」，现在一条命令。
import json, os, re, subprocess, sys, urllib.request, urllib.error, zipfile, io
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

REPO = 'xiaosen23333/test'
API  = 'https://api.github.com'
UA   = {'User-Agent': 'dm-ci-log', 'Accept': 'application/vnd.github+json'}

def token():
    p = subprocess.run(['git', 'credential', 'fill'],
                       input='protocol=https\nhost=github.com\n\n',
                       capture_output=True, text=True, encoding='utf-8', errors='replace',
                       env=dict(os.environ, GIT_TERMINAL_PROMPT='0', GCM_INTERACTIVE='never'),
                       cwd=r'D:\d2r-mobile\work\d2r-mobile')
    for line in (p.stdout or '').splitlines():
        if line.startswith('password='):
            return line.split('=', 1)[1].strip()
    return None

class DropAuth(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        new = super().redirect_request(req, fp, code, msg, headers, newurl)
        if new:
            for k in ('Authorization', 'authorization'):
                new.headers.pop(k, None); new.unredirected_hdrs.pop(k, None)
        return new

OPENER = urllib.request.build_opener(DropAuth)

def api(path, tok):
    h = dict(UA); h['Authorization'] = 'Bearer ' + tok
    req = urllib.request.Request(API + path, headers=h)
    with urllib.request.urlopen(req, timeout=60) as r:
        return json.load(r)

def main():
    tok = token()
    if not tok:
        print('取不到凭据'); return 2
    want = sys.argv[1] if len(sys.argv) > 1 else None
    runs = api('/repos/%s/actions/runs?per_page=10' % REPO, tok)['workflow_runs']
    runs = [r for r in runs if r['name'] == 'Build iOS shell']
    if want:
        runs = [r for r in runs if r['head_sha'].startswith(want)]
    run = runs[0]
    print('run #%s  %s  %s  %s' % (run['run_number'], run['status'], run['conclusion'], run['head_sha'][:8]))
    print('  %s' % run['html_url'])
    arts = api('/repos/%s/actions/runs/%s/artifacts' % (REPO, run['id']), tok)['artifacts']
    pick = None
    for a in arts:
        if a['name'] == 'build-log':
            pick = a
    if not pick:
        print('  没有 build-log 产物'); return 3
    h = dict(UA); h['Authorization'] = 'Bearer ' + tok
    req = urllib.request.Request(pick['archive_download_url'], headers=h)
    with OPENER.open(req, timeout=180) as r:
        blob = r.read()
    z = zipfile.ZipFile(io.BytesIO(blob))
    name = [n for n in z.namelist() if n.endswith('build.log')][0]
    text = z.read(name).decode('utf-8', 'replace')
    print('\n=== 日志大小 %d 字节，共 %d 行 ===\n' % (len(text), text.count('\n')))

    errs = [l for l in text.splitlines() if re.search(r'error:|fatal error|error LNK', l)]
    print('=== 报错 %d 行 ===' % len(errs))
    for l in errs[:40]:
        print('  ' + l.strip()[:220])
    print()
    print('=== 静态闸门那一段 ===')
    for i, l in enumerate(text.splitlines()):
        if '先调用后声明' in l or '静态闸门' in l:
            for x in text.splitlines()[i:i + 8]:
                print('  ' + x[:200])
            break
    print()
    print('=== 结果性的一行 ===')
    for l in text.splitlines():
        if re.search(r'BUILD (SUCCEEDED|FAILED)|语法体检合计', l):
            print('  ' + l.strip()[:200])
    return 0

if __name__ == '__main__':
    sys.exit(main())
