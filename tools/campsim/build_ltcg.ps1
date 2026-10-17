# build_ltcg.ps1 -- build campsim with link-time code generation (~12% faster, same results) in a
# worktree of its own, so the game build's libraries are never recompiled with /GL.
#
#   powershell -File tools\campsim\build_ltcg.ps1            # build this checkout's HEAD
#   powershell -File tools\campsim\build_ltcg.ps1 -Ref campsim-gpu -Rebuild
#
# Then point the batch scripts at it (run_batch.py, supply_spread.py read CAMPSIM_EXE_DIR):
#   $env:CAMPSIM_EXE_DIR = '<worktree>\src\Falcon4___x64_Release\Tools\campsim'
#
# The worktree is detached at the commit built; it must have no local edits (the script refuses).
# Use -Rebuild after a header gains a member (stale objects against a changed layout corrupt memory).

param(
    [string]$Ref = 'HEAD',
    [string]$Worktree = '',
    [string]$Game = 'C:\FreeFalcon6',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$repo = (git -C $PSScriptRoot rev-parse --show-toplevel).Trim()

if (-not $Worktree) {
    $Worktree = Join-Path (Split-Path $repo -Parent) 'ff-campsim-ltcg'
}

$commit = (git -C $repo rev-parse --verify "$Ref^{commit}").Trim()

if (-not (Test-Path (Join-Path $Worktree '.git'))) {
    git -C $repo worktree add --detach $Worktree $commit
    if ($LASTEXITCODE) { throw "git worktree add failed" }
}
else {
    if (git -C $Worktree status --porcelain --untracked-files=no) {
        throw "$Worktree has local edits; commit or discard them first"
    }

    git -C $Worktree checkout --quiet --detach $commit
    if ($LASTEXITCODE) { throw "git checkout $commit failed in $Worktree" }
}

# NuGet packages (OpenXR loader) are untracked: share the main checkout's
$pkg = Join-Path $Worktree 'packages'
if (-not (Test-Path $pkg)) {
    New-Item -ItemType Junction -Path $pkg -Target (Join-Path $repo 'packages') | Out-Null
}

$props = Join-Path $Worktree 'tools\campsim\ltcg.props'
if (-not (Test-Path $props)) { throw "$commit predates tools\campsim\ltcg.props" }

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found" }

$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
Write-Host "campsim LTCG: $target $commit in $Worktree"
& $msbuild (Join-Path $Worktree 'src\tools\campsim\campsim.vcxproj') "/t:$target" /p:Configuration=Release `
    /p:Platform=x64 /m /v:minimal /nr:false "/p:ForceImportAfterCppTargets=$props" /p:FFLinkMode=LTCG
if ($LASTEXITCODE) { throw "MSBuild failed ($LASTEXITCODE)" }

# runtime DLLs the build does not produce; the game install has them
$out = Join-Path $Worktree 'src\Falcon4___x64_Release\Tools\campsim'
foreach ($dll in 'OpenAL32.dll', 'dxcompiler.dll', 'dxil.dll', 'nvtt30205.dll') {
    Copy-Item (Join-Path $Game $dll) $out
}

Write-Host "built $(Join-Path $out 'campsim.exe')"
Write-Host "`$env:CAMPSIM_EXE_DIR = '$out'"
