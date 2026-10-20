@echo off
setlocal enabledelayedexpansion

echo =======================================================
echo    SV MultiCam Replay - Compilador para OBS Studio
echo =======================================================
echo.

cd /d "%~dp0"

echo [1/4] Limpiando residuos anteriores...
if exist "build" (
    rmdir /s /q "build"
)
mkdir "build"

echo [2/4] Buscando compilador de Visual Studio...
set "VS_DIR="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -property installationPath`) do (
        set "VS_DIR=%%i"
    )
)

if not defined VS_DIR (
    echo [ERROR] No se encontro instalacion de Visual Studio.
    pause
    exit /b 1
)

echo Visual Studio detectado en: !VS_DIR!

echo [3/4] Activando entorno de compilacion x64...
set "VCVARS="
if exist "!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat" (
    set "VCVARS=!VS_DIR!\VC\Auxiliary\Build\vcvars64.bat"
) else if exist "!VS_DIR!\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARS=!VS_DIR!\VC\Auxiliary\Build\vcvarsall.bat"
) else if exist "!VS_DIR!\Common7\Tools\VsDevCmd.bat" (
    set "VCVARS=!VS_DIR!\Common7\Tools\VsDevCmd.bat"
)

if defined VCVARS (
    echo Cargando entorno: !VCVARS!
    call "!VCVARS!" -arch=x64 -host_arch=x64 >nul 2>nul
) else (
    echo [AVISO] Continuando con la configuracion del sistema...
)

cd "build"

echo [4/4] Configurando proyecto con CMake...
cmake .. -A x64
if %errorlevel% neq 0 (
    echo Reintentando configuracion directa con CMake...
    cmake ..
)

if %errorlevel% neq 0 (
    echo.
    echo [ERROR] No se pudo configurar el proyecto con CMake.
    pause
    exit /b 1
)

echo.
echo Compilando plugin en modo Release...
cmake --build . --config Release

if %errorlevel% neq 0 (
    echo [ERROR] Fallo al compilar el codigo.
    pause
    exit /b 1
)

echo.
echo =======================================================
echo  COMPILACION COMPLETADA CON EXITO
echo  Archivo generado: build\Release\sv-multicam-replay.dll
echo =======================================================
echo.

set "OBS_DIR=C:\Program Files\obs-studio\obs-plugins\64bit"
if exist "%OBS_DIR%" (
    echo Copiando sv-multicam-replay.dll a OBS Studio...
    copy /y "Release\sv-multicam-replay.dll" "%OBS_DIR%\"
    if !errorlevel! equ 0 (
        echo [OK] Plugin instalado con exito en OBS!
    ) else (
        echo [!] Copia manualmente build\Release\sv-multicam-replay.dll a %OBS_DIR%
    )
)

pause
