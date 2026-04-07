@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
set "SETUP_STATUS=%SCRIPT_DIR%setup.json"
set "VSBT_INSTALL_DIR=C:\VS2022\BuildTools"
set "VS_BOOT_URL=https://aka.ms/vs/17/release/vs_BuildTools.exe"
set "VS_BOOT_EXE=%TEMP%\vs_BuildTools.exe"
set "INSTALL_TIMEOUT_SECS=5400"

if "%~1"=="__RUN__" (
    shift
    goto :main
)

set "SCRIPT_PATH=%~f0"

cmd /v:on /c call "%SCRIPT_PATH%" __RUN__ %* 2>&1
set "RC=%ERRORLEVEL%"
call :WriteSetupStatus %RC%
exit /b %RC%

:main
pushd "%~dp0" >nul || goto :fail
set "REPO_ROOT=%CD%"
cd /d "%REPO_ROOT%" || goto :fail

call :EnsureWinget       || goto :fail
call :EnsureGit          || goto :fail
call :EnsureVSBuildTools || goto :fail
call :EnsureDevShell     || goto :fail
call :EnsureCMake        || goto :fail
call :EnsureNinja        || goto :fail

set "LOCAL_VCPKG=%REPO_ROOT%\vcpkg"
call :EnsureLocalVcpkg || goto :fail
set "VCPKG_ROOT=%LOCAL_VCPKG%"

if not exist "%LOCAL_VCPKG%\LICENSE.txt" (
    echo [setup.bat] vcpkg\LICENSE.txt missing, creating it for vcpkg-cmake...
    if exist "%LOCAL_VCPKG%\LICENSE" (
        copy /y "%LOCAL_VCPKG%\LICENSE" "%LOCAL_VCPKG%\LICENSE.txt" >nul
    ) else (
        if exist "%LOCAL_VCPKG%\LICENSE.md" (
            copy /y "%LOCAL_VCPKG%\LICENSE.md" "%LOCAL_VCPKG%\LICENSE.txt" >nul
        ) else (
            >"%LOCAL_VCPKG%\LICENSE.txt" echo Local vcpkg license placeholder
        )
    )
)

if exist "%REPO_ROOT%\vcpkg.json" (
    powershell -NoProfile -Command ^
      "$b=(Get-Content 'vcpkg.json' -Raw | ConvertFrom-Json).'builtin-baseline';" ^
      "if($b -and $b -match '^[0-9a-fA-F]{40}$'){exit 0}else{Write-Host '[ERROR] builtin-baseline invalid:' $b; exit 1}"
    if errorlevel 1 (
        echo [ERROR] builtin-baseline is not a 40-hex SHA. Aborting.
        goto :fail
    )

    call :EnsureVcpkgBaselineAvailable || goto :fail

    if /I "%VIBBLE_UPDATE_BASELINE%"=="1" (
        echo [setup.bat] Updating vcpkg baseline because VIBBLE_UPDATE_BASELINE=1...
        "%LOCAL_VCPKG%\vcpkg.exe" x-update-baseline
        if errorlevel 1 (
            echo [ERROR] vcpkg baseline update failed.
            goto :fail
        )
    ) else (
        echo [setup.bat] Keeping existing vcpkg baseline. Set VIBBLE_UPDATE_BASELINE=1 to update.
    )
) else (
    echo [setup.bat] vcpkg.json not found, skipping baseline update.
)

if exist "%REPO_ROOT%\vcpkg.json" (
    echo [setup.bat] Installing vcpkg manifest dependencies...
    "%LOCAL_VCPKG%\vcpkg.exe" install --triplet x64-windows --feature-flags=manifests,binarycaching
    if errorlevel 1 (
        echo [ERROR] vcpkg install failed.
        goto :fail
    )
) else (
    echo [setup.bat] No vcpkg.json found. You can still build if your CMake presets do not use manifests.
)

echo [setup.bat] Setup complete.
popd >nul
exit /b 0

:EnsureWinget
where winget >nul 2>&1 && exit /b 0
echo [ERROR] winget is not available. Install App Installer from Microsoft Store.
exit /b 1

:EnsureGit
git --version >nul 2>&1 && (
    echo [setup.bat] Git is installed.
    exit /b 0
)
echo [setup.bat] Installing Git via winget...
winget install --id Git.Git -e --source winget --silent || (
    echo [ERROR] Git install failed.
    exit /b 1
)
git --version >nul 2>&1 && (
    echo [setup.bat] Git installed and on PATH.
    exit /b 0
)
echo [ERROR] Git install succeeded but git is still not on PATH.
exit /b 1

:EnsureVSBuildTools
where cl >nul 2>&1 && (
    echo [setup.bat] MSVC toolchain already available.
    exit /b 0
)

set "VS_INSTALL_PATH="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq tokens=* delims=" %%I in (`
        "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    `) do set "VS_INSTALL_PATH=%%I"
)

