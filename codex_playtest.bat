@echo off
setlocal enabledelayedexpansion

set "SCRIPT_PATH=%~f0"
set "REPO_ROOT=%~dp0"
set "LAUNCHER_LOG=%REPO_ROOT%codex_playtest_launcher.log"

if not "%~1"=="__RUN__" (
    powershell -NoProfile -Command "foreach($p in @('%LAUNCHER_LOG%')){ if(Test-Path $p){ Remove-Item -Force $p -ErrorAction SilentlyContinue } }"
    cmd /v:on /c call "%SCRIPT_PATH%" __RUN__ 2^>^&1 | powershell -NoProfile -Command "$input | Tee-Object -FilePath '%LAUNCHER_LOG%'; exit $LASTEXITCODE"
    exit /b %ERRORLEVEL%
)

shift
pushd "%REPO_ROOT%" >nul
set "REPO_ROOT=%CD%"
set "BUILD_CONFIG=RelWithDebInfo"
set "BUILD_EXIT_CODE=0"
set "RUN_EXIT_CODE=0"

if not defined CODEX_PLAYTEST_MAP set "CODEX_PLAYTEST_MAP=forrest"
if not defined CODEX_PLAYTEST_SECONDS set "CODEX_PLAYTEST_SECONDS=60"
if not defined CODEX_PLAYTEST_PROFILE set "CODEX_PLAYTEST_PROFILE=default"
if not defined CODEX_PLAYTEST_ALLOW_SHORT set "CODEX_PLAYTEST_ALLOW_SHORT=0"
if /I "%CODEX_PLAYTEST_PROFILE%"=="spider_slow" set "CODEX_PLAYTEST_ALLOW_SHORT=1"
for /f %%S in ('powershell -NoProfile -Command "$s=60; [void][int]::TryParse($env:CODEX_PLAYTEST_SECONDS,[ref]$s); if($env:CODEX_PLAYTEST_ALLOW_SHORT -eq "1"){ [Math]::Max(1,$s) } else { [Math]::Max(60,$s) }"') do set "CODEX_PLAYTEST_SECONDS=%%S"
if /I "%CODEX_PLAYTEST_PROFILE%"=="spider_slow" if "%CODEX_PLAYTEST_SECONDS%"=="60" set "CODEX_PLAYTEST_SECONDS=120"
if not defined CODEX_PLAYTEST_REPORT set "CODEX_PLAYTEST_REPORT=codex_playtest_report.md"
if not defined VIBBLE_SAFE_LOADING set "VIBBLE_SAFE_LOADING=1"

if not defined CODEX_PLAYTEST_FRAME_LIMIT (
    powershell -NoProfile -Command "$s=60; if([int]::TryParse($env:CODEX_PLAYTEST_SECONDS,[ref]$s)){ [Math]::Max(1,$s*60) } else { 3600 }" > "%TEMP%\codex_playtest_frame_limit.txt"
    set /p CODEX_PLAYTEST_FRAME_LIMIT=<"%TEMP%\codex_playtest_frame_limit.txt"
    del /q "%TEMP%\codex_playtest_frame_limit.txt" >nul 2>&1
)

set "VIBBLE_AUTOSTART_MAP=%CODEX_PLAYTEST_MAP%"
set "VIBBLE_RUNTIME_FRAME_LIMIT=%CODEX_PLAYTEST_FRAME_LIMIT%"
set "VIBBLE_CODEX_PLAYTEST_INPUT=1"
set "VIBBLE_CODEX_PLAYTEST_PROFILE=%CODEX_PLAYTEST_PROFILE%"
set "METADATA_FILE=%REPO_ROOT%\codex_playtest_metadata.json"
set "REPORT_PATH=%REPO_ROOT%\%CODEX_PLAYTEST_REPORT%"

echo [codex_playtest.bat] Repo root: %REPO_ROOT%
echo [codex_playtest.bat] Map: %CODEX_PLAYTEST_MAP%
echo [codex_playtest.bat] Seconds: %CODEX_PLAYTEST_SECONDS%
echo [codex_playtest.bat] Profile: %CODEX_PLAYTEST_PROFILE%
echo [codex_playtest.bat] Frame limit: %CODEX_PLAYTEST_FRAME_LIMIT%
echo [codex_playtest.bat] Report: %REPORT_PATH%

call :RotateFile "%REPO_ROOT%\log.txt"
call :RotateFile "%REPO_ROOT%\runtime_frame_stats.csv"
for %%P in (
    "%REPO_ROOT%\codex_playtest_stdout.log"
    "%REPO_ROOT%\codex_playtest_stderr.log"
    "%REPO_ROOT%\codex_playtest_metadata.json"
    "%REPORT_PATH%"
) do (
    if exist %%~P del /q %%~P >nul 2>&1
)

