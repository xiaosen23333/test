<#
  D2R Mobile 移植工作区 —— Windows 工具链环境引导。

  用法（在 D:\d2r-mobile 下）：
      . .\work\porting\dm_env.ps1          # 点源进当前会话
      dm-env-check                          # 看清单

  做的事：
    1. 把本次装好的工具加进 PATH（cmake / ninja / spirv-cross / nstool / 编译器垫片）
    2. 若 LLVM 已就绪则一并加入；否则用 Visual Studio 的 MSVC
    3. 注入 MSVC 的开发者环境（INCLUDE / LIB / PATH），让 cl.exe 可用
    4. 设置仓库工具认的环境变量（DM_ROOT 等）

  注：本机原本的 gcc 是残缺的 MinGW（缺 C 运行库头，连 stdio.h 都没有），
  所以这里优先用 Visual Studio 2022 的 MSVC，并通过 cc.cmd / gcc.cmd 垫片
  满足 d2rm_pack.py 的「C 编译器」检查。
#>

$ErrorActionPreference = 'Continue'

$script:RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$script:ToolsBin = Join-Path $RepoRoot 'work\tools\bin'

function Add-PathEntry([string]$p) {
    if ($p -and (Test-Path $p) -and ($env:PATH -notlike "*$p*")) {
        $env:PATH = "$p;$env:PATH"
    }
}

# --- 1. 本次装好的工具 ---
Add-PathEntry $script:ToolsBin
Add-PathEntry (Join-Path $RepoRoot 'work\tools\cmake\bin')
Add-PathEntry (Join-Path $RepoRoot 'work\tools\ninja')
Add-PathEntry (Join-Path $RepoRoot 'work\tools\nstool')
Add-PathEntry (Join-Path $RepoRoot 'work\tools\llvm\bin')      # 若装好了就用

# --- 2. .NET 10（着色器翻译链要求 net10.0）---
$dotnet10 = Join-Path $RepoRoot 'work\tools\dotnet10'
if (Test-Path (Join-Path $dotnet10 'dotnet.exe')) {
    $env:DOTNET_ROOT = $dotnet10
    $env:DOTNET_MULTILEVEL_LOOKUP = '0'
    Add-PathEntry $dotnet10
}

# --- 3. 注入 MSVC 开发者环境（vcvars64.bat）---
function Import-MsvcEnv {
    $vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { Write-Host '  [跳过] 没有 vswhere，找不到 Visual Studio' -ForegroundColor Yellow; return $false }
    $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    if (-not $vsPath) { Write-Host '  [跳过] Visual Studio 没装 C++ 工具集' -ForegroundColor Yellow; return $false }
    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { Write-Host "  [跳过] 没有 $vcvars" -ForegroundColor Yellow; return $false }

    # 跑一次 vcvars64.bat，把它的环境变量捞回当前会话
    $out = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    foreach ($line in $out) {
        if ($line -match '^([^=]+)=(.*)$') {
            $name = $Matches[1]; $val = $Matches[2]
            # 不覆盖 PowerShell 自己的几个变量
            if ($name -notin @('PROMPT')) { Set-Item -Path "env:$name" -Value $val -ErrorAction SilentlyContinue }
        }
    }
    return $true
}

$msvcOk = Import-MsvcEnv
if ($msvcOk) { Write-Host "  [OK] MSVC 开发者环境已注入" -ForegroundColor Green }

# --- 4. 仓库工具认的环境变量 ---
if (-not $env:DM_ROOT) { $env:DM_ROOT = Join-Path $RepoRoot 'work\dm-work' }
# 中文 Windows 控制台默认 GBK，Python 打中文会乱码或抛编码异常
$env:PYTHONIOENCODING = 'utf-8'

function dm-env-check {
    Write-Host ''
    Write-Host '=== D2R Mobile 移植工具链清单 ===' -ForegroundColor Cyan
    $items = [ordered]@{
        'python'      = 'python'
        'dotnet'      = 'dotnet'
        'cl (MSVC)'   = 'cl'
        'cc (垫片)'    = 'cc'
        'gcc (垫片)'   = 'gcc'
        'clang'       = 'clang'
        'cmake'       = 'cmake'
        'ninja'       = 'ninja'
        'spirv-cross' = 'spirv-cross'
        'nstool'      = 'nstool'
        'git'         = 'git'
    }
    foreach ($k in $items.Keys) {
        $c = Get-Command $items[$k] -ErrorAction SilentlyContinue
        if ($c) { Write-Host ("  {0,-14} OK  {1}" -f $k, $c.Source) -ForegroundColor Green }
        else    { Write-Host ("  {0,-14} --" -f $k) -ForegroundColor DarkGray }
    }
    Write-Host ''
    Write-Host ("  DM_ROOT      = " + $env:DM_ROOT)
    Write-Host ("  DOTNET_ROOT  = " + $(if ($env:DOTNET_ROOT) { $env:DOTNET_ROOT } else { '(用系统 dotnet)' }))
    Write-Host ''
    Write-Host '  注：gcc 垫片指向 MSVC。本机原生 MinGW 残缺（无 C 运行库头），不要直接用。' -ForegroundColor DarkYellow
    Write-Host ''
}

Write-Host "工具链环境就绪。跑 dm-env-check 看清单。" -ForegroundColor Cyan