if not defined VS_INSTALL_PATH if exist "C:\Program Files\Microsoft Visual Studio\2022\BuildTools" set "VS_INSTALL_PATH=C:\Program Files\Microsoft Visual Studio\2022\BuildTools"
if not defined VS_INSTALL_PATH if exist "%VSBT_INSTALL_DIR%" set "VS_INSTALL_PATH=%VSBT_INSTALL_DIR%"

if defined VS_INSTALL_PATH (
    echo [setup.bat] Found existing MSVC install at "%VS_INSTALL_PATH%".
    exit /b 0
)

echo [setup.bat] MSVC build tools not found. Installing Visual Studio 2022 Build Tools with winget...
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --source winget --silent ^
  --override "--installPath \"%VSBT_INSTALL_DIR%\" --quiet --wait --norestart --nocache --includeRecommended --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --add Microsoft.VisualStudio.Component.Windows11SDK.22621"
if errorlevel 1 (
    echo [ERROR] VS Build Tools install failed.
    exit /b 1
)

set "VS_INSTALL_PATH="
if exist "%VSWHERE%" (
    for /f "usebackq tokens=* delims=" %%I in (`
        "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    `) do set "VS_INSTALL_PATH=%%I"
)
if not defined VS_INSTALL_PATH if exist "%VSBT_INSTALL_DIR%" set "VS_INSTALL_PATH=%VSBT_INSTALL_DIR%"

if not defined VS_INSTALL_PATH (
    echo [ERROR] VS Build Tools installation did not register correctly.
    exit /b 1
)

echo [setup.bat] VS Build Tools installed at "%VS_INSTALL_PATH%".
exit /b 0

:EnsureDevShell
where cl >nul 2>&1 && (
    echo [setup.bat] MSVC already on PATH.
    exit /b 0
)

set "VSROOT="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=* delims=" %%I in (`
        "%VSWHERE%" -latest -products Microsoft.VisualStudio.Product.BuildTools -property installationPath
    `) do set "VSROOT=%%I"
)
if not defined VSROOT if exist "%VSBT_INSTALL_DIR%" set "VSROOT=%VSBT_INSTALL_DIR%"

if defined VSROOT (
    echo [setup.bat] Loading dev environment from "%VSROOT%"...
    if exist "%VSROOT%\Common7\Tools\VsDevCmd.bat" (
        call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -host_arch=x64 -arch=x64
    ) else (
        if exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
            call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
        )
    )
)

where cl >nul 2>&1 && (
    echo [setup.bat] MSVC toolchain loaded.
    exit /b 0
)
echo [ERROR] Could not load MSVC dev environment.
exit /b 1

:EnsureCMake
where cmake >nul 2>&1 && (
    echo [setup.bat] CMake is installed.
    exit /b 0
)
echo [setup.bat] Installing CMake via winget...
winget install -e --id Kitware.CMake --source winget --silent || (
    echo [ERROR] CMake install failed.
    exit /b 1
)
echo [setup.bat] CMake installed.
exit /b 0

:EnsureNinja
where ninja >nul 2>&1 && (
    echo [setup.bat] Ninja is installed.
    exit /b 0
)
echo [setup.bat] Installing Ninja via winget...
winget install -e --id Ninja-build.Ninja --source winget --silent || (
    echo [ERROR] Ninja install failed.
    exit /b 1
)
echo [setup.bat] Ninja installed.
exit /b 0

:EnsureLocalVcpkg
if exist "%LOCAL_VCPKG%\.git" (
    echo [setup.bat] Reusing existing local vcpkg at "%LOCAL_VCPKG%".
    call :RefreshVcpkgRepo || exit /b 1
    if not exist "%LOCAL_VCPKG%\vcpkg.exe" (
        echo [setup.bat] Bootstrapping vcpkg...
        pushd "%LOCAL_VCPKG%" >nul
        call bootstrap-vcpkg.bat -disableMetrics
        if errorlevel 1 (
            popd >nul
            echo [ERROR] vcpkg bootstrap failed.
            exit /b 1
        )
        popd >nul
    )
    if not exist "%LOCAL_VCPKG%\scripts\buildsystems\vcpkg.cmake" (
        echo [ERROR] vcpkg toolchain file missing after refresh.
        exit /b 1
    )
    exit /b 0
)

if exist "%LOCAL_VCPKG%" (
    echo [setup.bat] Existing vcpkg directory is not a valid git repo. Recreating it...
    rmdir /s /q "%LOCAL_VCPKG%"
    if exist "%LOCAL_VCPKG%" (
        echo [ERROR] Failed to remove invalid vcpkg directory.
        exit /b 1
    )
)

echo [setup.bat] Cloning local vcpkg...
git clone https://github.com/microsoft/vcpkg.git "%LOCAL_VCPKG%"
if errorlevel 1 (
    echo [ERROR] Failed to clone vcpkg repository.
    exit /b 1
)

if not exist "%LOCAL_VCPKG%\bootstrap-vcpkg.bat" (
    echo [ERROR] bootstrap-vcpkg.bat not found in "%LOCAL_VCPKG%".
    exit /b 1
)

echo [setup.bat] Bootstrapping vcpkg...
pushd "%LOCAL_VCPKG%" >nul
call bootstrap-vcpkg.bat -disableMetrics
if errorlevel 1 (
    popd >nul
    echo [ERROR] vcpkg bootstrap failed.
    exit /b 1
)
popd >nul
exit /b 0

:RefreshVcpkgRepo
if not exist "%LOCAL_VCPKG%\.git" (
    echo [ERROR] vcpkg repo missing .git directory.
    exit /b 1
)

echo [setup.bat] Refreshing local vcpkg repository...
git -C "%LOCAL_VCPKG%" fetch origin --tags --prune
if errorlevel 1 (
    echo [ERROR] Failed to fetch latest refs for local vcpkg.
    exit /b 1
)

set "VCPKG_IS_SHALLOW="
for /f "usebackq delims=" %%S in (`git -C "%LOCAL_VCPKG%" rev-parse --is-shallow-repository 2^>nul`) do set "VCPKG_IS_SHALLOW=%%S"
if /I "%VCPKG_IS_SHALLOW%"=="true" (
    echo [setup.bat] Local vcpkg clone is shallow. Unshallowing...
    git -C "%LOCAL_VCPKG%" fetch --unshallow --tags
    if errorlevel 1 (
        echo [ERROR] Failed to unshallow local vcpkg repo.
        exit /b 1
    )
)
exit /b 0

:EnsureVcpkgBaselineAvailable
set "VCPKG_BASELINE="
for /f "usebackq delims=" %%B in (`
    powershell -NoProfile -Command "(Get-Content '%REPO_ROOT%\vcpkg.json' -Raw | ConvertFrom-Json).'builtin-baseline'"
