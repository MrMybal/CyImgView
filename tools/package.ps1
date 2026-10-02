# Construit l'archive de release : dist\CyImgView-<version>-win64.zip
#   .\tools\package.ps1            (compile en Release puis empaquette)
#   .\tools\package.ps1 -NoBuild   (empaquette le build existant)
# L'archive contient CyImgView.exe + plugins\ ; c'est elle qu'il faut joindre à la release GitHub
# (étiquette « v<version> ») pour que la mise à jour automatique la trouve.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$h = Get-Content (Join-Path $root 'src\version.h') -Raw
$ver = '{0}.{1}.{2}' -f ([regex]::Match($h, 'VERSION_MAJOR (\d+)').Groups[1].Value),
                        ([regex]::Match($h, 'VERSION_MINOR (\d+)').Groups[1].Value),
                        ([regex]::Match($h, 'VERSION_PATCH (\d+)').Groups[1].Value)

if (-not $NoBuild) {
    & cmd /c "`"$root\build.cmd`""
    if ($LASTEXITCODE -ne 0) { throw 'La compilation a échoué.' }
}

$bin = Join-Path $root 'build\Release\bin'
$stage = Join-Path $root "dist\CyImgView-$ver-win64"
$zip = "$stage.zip"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'plugins') | Out-Null

Copy-Item (Join-Path $bin 'CyImgView.exe') $stage
Get-ChildItem (Join-Path $bin 'plugins') -Filter *.dll | Where-Object { $_.Name -notlike 'oo2core*' } |
    Copy-Item -Destination (Join-Path $stage 'plugins')   # Oodle n'est jamais redistribué
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'LICENSE') $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null
Copy-Item (Join-Path $root 'third_party\LibRaw\LICENSE.LGPL') (Join-Path $stage 'licenses\LibRaw-LICENSE.LGPL')
Copy-Item (Join-Path $root 'third_party\LibRaw\LICENSE.CDDL') (Join-Path $stage 'licenses\LibRaw-LICENSE.CDDL')
Copy-Item (Join-Path $root 'third_party\LibRaw\COPYRIGHT') (Join-Path $stage 'licenses\LibRaw-COPYRIGHT')

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()

# Installeur : programme d'installation + archive + pied « CYIMGPK1 » + taille de l'archive (uint64)
$setup = Join-Path $root "dist\CyImgView-$ver-Setup.exe"
$stub = [IO.File]::ReadAllBytes((Join-Path $bin 'setup-stub.exe'))
$payload = [IO.File]::ReadAllBytes($zip)
$out = [IO.File]::Create($setup)
$out.Write($stub, 0, $stub.Length)
$out.Write($payload, 0, $payload.Length)
$magic = [Text.Encoding]::ASCII.GetBytes('CYIMGPK1')
$out.Write($magic, 0, 8)
$len = [BitConverter]::GetBytes([uint64]$payload.Length)
$out.Write($len, 0, 8)
$out.Close()

Write-Host "Archive    : $zip"
Write-Host "Installeur : $setup"
Write-Host "Version    : v$ver"
Write-Host "SHA-256    : $hash"
