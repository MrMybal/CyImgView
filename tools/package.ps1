# Construit l'archive de release : dist\CyImgView-<version>-win64.zip
#   .\tools\package.ps1            (compile en Release puis empaquette)
#   .\tools\package.ps1 -NoBuild   (empaquette le build existant)
# L'archive contient CyImgView.exe + plugins\ + Documentation\ ; elle est jointe à la release GitHub
# (étiquette « v<version> ») pour que la mise à jour automatique la trouve.
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$h = Get-Content (Join-Path $root 'src\version.h') -Raw
$ver = '{0}.{1}.{2}' -f ([regex]::Match($h, 'VERSION_MAJOR (\d+)').Groups[1].Value),
                        ([regex]::Match($h, 'VERSION_MINOR (\d+)').Groups[1].Value),
                        ([regex]::Match($h, 'VERSION_PATCH (\d+)').Groups[1].Value)

if (-not $NoBuild) {
    # Compilation complète : une compilation incrémentale peut garder un objet compilé avec une
    # ancienne version d'un en-tête (modifié pendant une compilation) -> plantages imprévisibles.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'build.ps1') -Clean
    if ($LASTEXITCODE -ne 0) { throw 'La compilation a échoué.' }
}

$bin = Join-Path $root 'build\Release\bin'
# Le plugin RAW utilise OpenMP ; ne jamais publier un paquet qui dépend du PC de compilation.
if (-not (Test-Path -LiteralPath (Join-Path $bin 'plugins/vcomp140.dll'))) {
    throw 'Runtime OpenMP x64 absent du build : impossible de distribuer le plugin RAW.'
}
& node (Join-Path $root '.CyDocBuild\build.mjs') --out (Join-Path $bin 'Documentation')
if ($LASTEXITCODE -ne 0) { throw 'La génération du manuel a échoué.' }
$stage = Join-Path $root "dist\CyImgView-$ver-win64"
$zip = "$stage.zip"
$distRoot = [IO.Path]::GetFullPath((Join-Path $root 'dist')) + [IO.Path]::DirectorySeparatorChar
$stage = [IO.Path]::GetFullPath($stage)
if (-not $stage.StartsWith($distRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Chemin de staging invalide.' }
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage 'plugins') | Out-Null

Copy-Item (Join-Path $bin 'CyImgView.exe') $stage
Copy-Item -LiteralPath (Join-Path $bin 'Documentation') -Destination (Join-Path $stage 'Documentation') -Recurse -Force
# Oodle n'est jamais redistribué ; les plugins de test (CYIMG_TEST_PLUGINS) vont dans bin/testplugins,
# jamais dans bin\plugins : le filtre crashtest* n'est qu'une sécurité en cas de copie manuelle.
Get-ChildItem (Join-Path $bin 'plugins') -Filter *.dll | Where-Object { $_.Name -notlike 'oo2core*' -and $_.Name -notlike 'crashtest*' } |
    Copy-Item -Destination (Join-Path $stage 'plugins')
Copy-Item (Join-Path $root 'README.md') $stage
Copy-Item (Join-Path $root 'README.fr.md') $stage
Copy-Item (Join-Path $root 'LICENSE') $stage
New-Item -ItemType Directory -Force (Join-Path $stage 'licenses') | Out-Null
Copy-Item (Join-Path $root 'third_party\LibRaw\LICENSE.LGPL') (Join-Path $stage 'licenses\LibRaw-LICENSE.LGPL')
Copy-Item (Join-Path $root 'third_party\LibRaw\LICENSE.CDDL') (Join-Path $stage 'licenses\LibRaw-LICENSE.CDDL')
Copy-Item (Join-Path $root 'third_party\LibRaw\COPYRIGHT') (Join-Path $stage 'licenses\LibRaw-COPYRIGHT')
if (Test-Path (Join-Path $bin 'plugins/effects')) {
    Copy-Item -LiteralPath (Join-Path $bin 'plugins/effects') -Destination (Join-Path $stage 'plugins/effects') -Recurse -Force
}

# Licences des bibliothèques intégrées aux plugins : licenses\<bibliothèque>-<fichier>
$thirdLicenses = @(
    'libwebp\COPYING', 'libwebp\PATENTS', 'libheif\COPYING', 'libde265\COPYING', 'dav1d\COPYING',
    'libjxl\LICENSE', 'libjxl\PATENTS', 'highway\LICENSE', 'openjpeg\LICENSE', 'openexr\LICENSE.md',
    'openexr\PATENTS', 'openexr\external\zstd\LICENSE', 'openexr\external\OpenJPH\LICENSE',
    'libdeflate\COPYING', 'Imath\LICENSE.md', 'exiv2\COPYING', 'exiv2\LICENSE.txt', 'exiv2\doc\COPYING-XMPSDK',
    'expat\expat\COPYING', 'zlib\LICENSE')
foreach ($l in $thirdLicenses) {
    $src = Join-Path $root "third_party\$l"
    if (-not (Test-Path $src)) { throw "Licence manquante : $src" }
    Copy-Item $src (Join-Path $stage ('licenses\' + $l.Replace('\', '-')))
}

if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Remove-Item -LiteralPath $stage -Recurse -Force
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