`) do set "VCPKG_BASELINE=%%B"

if not defined VCPKG_BASELINE (
    echo [ERROR] Could not read builtin-baseline from vcpkg.json.
    exit /b 1
)

git -C "%LOCAL_VCPKG%" rev-parse --verify "%VCPKG_BASELINE%" >nul 2>&1
if errorlevel 1 (
    echo [setup.bat] Baseline commit %VCPKG_BASELINE% is missing locally. Fetching it...
    git -C "%LOCAL_VCPKG%" fetch origin %VCPKG_BASELINE%
    if errorlevel 1 (
        echo [ERROR] Failed to fetch required vcpkg baseline commit %VCPKG_BASELINE%.
        exit /b 1
    )
)

git -C "%LOCAL_VCPKG%" rev-parse --verify "%VCPKG_BASELINE%" >nul 2>&1
if errorlevel 1 (
    echo [setup.bat] Baseline commit still missing after targeted fetch. Refreshing full vcpkg repo...
    git -C "%LOCAL_VCPKG%" fetch origin --tags --prune
    if errorlevel 1 (
        echo [ERROR] Failed to refresh vcpkg repo while resolving baseline commit.
        exit /b 1
    )
)

git -C "%LOCAL_VCPKG%" rev-parse --verify "%VCPKG_BASELINE%" >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Required vcpkg baseline commit %VCPKG_BASELINE% is still unavailable.
    exit /b 1
)

exit /b 0

:DownloadVSBootstrapper
if exist "%VS_BOOT_EXE%" del /q "%VS_BOOT_EXE%" >nul 2>&1
powershell -NoProfile -Command ^
  "$ErrorActionPreference='Stop';[Net.ServicePointManager]::SecurityProtocol=[Net.SecurityProtocolType]::Tls12;Invoke-WebRequest -Uri '%VS_BOOT_URL%' -OutFile '%VS_BOOT_EXE%'"
if errorlevel 1 exit /b 1
exit /b 0

:RunWithTimeoutPS
set "_R_EXE=%~1"
set "_R_ARGS=%~2"
set "_R_TO=%~3"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$exe='%_R_EXE%';$args='%_R_ARGS%';$t=%_R_TO%;" ^
  "$p=Start-Process -FilePath $exe -ArgumentList $args -PassThru -WindowStyle Hidden;" ^
  "$sw=[Diagnostics.Stopwatch]::StartNew();" ^
  "while(-not $p.HasExited){" ^
  "  Start-Sleep -Seconds 2;" ^
  "  if($sw.Elapsed.TotalSeconds -gt $t){" ^
  "    try{ $p.Kill() } catch{}; exit 901" ^
  "  }" ^
  "};" ^
  "exit $p.ExitCode"
exit /b %ERRORLEVEL%

:WriteSetupStatus
set "RC=%~1"
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$code = %RC%;" ^
  "if ($code -eq 0) { $status = 'SUCCESS' } else { $status = 'FAIL' }" ^
  "$ts = Get-Date -Format o;" ^
  "$obj = @{ status = $status; timestamp = $ts; exitCode = $code };" ^
  "$obj | ConvertTo-Json -Depth 5 | Set-Content -Path '%SETUP_STATUS%' -Encoding UTF8"
goto :eof

:fail
echo [setup.bat] Setup failed.
popd >nul 2>nul
exit /b 1