call :BuildGame
set "BUILD_EXIT_CODE=%ERRORLEVEL%"

if not "%BUILD_EXIT_CODE%"=="0" (
    echo [codex_playtest.bat] Build failed with exit code %BUILD_EXIT_CODE%.
    powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\codex_playtest_report.ps1" ^
        -RepoRoot "%REPO_ROOT%" ^
        -ReportPath "%REPORT_PATH%" ^
        -MetadataPath "%METADATA_FILE%" ^
        -BuildExitCode %BUILD_EXIT_CODE% ^
        -RunExitCode 1 ^
        -TimedOut:$false ^
        -TimeoutForcedKill:$false ^
        -DurationSeconds 0 ^
        -Map "%CODEX_PLAYTEST_MAP%" ^
        -Profile "%CODEX_PLAYTEST_PROFILE%" ^
        -FrameLimit %CODEX_PLAYTEST_FRAME_LIMIT% ^
        -LauncherLogPath "%LAUNCHER_LOG%" ^
        -StdoutPath "%REPO_ROOT%\codex_playtest_stdout.log" ^
        -StderrPath "%REPO_ROOT%\codex_playtest_stderr.log"
    popd >nul
    exit /b %BUILD_EXIT_CODE%
)

set "EXE=%REPO_ROOT%\release\engine.exe"
if not exist "%EXE%" (
    echo [codex_playtest.bat] Executable not found after build: "%EXE%"
    powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\codex_playtest_report.ps1" ^
        -RepoRoot "%REPO_ROOT%" ^
        -ReportPath "%REPORT_PATH%" ^
        -MetadataPath "%METADATA_FILE%" ^
        -BuildExitCode 1 ^
        -RunExitCode 1 ^
        -TimedOut:$false ^
        -TimeoutForcedKill:$false ^
        -DurationSeconds 0 ^
        -Map "%CODEX_PLAYTEST_MAP%" ^
        -Profile "%CODEX_PLAYTEST_PROFILE%" ^
        -FrameLimit %CODEX_PLAYTEST_FRAME_LIMIT% ^
        -LauncherLogPath "%LAUNCHER_LOG%" ^
        -StdoutPath "%REPO_ROOT%\codex_playtest_stdout.log" ^
        -StderrPath "%REPO_ROOT%\codex_playtest_stderr.log"
    popd >nul
    exit /b 1
)

echo [codex_playtest.bat] Launching Codex playtest runner.
powershell -NoProfile -ExecutionPolicy Bypass -File "%REPO_ROOT%\scripts\codex_playtest.ps1" ^
    -RepoRoot "%REPO_ROOT%" ^
    -ExePath "%EXE%" ^
    -Map "%CODEX_PLAYTEST_MAP%" ^
    -Seconds %CODEX_PLAYTEST_SECONDS% ^
    -FrameLimit %CODEX_PLAYTEST_FRAME_LIMIT% ^
    -Profile "%CODEX_PLAYTEST_PROFILE%" ^
    -ReportPath "%REPORT_PATH%" ^
    -MetadataPath "%METADATA_FILE%" ^
    -LauncherLogPath "%LAUNCHER_LOG%" ^
    -StdoutPath "%REPO_ROOT%\codex_playtest_stdout.log" ^
    -StderrPath "%REPO_ROOT%\codex_playtest_stderr.log"
set "RUN_EXIT_CODE=%ERRORLEVEL%"

echo [codex_playtest.bat] Codex playtest finished with exit code %RUN_EXIT_CODE%.
popd >nul
exit /b %RUN_EXIT_CODE%

:BuildGame
if not exist "%REPO_ROOT%\CMakeLists.txt" (
    echo [ERROR] CMakeLists.txt not found at repo root: "%REPO_ROOT%\CMakeLists.txt"
    exit /b 1
)

set "SETUP_JSON_FILE=%REPO_ROOT%\setup.json"
set "NEED_SETUP=0"
if exist "%SETUP_JSON_FILE%" (
    powershell -NoProfile -Command "try{$j=Get-Content '%SETUP_JSON_FILE%' -Raw|ConvertFrom-Json;if($j.status -eq 'SUCCESS'){exit 0}else{exit 1}}catch{exit 1}"
    if errorlevel 1 set "NEED_SETUP=1"
) else (
    set "NEED_SETUP=1"
)

if "%NEED_SETUP%"=="1" (
    echo [codex_playtest.bat] Running setup.bat...
    call "%REPO_ROOT%\setup.bat"
    if errorlevel 1 exit /b 1
)

call :EnsureDevShell
if errorlevel 1 exit /b 1

