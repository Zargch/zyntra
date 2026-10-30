@echo off
rem Compila Zyntra.exe con Visual Studio (2019 o posterior, Build Tools basta).
rem   build.bat          -> x64 (bin\x64\Zyntra.exe)
rem   build.bat x86      -> 32 bits (bin\x86\Zyntra.exe)
rem   build.bat all      -> ambas
setlocal
set ARCH=%1
if "%ARCH%"=="" set ARCH=x64
if /i "%ARCH%"=="all" (
  call "%~f0" x64 || exit /b 1
  call "%~f0" x86 || exit /b 1
  exit /b 0
)
set VCVARS=
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq delims=" %%i in (`call "%VSWHERE%" -all -latest -products * -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VCVARS%" (
  echo No se encontro Visual Studio con las herramientas de C++.
  exit /b 1
)
if /i "%ARCH%"=="x86" (call "%VCVARS%" x64_x86 >nul) else (call "%VCVARS%" x64 >nul)
cd /d "%~dp0"
set OUT=bin\%ARCH%
set OBJ=build\%ARCH%
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OBJ%" mkdir "%OBJ%"
rc /nologo /fo "%OBJ%\resource.res" src\resource.rc || exit /b 1
cl /nologo /std:c++17 /O1 /GL /EHsc /W3 /MT /utf-8 /bigobj /MP /guard:cf /DUNICODE /D_UNICODE /DNDEBUG /FIversion.h /Isrc ^
   /Fo"%OBJ%\\" src\*.cpp "%OBJ%\resource.res" /Fe:"%OUT%\Zyntra.exe" ^
   /link /LTCG /OPT:REF /OPT:ICF /SUBSYSTEM:WINDOWS /DYNAMICBASE /NXCOMPAT /GUARD:CF /MANIFEST:EMBED /MANIFESTUAC:"level='asInvoker'" || exit /b 1
copy /y assets\intro.mov "%OUT%\intro.mov" >nul
echo Listo: %OUT%\Zyntra.exe
endlocal
