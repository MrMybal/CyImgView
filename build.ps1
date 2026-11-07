# Compile CyImgView avec Visual Studio (MSVC + CMake + Ninja fournis par VS).
#   .\build.ps1            -> Release
#   .\build.ps1 -Config Debug
#   .\build.ps1 -ShellTests -> construit aussi les tests isolés des associations Windows
#   .\build.ps1 -MetadataTests -> construit les tests du moteur et des fournisseurs de métadonnées
#   .\build.ps1 -Clean     -> recompile tout (obligatoire avant de livrer : un objet resté d'une
#                              ancienne version de viewer.h donne une classe incohérente entre fichiers)
param([ValidateSet('Release', 'Debug')][string]$Config = 'Release', [switch]$Clean, [switch]$ShellTests, [switch]$MetadataTests)
$ErrorActionPreference = 'Stop'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio avec les outils C++ est introuvable." }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$build = Join-Path $PSScriptRoot "build\$Config"
$env:PATH = "$(Split-Path $vswhere);$env:PATH"  # vcvars appelle vswhere
$ErrorActionPreference = 'Continue'

$cleanFirst = if ($Clean) { '--clean-first' } else { '' }
$shellTestOption = if ($ShellTests) { '-DCYIMG_SHELL_TESTS=ON' } else { '' }
$metadataTestOption = if ($MetadataTests) { '-DCYIMG_METADATA_TESTS=ON' } else { '' }
cmd /c "`"$vcvars`" >nul && cmake -S `"$PSScriptRoot`" -B `"$build`" -G Ninja -DCMAKE_BUILD_TYPE=$Config $shellTestOption $metadataTestOption && cmake --build `"$build`" $cleanFirst"
if ($LASTEXITCODE -ne 0) { throw "La compilation a échoué." }
Write-Host "OK -> $build\bin\CyImgView.exe"