set "LOCAL_VCPKG=%REPO_ROOT%\vcpkg"
if exist "%LOCAL_VCPKG%\scripts\buildsystems\vcpkg.cmake" set "VCPKG_ROOT=%LOCAL_VCPKG%"

if not exist "%REPO_ROOT%\CMakePresets.json" (
    echo [ERROR] CMakePresets.json not found in repo root.
    exit /b 1
)

set "CMAKE_CMD="
call :LocateCMake
if not defined CMAKE_CMD (
    echo [ERROR] CMake executable not found.
    exit /b 1
)

for %%P in ("%CMAKE_CMD%") do set "CMAKE_DIR=%%~dpP"
if defined CMAKE_DIR set "PATH=%CMAKE_DIR%;%PATH%"

echo [codex_playtest.bat] Using CMake from: %CMAKE_CMD%
echo [codex_playtest.bat] Configuring with preset: windows-vcpkg
"%CMAKE_CMD%" --preset windows-vcpkg
if errorlevel 1 exit /b 1

echo [codex_playtest.bat] Building with preset: windows-vcpkg-release (%BUILD_CONFIG%)
"%CMAKE_CMD%" --build --preset windows-vcpkg-release --config %BUILD_CONFIG%
if errorlevel 1 exit /b 1

set "RELEASE_DIR=%REPO_ROOT%\release"
if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
if errorlevel 1 exit /b 1

for %%P in ("%RELEASE_DIR%\*.exe" "%RELEASE_DIR%\*.dll" "%RELEASE_DIR%\*.pdb") do (
    if exist %%~P del /q %%~P >nul 2>&1
)

call :CollectArtifacts "%REPO_ROOT%"
call :CollectArtifacts "%REPO_ROOT%\ENGINE"
call :CollectArtifacts "%REPO_ROOT%\build\%BUILD_CONFIG%"
call :CollectArtifacts "%REPO_ROOT%\build"

if not exist "%RELEASE_DIR%\engine.exe" (
    echo [ERROR] Executable not found in release directory.
    exit /b 1
)
exit /b 0

:CollectArtifacts
set "SRC_DIR=%~1"
if not exist "%SRC_DIR%" goto :eof
for %%E in (exe dll pdb) do (
    for /f "delims=" %%F in ('dir /b "%SRC_DIR%\*.%%E" 2^>nul') do (
        move /y "%SRC_DIR%\%%F" "%RELEASE_DIR%" >nul
    )
)
goto :eof

:RotateFile
set "ROTATE_TARGET=%~1"
if not exist "%ROTATE_TARGET%" goto :eof
if exist "%ROTATE_TARGET%.codex_prev" del /q "%ROTATE_TARGET%.codex_prev" >nul 2>&1
move /y "%ROTATE_TARGET%" "%ROTATE_TARGET%.codex_prev" >nul
goto :eof

:EnsureDevShell
where cl >nul 2>&1 && (echo [codex_playtest.bat] MSVC already on PATH. & exit /b 0)
set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=* delims=" %%I in (`"%VSWHERE%" -latest -products Microsoft.VisualStudio.Product.BuildTools -property installationPath`) do set "VSROOT=%%I"
)
if not defined VSROOT if exist "C:\VS2022\BuildTools" set "VSROOT=C:\VS2022\BuildTools"
if defined VSROOT (
    echo [codex_playtest.bat] Loading dev environment from "%VSROOT%"...
    if exist "%VSROOT%\Common7\Tools\VsDevCmd.bat" (
        call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -host_arch=x64 -arch=x64
    ) else if exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
        call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
    )
)
where cl >nul 2>&1 && (echo [codex_playtest.bat] MSVC toolchain loaded. & exit /b 0)
echo [ERROR] Could not load MSVC dev environment.
exit /b 1

:LocateCMake
set "CMAKE_CMD="
for /f "delims=" %%I in ('where cmake 2^>nul') do (
    if not defined CMAKE_CMD set "CMAKE_CMD=%%~fI"
)
if defined CMAKE_CMD goto :locate_done
for %%P in (
    "%ProgramFiles%\CMake\bin\cmake.exe"
    "C:\Program Files\CMake\bin\cmake.exe"
    "C:\Program Files (x86)\CMake\bin\cmake.exe"
    "%ProgramFiles(x86)%\CMake\bin\cmake.exe"
) do (
    if not defined CMAKE_CMD if exist %%~P set "CMAKE_CMD=%%~fP"
)
:locate_done
if defined CMAKE_CMD (
    for %%Q in ("%CMAKE_CMD%") do if exist %%~fQ set "CMAKE_CMD=%%~fQ"
)
exit /b 0
