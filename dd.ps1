# platform-h developer commands.
#
# There is no application here, so 'run' is not offered; 'test' builds and runs
# the unit suite.
param(
    [Parameter(Position = 0)]
    [ValidateSet('test', 'build', 'clean')]
    [string] $Command = 'test',

    [ValidateSet('Debug', 'Release')]
    [string] $Config = 'Release',

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $Rest
)

$ErrorActionPreference = 'Stop'

function Get-VisualStudioPath {
    $vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw 'vswhere.exe was not found; install Visual Studio with the C++ desktop workload.'
    }

    $path = & $vswhere -latest -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath |
        Select-Object -First 1

    if (-not $path) {
        throw 'No Visual Studio installation with the C++ desktop workload was found.'
    }

    return $path
}

# cl and rc only work inside the MSVC environment, and Ninja invokes cl directly.
function Enter-MsvcEnvironment {
    param([string] $VisualStudio)

    if ($env:VSCMD_ARG_TGT_ARCH -eq 'x64') {
        return
    }

    $vcvars = Join-Path $VisualStudio 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) {
        throw "vcvars64.bat was not found at $vcvars."
    }

    & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') {
            Set-Item -LiteralPath "Env:$($Matches[1])" -Value $Matches[2]
        }
    }
}

# Prefer whatever is on PATH, then the copy Visual Studio ships, so neither tool
# has to be installed separately.
function Resolve-Tool {
    param([string] $Name, [string] $VisualStudio, [string] $BundledRelativePath)

    $onPath = Get-Command $Name -ErrorAction SilentlyContinue
    if ($onPath) {
        return $onPath.Source
    }

    $bundled = Join-Path $VisualStudio $BundledRelativePath
    if (Test-Path $bundled) {
        return $bundled
    }

    throw "$Name was not found on PATH or under $VisualStudio."
}

function Get-BuildDir {
    return Join-Path $PSScriptRoot "build\$($Config.ToLowerInvariant())"
}

function Invoke-Build {
    $vs = Get-VisualStudioPath
    Enter-MsvcEnvironment -VisualStudio $vs

    $cmake = Resolve-Tool -Name 'cmake' -VisualStudio $vs `
        -BundledRelativePath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    $ninja = Resolve-Tool -Name 'ninja' -VisualStudio $vs `
        -BundledRelativePath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'

    $env:PATH = "$(Split-Path $ninja);$env:PATH"

    $preset = $Config.ToLowerInvariant()

    Push-Location $PSScriptRoot
    try {
        & $cmake --preset $preset
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

        & $cmake --build --preset $preset
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
    finally {
        Pop-Location
    }
}

switch ($Command) {
    'build' { Invoke-Build }

    'test' {
        Invoke-Build
        & (Join-Path (Get-BuildDir) 'platform_tests.exe') @Rest
        exit $LASTEXITCODE
    }

    'clean' {
        $full = Join-Path $PSScriptRoot 'build'
        if (Test-Path $full) {
            Remove-Item -LiteralPath $full -Recurse -Force
        }
    }
}
