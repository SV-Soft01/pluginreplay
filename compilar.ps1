# SV MultiCam Replay - Compilador PowerShell para OBS Studio
$ErrorActionPreference = "Stop"

Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host "   SV MultiCam Replay - Compilador para OBS Studio" -ForegroundColor Cyan
Write-Host "=======================================================" -ForegroundColor Cyan
Write-Host ""

Set-Location -Path $PSScriptRoot

Write-Host "[1/4] Buscando CMake en el sistema..." -ForegroundColor Yellow
$cmakePath = "cmake"
$cmakeFound = $false

if (Get-Command "cmake" -ErrorAction SilentlyContinue) {
    $cmakeFound = $true
} else {
    $commonPaths = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    )
    foreach ($p in $commonPaths) {
        if (Test-Path $p) {
            $cmakePath = $p
            $cmakeFound = $true
            Write-Host "CMake detectado en: $p" -ForegroundColor Green
            break
        }
    }
}

if (-not $cmakeFound) {
    Write-Host "[ERROR] CMake no está instalado o no se encuentra en el sistema." -ForegroundColor Red
    Write-Host "Puedes instalarlo de dos formas muy rápidas:" -ForegroundColor Yellow
    Write-Host "1. Con el instalador de Visual Studio 2022 -> C++ -> Marca 'Herramientas de CMake para C++'" -ForegroundColor Yellow
    Write-Host "2. Descarga el instalador oficial de https://cmake.org/download/ (Windows x64 Installer)" -ForegroundColor Yellow
    Read-Host "Presiona Enter para salir..."
    exit 1
}

Write-Host "[2/4] Preparando carpeta de compilación 'build'..." -ForegroundColor Yellow
if (-not (Test-Path "build")) {
    New-Item -ItemType Directory -Path "build" | Out-Null
}

Set-Location -Path "build"

Write-Host "[3/4] Generando proyecto con CMake (MSVC x64)..." -ForegroundColor Yellow
& $cmakePath .. -A x64

Write-Host "[4/4] Compilando en modo Release..." -ForegroundColor Yellow
& $cmakePath --build . --config Release

$dllPath = "Release\sv-multicam-replay.dll"
if (Test-Path $dllPath) {
    Write-Host ""
    Write-Host "=======================================================" -ForegroundColor Green
    Write-Host " ¡COMPILACIÓN COMPLETADA CON ÉXITO!" -ForegroundColor Green
    Write-Host " Archivo generado: $dllPath" -ForegroundColor Green
    Write-Host "=======================================================" -ForegroundColor Green
    Write-Host ""

    $obsPluginsDir = "C:\Program Files\obs-studio\obs-plugins\64bit"
    if (Test-Path $obsPluginsDir) {
        $copiar = Read-Host "¿Deseas copiar automáticamente el plugin a OBS Studio? (S/N)"
        if ($copiar -eq "S" -or $copiar -eq "s") {
            try {
                Copy-Item -Path $dllPath -Destination "$obsPluginsDir\" -Force
                Write-Host "[OK] ¡Plugin instalado exitosamente en OBS Studio!" -ForegroundColor Green
            } catch {
                Write-Host "[!] No se pudo copiar. Asegúrate de ejecutar PowerShell como Administrador." -ForegroundColor Red
            }
        }
    }
}

Read-Host "Presiona Enter para finalizar..."
