<#
    Cross-compiles the hook library with the NDK clang:
      - azurlul.so : the inline-hook library (azurlul.c + amalgamated Zydis)

    Run from the project folder:  .\build.ps1

    Examples:
        .\build.ps1                       # x86_64, API 24 (default)
        .\build.ps1 -Abi arm64-v8a        # ARM device/emulator
        .\build.ps1 -Api 21               # target an older API level
        .\build.ps1 -NdkPath D:\android-ndk
#>
[CmdletBinding()]
param(
    [ValidateSet('x86_64', 'x86', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'x86_64',
    [int]$Api = 24,
    [string]$NdkPath
)

$ErrorActionPreference = 'Stop'
Set-Location -Path $PSScriptRoot

# --- Locate the NDK ---------------------------------------------------------
if (-not $NdkPath) {
    $candidates = @(
        $env:ANDROID_NDK_HOME,
        $env:ANDROID_NDK_ROOT,
        'C:\android-ndk'
    )
    $sdkNdk = Join-Path "$env:LOCALAPPDATA\Android\Sdk" 'ndk'
    if (Test-Path $sdkNdk) {
        $candidates += (Get-ChildItem $sdkNdk -Directory |
            Sort-Object Name -Descending | Select-Object -First 1).FullName
    }
    $NdkPath = $candidates | Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
}
if (-not $NdkPath -or -not (Test-Path $NdkPath)) {
    throw "Android NDK not found. Pass -NdkPath 'C:\path\to\ndk'."
}

$clang = Join-Path $NdkPath 'toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe'
if (-not (Test-Path $clang)) {
    throw "clang.exe not found under NDK: $clang"
}

# --- ABI -> clang target triple --------------------------------------------
$triple = switch ($Abi) {
    'x86_64'      { "x86_64-linux-android$Api" }
    'x86'         { "i686-linux-android$Api" }
    'arm64-v8a'   { "aarch64-linux-android$Api" }
    'armeabi-v7a' { "armv7a-linux-androideabi$Api" }
}

Write-Host "NDK    : $NdkPath"
Write-Host "clang  : $clang"
Write-Host "target : $triple"
Write-Host ""

function Invoke-Clang([string[]]$clangArgs, [string]$out) {
    Write-Host "clang $($clangArgs -join ' ')" -ForegroundColor DarkGray
    & $clang @clangArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed (clang exit $LASTEXITCODE)." }
    $size = [math]::Round((Get-Item $out).Length / 1KB, 1)
    Write-Host "OK -> $out ($size KB)" -ForegroundColor Green
    Write-Host ""
}

# --- Hook (.so) -------------------------------------------------------------
Invoke-Clang @(
    "--target=$triple",
    '-shared', '-fPIC', '-O2',
    'azurlul.c', 'Zydis.c',
    '-I.',
    '-o', 'azurlul.so',
    '-ldl'
) 'azurlul.so'
