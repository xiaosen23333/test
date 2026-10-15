# 一键全量构建与回归 —— 把宿主全部源码 + 全部自测编译并跑一遍。
#
# 用途：
#   · 我改任何代码后，一条命令确认没有回归
#   · 交给别人/未来的自己时，一条命令确认「这套东西是活的」
#
# 用法（在仓库根，先 . .\work\porting\dm_env.ps1）：
#   pwsh -File work\tools\build_all.ps1
#
# 退出码：0 = 全部通过；非 0 = 有失败（末尾会列出）
$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$H    = Join-Path $root 'work\host'
$B    = Join-Path $H 'build'
$OUT  = Join-Path $root 'work\patcher\out'
$SH   = Join-Path $root 'work\shaders2'
$GAME = Join-Path $root 'work\extract\game'
$DAT  = Join-Path $GAME 'data\shader_1_00.dat'

New-Item -ItemType Directory -Force -Path $B | Out-Null
Push-Location $B

# ---- 宿主源码（顺序无关，一次编译）----
$SRC = @(
  'hle.c','loader.c','hle_host_dir.c','hle_diag.c',
  'hle_nn.c','hle_nn_fs.c','hle_nn_fs2.c','hle_nn_thread.c','hle_nn_util.c',
  'hle_nn_more.c','hle_nn_audio.c','nn_impl_table.c',
  'nvn_names.c','hle_nvn.c','hle_nvn_obj.c','hle_nvn_cmd.c','hle_nvn_replay.c',
  'hle_nvn_metal.c','hle_nvn_fmt.c','hle_nvn_glslc.c','hle_nvn_mtl.c','hle_nvn_boot.c'
) | ForEach-Object { Join-Path $H $_ }

# ---- 自测清单：名字 -> 运行参数 ----
# 只列「应该有结论」的；诊断类（diag_map 等）不算回归。
$TESTS = @(
  @{ n='boot_selftest';      a=@($OUT, $GAME) }
  @{ n='loader_selftest';    a=@($OUT) }
  @{ n='boot_syms_selftest'; a=@($OUT) }
  @{ n='host_selftest';      a=@("$OUT\main.gates.tsv") }
  @{ n='nn_selftest';        a=@($GAME) }
  @{ n='entry_selftest';     a=@($OUT) }
  @{ n='tlsdesc_selftest';   a=@($OUT) }
  @{ n='libc_init2_selftest';         a=@($OUT) }
  @{ n='nvn_selftest';       a=@() }
  @{ n='nvn_obj_selftest';   a=@() }
  @{ n='nvn_metal_selftest'; a=@() }
  @{ n='nvn_shader_selftest';a=@("$SH\shader_map.tsv", "$SH\metal", $DAT) }
  @{ n='glslc_selftest';     a=@("$SH\shader_map.tsv", "$SH\metal", $DAT) }
  @{ n='glslc_invariant_selftest';          a=@() }
  @{ n='replay_selftest';    a=@() }
  @{ n='mtl_selftest';       a=@() }
  @{ n='fmt_selftest';       a=@() }
)

$env:DM_SHADER_DIR = $SH
$env:DM_MODULE_DIR = $OUT

$built = 0; $passed = 0; $failed = @()
foreach ($t in $TESTS) {
    $name = $t.n
    $cs   = Join-Path $H "tests\$name.c"
    if (-not (Test-Path $cs)) {
        Write-Host ("  {0,-24} 源文件缺失" -f $name) -ForegroundColor Yellow
        $failed += $name; continue
    }
    $log = Join-Path $B "$name.build.log"
    $clArgs = @('/nologo','/W3','/utf-8','/D_CRT_SECURE_NO_WARNINGS') + $SRC + @($cs, "/Fe:$name.exe")
    & cl @clArgs *> $log
    if (-not (Test-Path (Join-Path $B "$name.exe"))) {
        Write-Host ("  {0,-24} 编译失败（见 {1}）" -f $name, $log) -ForegroundColor Red
        $failed += $name; continue
    }
    $built++
    $rlog = Join-Path $B "$name.run.log"
    & (Join-Path $B "$name.exe") @($t.a) *> $rlog
    $rc = $LASTEXITCODE
    $line = Select-String -Path $rlog -Pattern 'result:' | Select-Object -Last 1
    $verdict = if ($line) { ($line.Line -replace '.*result:\s*','').Trim() } else { "(无结论行)" }
    $ok = ($rc -eq 0) -and ($verdict -match 'ALL PASS|PASS')
    if ($ok) {
        $passed++
        Write-Host ("  {0,-24} {1}" -f $name, $verdict) -ForegroundColor Green
    } else {
        $failed += $name
        Write-Host ("  {0,-24} rc={1} {2}" -f $name, $rc, $verdict) -ForegroundColor Red
    }
}

# ---- 改写器产物自检 ----
Write-Host ""
Write-Host "=== 产物清单核对 ==="
$need = @('main','sdk','rtld','NmplDLL.nro','lz4.nro') | ForEach-Object { "$_.patched.bin" }
$miss = @()
foreach ($f in $need) { if (-not (Test-Path (Join-Path $OUT $f))) { $miss += $f } }
if ($miss.Count) { Write-Host ("  缺改写产物: {0}" -f ($miss -join ', ')) -ForegroundColor Red; $failed += 'artifacts' }
else { Write-Host "  5 个改写模块齐全" -ForegroundColor Green }
$sm = Join-Path $SH 'shader_map.tsv'
if (Test-Path $sm) { Write-Host ("  着色器映射表: {0} 行" -f ((Get-Content $sm | Where-Object { $_ -notmatch '^#' }).Count)) -ForegroundColor Green }
else { Write-Host "  缺着色器映射表" -ForegroundColor Red; $failed += 'shader_map' }

Pop-Location
Write-Host ""
if ($failed.Count) {
    Write-Host ("=== 失败 {0} 项: {1} ===" -f $failed.Count, ($failed -join ', ')) -ForegroundColor Red
    exit 1
}
Write-Host ("=== 全部通过（编译 {0} 个，通过 {1} 个）===" -f $built, $passed) -ForegroundColor Green
exit 0
