# Compila SM64 para PSP dentro de Docker.
#
#   .\build_psp.ps1                  -> version US, -O3 (el de siempre). Resultado: build\us_psp\mario64\ (EBOOT.PBP + SM64.PRX)
#   .\build_psp.ps1 -Version eu      -> otra version: us (por defecto), jp, eu o sh. Resultado: build\eu_psp\mario64\
#                                       (necesita baserom.<version>.z64 junto al Makefile; sh necesita baserom.sh.z64 Y baserom.jp.z64)
#   .\build_psp.ps1 -Opt O2          -> -O2 sin alinear funciones. Resultado: build_O2\us_psp\mario64\ (EBOOT.PBP + SM64.PRX)
#   .\build_psp.ps1 -Clean           -> borra antes la compilacion de esa version y configuracion (las otras versiones no se tocan)
#   .\build_psp.ps1 -Rebuild      -> vuelve a construir la imagen de Docker (si cambias Dockerfile.psp)
#   .\build_psp.ps1 -Version eu -Release -> ademas del build normal, prepara el release SIN datos de la ROM (instalador en la PSP):
#                                       dist\psp_release\eu\PSP\GAME\SM64_EU\ (copia la carpeta PSP al Memory Stick). Solo con -Opt O3.
#                                       Con -Fast se salta la comprobacion lenta de que no queda nada de la ROM en la plantilla.
#
# Cada configuracion usa su propia carpeta de compilacion, asi se puede cambiar de -O3 a -O2 sin borrar nada ni recompilar todo.
param(
    [ValidateSet("us", "jp", "eu", "sh")][string]$Version = "us",
    [ValidateSet("O3", "O2")][string]$Opt = "O3",
    [switch]$Clean,
    [switch]$Rebuild,
    [switch]$Release,
    [switch]$Fast
)

$img = "sm64psp"
Set-Location $PSScriptRoot

# 1) imagen (solo la primera vez, o con -Rebuild). Se manda por stdin: no se envia la carpeta del proyecto como contexto.
docker image inspect $img *> $null
if ($Rebuild -or $LASTEXITCODE -ne 0) {
    Write-Host "Construyendo la imagen $img ..."
    Get-Content Dockerfile.psp | docker build -t $img -
    if ($LASTEXITCODE -ne 0) { Write-Host "Fallo al construir la imagen"; exit 1 }
}

# 2) carpeta de compilacion y opciones de cada configuracion (el Makefile lee PSP_OPT / PSP_FALIGN del entorno)
if ($Opt -eq "O2") {
    $buildDir = "build_O2"
    $envArgs = @("-e", "PSP_OPT=-O2", "-e", "PSP_FALIGN=")
} else {
    $buildDir = "build"
    $envArgs = @()
}

$outDir = "$buildDir\${Version}_psp"
if ($Clean -and (Test-Path $outDir)) {
    Write-Host "Borrando $outDir ..."
    Remove-Item -Recurse -Force $outDir
}

# comprobar las ROM necesarias antes de empezar
$roms = @("baserom.$Version.z64")
if ($Version -eq "sh") { $roms += "baserom.jp.z64" }
foreach ($r in $roms) {
    if (-not (Test-Path $r)) { Write-Host "Falta $r (debe estar junto al Makefile, con ese nombre exacto)"; exit 1 }
}

# 3) compilar
$jobs = $env:NUMBER_OF_PROCESSORS
if (-not $jobs) { $jobs = 8 }
$target = "pbp"
$extra = @()
if ($Release) {
    if ($Opt -ne "O3") { Write-Host "-Release solo se puede combinar con -Opt O3"; exit 1 }
    $target = "release"
    if ($Fast) { $extra += "FAST=1" }
}
docker run --rm @envArgs -v "${PWD}:/sm64" -w /sm64 $img make TARGET_PSP=1 $target VERSION=$Version "-j$jobs" "BUILD_DIR_BASE=$buildDir" @extra
if ($LASTEXITCODE -ne 0) { Write-Host "La compilacion fallo"; exit 1 }

Write-Host ""
Write-Host "Listo: $buildDir\${Version}_psp\mario64\  (copia EBOOT.PBP y SM64.PRX juntos a la misma carpeta de la PSP)"
if ($Release) {
    Write-Host "Release sin datos de la ROM: dist\psp_release\$Version\PSP\GAME\SM64_$($Version.ToUpper())\  (copia la carpeta PSP al Memory Stick; la ROM va en data\)"
}
