# Compila SM64 para PSP dentro de Docker.
#
#   .\build_psp.ps1               -> -O3 (el de siempre). Resultado: build\us_psp\mario64\EBOOT.PBP
#   .\build_psp.ps1 -Opt O2       -> -O2 sin alinear funciones. Resultado: build_O2\us_psp\mario64\EBOOT.PBP
#   .\build_psp.ps1 -Clean        -> borra antes la carpeta de compilacion de esa configuracion
#   .\build_psp.ps1 -Rebuild      -> vuelve a construir la imagen de Docker (si cambias Dockerfile.psp)
#
# Cada configuracion usa su propia carpeta de compilacion, asi se puede cambiar de -O3 a -O2 sin borrar nada ni recompilar todo.
param(
    [ValidateSet("O3", "O2")][string]$Opt = "O3",
    [switch]$Clean,
    [switch]$Rebuild
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

if ($Clean -and (Test-Path $buildDir)) {
    Write-Host "Borrando $buildDir ..."
    Remove-Item -Recurse -Force $buildDir
}

# 3) compilar
$jobs = $env:NUMBER_OF_PROCESSORS
if (-not $jobs) { $jobs = 8 }
docker run --rm @envArgs -v "${PWD}:/sm64" -w /sm64 $img make TARGET_PSP=1 pbp VERSION=us "-j$jobs" "BUILD_DIR_BASE=$buildDir"
if ($LASTEXITCODE -ne 0) { Write-Host "La compilacion fallo"; exit 1 }

Write-Host ""
Write-Host "Listo: $buildDir\us_psp\mario64\EBOOT.PBP"
