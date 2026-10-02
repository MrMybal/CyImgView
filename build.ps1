# Compile CyImgView avec Visual Studio (MSVC + CMake + Ninja fournis par VS).
#   .\build.ps1            -> Release
#   .\build.ps1 -Config Debug
param([ValidateSet('Release', 'Debug')][string]$Config = 'Release')
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio avec les outils C++ est introuvable." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$build = Join-Path $PSScriptRoot "build\$Config"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars appelle vswhere
$ErrorActionPreference = 'Continue'

cmd /c "`"$vcvars`" >nul && cmake -S `"$PSScriptRoot`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config && cmake --build `"$build`""
if ($LASTEXITCODE -ne 0) { throw "La compilation a échoué." }
Write-Host "OK -> $build\bin\CyImgView.exe"
