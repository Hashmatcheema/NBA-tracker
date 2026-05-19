@echo off
REM Explorer double-click uses "cmd /c" and the window closes as soon as the .bat ends.
REM If there are no args (typical double-click), re-launch under "cmd /k" so the window stays open.
if /I "%~1"=="__J2K_STAY__" goto :j2k_entry
if not "%~1"=="" goto :j2k_entry
echo.%CMDCMDLINE% | findstr /I /C:"/c" >nul 2>&1
if errorlevel 1 goto :j2k_entry
REM Do not use CALL here: "call same.bat" breaks "goto :install" (label not found). Run the file as primary.
start "J2K Vision" /D "%~dp0" cmd /k ""%~f0" __J2K_STAY__"
exit /b 0

:j2k_entry
if /I "%~1"=="__J2K_STAY__" shift /1
setlocal
cd /d "%~dp0"

set ROOT=%~dp0..
set CPP_DIR=%ROOT%\cpp
set BUILD_DIR=%CPP_DIR%\build
set BUILD_DLL_DIR=%CPP_DIR%\build_dll
set PY_DIR=%ROOT%\python
if not exist "%PY_DIR%\bin" mkdir "%PY_DIR%\bin"
REM Native ORT + CUDA/cuDNN DLLs resolve from the same folder as j2k_ch.dll when Helios cwd differs.
set "PATH=%PY_DIR%\bin;%PATH%"
REM Prefer extracted ONNX Runtime GPU tree when present (override with env J2K_ONNXRUNTIME_ROOT).
REM Newest first: 1.26.0 (CUDA 12 + TRT 10) -> 1.21.0 -> ... -> 1.17.3 legacy fallback.
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.26.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.26.0"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.26.0\onnxruntime-win-x64-gpu-1.26.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.26.0\onnxruntime-win-x64-gpu-1.26.0"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.21.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.21.0"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.21.0\onnxruntime-win-x64-gpu-1.21.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.21.0\onnxruntime-win-x64-gpu-1.21.0"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.1\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.1"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.1\onnxruntime-win-x64-gpu-1.20.1\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.1\onnxruntime-win-x64-gpu-1.20.1"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.0"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.19.2\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.19.2"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.19.2\onnxruntime-win-x64-gpu-1.19.2\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.19.2\onnxruntime-win-x64-gpu-1.19.2"
REM Legacy 1.17.3 fallback (CUDA 11.8 + TRT 8)
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3\onnxruntime-win-x64-gpu-1.17.3\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3\onnxruntime-win-x64-gpu-1.17.3"
if "%J2K_ONNXRUNTIME_ROOT%"=="" if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3"
REM CUDA 11.x + cuDNN 8 on PATH (onnxruntime_providers_cuda.dll needs cudart64_110 / cudnn64_8 — Win32 error 126 if missing).
call :j2k_cuda_path
set J2K_DLL_OUT=%BUILD_DLL_DIR%\Release\j2k_ch.dll
set J2K_DLL_DEST=%PY_DIR%\bin\j2k_ch.dll
set REQ_FILE=%PY_DIR%\requirements.txt
set "J2K_PURE_FOREHEAD_CUE=1"
set "J2K_FOREHEAD_ONLY_RELEASE=1"
set "J2K_RELIABILITY_FIRST=0"
set "J2K_FALLBACK_MODE=none"
set "J2K_ANIM_CUE_ROI_LO=60"
set "J2K_ANIM_CUE_ROI_HI=85"
set "J2K_MODEL_PT_PRIMARY=%PY_DIR%\models\best.pt"
set "J2K_MODEL_PT_BALL=%PY_DIR%\models\basket_ball.pt"
set "RAW_ACTION=%~1"
set ACTION=%RAW_ACTION%
if "%ACTION%"=="" set ACTION=install
REM In scripted/terminal usage with an explicit command, default to no pause.
if not "%RAW_ACTION%"=="" if /I "%J2K_NO_PAUSE%"=="" set "J2K_NO_PAUSE=1"
REM #region agent log
call :debug_log H14 action_dispatch action_received {"action":"%ACTION%"}
REM #endregion

if /I "%ACTION%"=="help" goto :help
if /I "%ACTION%"=="install" goto :install
if /I "%ACTION%"=="update" goto :update
if /I "%ACTION%"=="update_files" goto :update_files
if /I "%ACTION%"=="apply_updates" goto :apply_updates
if /I "%ACTION%"=="sanity" goto :sanity
if /I "%ACTION%"=="build" goto :build
if /I "%ACTION%"=="run" goto :run
if /I "%ACTION%"=="test" goto :test
if /I "%ACTION%"=="validate_video" goto :vv
if /I "%ACTION%"=="export_onnx" goto :export_onnx
if /I "%ACTION%"=="upgrade_gpu" goto :upgrade_gpu

echo [j2k] Unknown command: %ACTION%
goto :help

REM --- debug log helper disabled after fix verification ---
:debug_log
exit /b 0

REM --- Prepend CUDA 11.x + cuDNN bins to PATH (ORT 1.17 GPU provider: LoadLibrary error 126 when deps missing).
REM     Install CUDA Toolkit 11.8 (or 11.7/11.6) + cuDNN 8 for CUDA 11.x. Override dirs:
REM       set J2K_CUDA_BIN=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v11.8\bin
REM       set J2K_CUDNN_BIN=C:\path\to\cudnn\bin
REM     Optional: set J2K_CUDA_PATH_LOG=1 to print which dirs were prepended.
:j2k_cuda_path
set "J2K_CUDA_RUNTIME_BIN="
if not "%J2K_CUDA_BIN%"=="" (
  if exist "%J2K_CUDA_BIN%\cudart64_110.dll" set "J2K_CUDA_RUNTIME_BIN=%J2K_CUDA_BIN%"
)
if "%J2K_CUDA_RUNTIME_BIN%"=="" (
  if defined CUDA_PATH if exist "%CUDA_PATH%\bin\cudart64_110.dll" set "J2K_CUDA_RUNTIME_BIN=%CUDA_PATH%\bin"
)
REM Any CUDA 11.x toolkit under Program Files ^(install adds here even if user PATH was not refreshed^).
if "%J2K_CUDA_RUNTIME_BIN%"=="" (
  for /d %%D in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v11.*") do (
    if exist "%%D\bin\cudart64_110.dll" set "J2K_CUDA_RUNTIME_BIN=%%D\bin"
  )
)
if "%J2K_CUDA_RUNTIME_BIN%"=="" (
  for /d %%D in ("%ProgramFiles(x86)%\NVIDIA GPU Computing Toolkit\CUDA\v11.*") do (
    if exist "%%D\bin\cudart64_110.dll" set "J2K_CUDA_RUNTIME_BIN=%%D\bin"
  )
)
REM CUDA 12.x detection (for TRT 10 / ORT 1.19+)
if "%J2K_CUDA_RUNTIME_BIN%"=="" (
  for /d %%D in ("%ProgramFiles%\NVIDIA GPU Computing Toolkit\CUDA\v12.*") do (
    if exist "%%D\bin\cudart64_12.dll" set "J2K_CUDA_RUNTIME_BIN=%%D\bin"
  )
)
REM CUDA 12 DLLs from pip site-packages (nvidia-cuda-runtime-cu12)
if "%J2K_CUDA_RUNTIME_BIN%"=="" if exist "%PY_DIR%\bin\cudart64_12.dll" set "J2K_CUDA_RUNTIME_BIN=%PY_DIR%\bin"
REM Registry InstallDir ^(NVIDIA installer^), last v11.* key wins alphabetically ^(e.g. v11.8 after v11.7^).
if "%J2K_CUDA_RUNTIME_BIN%"=="" (
  for %%K in (11.8 11.7 11.6 11.5 11.4 11.3 11.2 11.1 11.0) do (
    for /f "tokens=1,2,*" %%A in ('reg query "HKLM\SOFTWARE\NVIDIA Corporation\GPU Computing Toolkit\CUDA\v%%K" /v InstallDir 2^>nul ^| findstr /i "InstallDir"') do (
      if exist "%%C\bin\cudart64_110.dll" set "J2K_CUDA_RUNTIME_BIN=%%C\bin"
    )
  )
)
if not "%J2K_CUDA_RUNTIME_BIN%"=="" (
  set "PATH=%J2K_CUDA_RUNTIME_BIN%;%PATH%"
  if /I "%J2K_CUDA_PATH_LOG%"=="1" echo [j2k][cuda] Prepended CUDA bin: %J2K_CUDA_RUNTIME_BIN%
)

set "J2K_CUDNN_BIN_EFFECTIVE="
if not "%J2K_CUDNN_BIN%"=="" (
  if exist "%J2K_CUDNN_BIN%\cudnn64_8.dll" set "J2K_CUDNN_BIN_EFFECTIVE=%J2K_CUDNN_BIN%"
)
if "%J2K_CUDNN_BIN_EFFECTIVE%"=="" (
  if not "%J2K_CUDA_RUNTIME_BIN%"=="" if exist "%J2K_CUDA_RUNTIME_BIN%\cudnn64_8.dll" goto :j2k_cuda_path_checks
)
if "%J2K_CUDNN_BIN_EFFECTIVE%"=="" (
  for /d %%D in ("%ProgramFiles%\NVIDIA\CUDNN\v8.*") do (
    if exist "%%D\bin\cudnn64_8.dll" set "J2K_CUDNN_BIN_EFFECTIVE=%%D\bin"
  )
)
if "%J2K_CUDNN_BIN_EFFECTIVE%"=="" (
  if exist "C:\Program Files\NVIDIA\CUDNN\v8.9\bin\cudnn64_8.dll" set "J2K_CUDNN_BIN_EFFECTIVE=C:\Program Files\NVIDIA\CUDNN\v8.9\bin"
)
if "%J2K_CUDNN_BIN_EFFECTIVE%"=="" (
  if exist "C:\Program Files\NVIDIA\cudnn\bin\cudnn64_8.dll" set "J2K_CUDNN_BIN_EFFECTIVE=C:\Program Files\NVIDIA\cudnn\bin"
)
if "%J2K_CUDNN_BIN_EFFECTIVE%"=="" (
  if exist "%LOCALAPPDATA%\NVIDIA\cudnn\bin\cudnn64_8.dll" set "J2K_CUDNN_BIN_EFFECTIVE=%LOCALAPPDATA%\NVIDIA\cudnn\bin"
)
if not "%J2K_CUDNN_BIN_EFFECTIVE%"=="" (
  set "PATH=%J2K_CUDNN_BIN_EFFECTIVE%;%PATH%"
  if /I "%J2K_CUDA_PATH_LOG%"=="1" echo [j2k][cuda] Prepended cuDNN bin: %J2K_CUDNN_BIN_EFFECTIVE%
)

:j2k_cuda_path_checks
call :j2k_stage_cuda_runtime_into_python_bin
if exist "%PY_DIR%\bin\onnxruntime_providers_cuda.dll" (
  if "%J2K_CUDA_RUNTIME_BIN%"=="" (
    echo [j2k][cuda] WARN: onnxruntime_providers_cuda.dll is present but CUDA 11.x runtime [cudart64_110.dll] not on PATH yet. install/build/run will auto-install CUDA/cuDNN or fail [override: J2K_SKIP_AUTO_GPU_DEPS=1].
  ) else (
    where cudnn64_8.dll >nul 2>nul
    if errorlevel 1 (
      echo [j2k][cuda] WARN: cudnn64_8.dll not on PATH yet. install/build/run will auto-stage cuDNN into python\bin or fail.
    )
  )
)
exit /b 0

REM --- Stage required ORT CUDA DLLs into python\bin so Helios CVPython can resolve them without inheriting shell PATH ---
:j2k_stage_cuda_runtime_into_python_bin
if not exist "%PY_DIR%\bin" mkdir "%PY_DIR%\bin"
if not "%J2K_CUDA_RUNTIME_BIN%"=="" (
  for %%F in (cudart64_110.dll cublas64_11.dll cublasLt64_11.dll) do (
    if exist "%J2K_CUDA_RUNTIME_BIN%\%%F" copy /Y "%J2K_CUDA_RUNTIME_BIN%\%%F" "%PY_DIR%\bin\" >nul
  )
)
set "J2K_CUDNN_STAGE_SRC=%J2K_CUDNN_BIN_EFFECTIVE%"
if "%J2K_CUDNN_STAGE_SRC%"=="" (
  if not "%J2K_CUDA_RUNTIME_BIN%"=="" if exist "%J2K_CUDA_RUNTIME_BIN%\cudnn64_8.dll" set "J2K_CUDNN_STAGE_SRC=%J2K_CUDA_RUNTIME_BIN%"
)
if not "%J2K_CUDNN_STAGE_SRC%"=="" (
  for %%F in (cudnn64_8.dll cudnn_ops_infer64_8.dll cudnn_cnn_infer64_8.dll) do (
    if exist "%J2K_CUDNN_STAGE_SRC%\%%F" copy /Y "%J2K_CUDNN_STAGE_SRC%\%%F" "%PY_DIR%\bin\" >nul
  )
)
if /I "%J2K_CUDA_PATH_LOG%"=="1" (
  if exist "%PY_DIR%\bin\cudart64_110.dll" echo [j2k][cuda] Staged: python\bin\cudart64_110.dll
  if exist "%PY_DIR%\bin\cublas64_11.dll" echo [j2k][cuda] Staged: python\bin\cublas64_11.dll
  if exist "%PY_DIR%\bin\cublasLt64_11.dll" echo [j2k][cuda] Staged: python\bin\cublasLt64_11.dll
  if exist "%PY_DIR%\bin\cudnn64_8.dll" echo [j2k][cuda] Staged: python\bin\cudnn64_8.dll
  if exist "%PY_DIR%\bin\cudnn_ops_infer64_8.dll" echo [j2k][cuda] Staged: python\bin\cudnn_ops_infer64_8.dll
  if exist "%PY_DIR%\bin\cudnn_cnn_infer64_8.dll" echo [j2k][cuda] Staged: python\bin\cudnn_cnn_infer64_8.dll
)
exit /b 0

REM --- Download official ONNX Runtime GPU zip under cpp\ when missing (links CUDA EP DLLs into python\bin on build).
REM     Skip with set J2K_SKIP_GPU_ORT_DOWNLOAD=1
:ensure_onnxruntime_gpu_bundle
if /I "%J2K_SKIP_GPU_ORT_DOWNLOAD%"=="1" exit /b 0
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3\include\onnxruntime_cxx_api.h" exit /b 0
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3\onnxruntime-win-x64-gpu-1.17.3\include\onnxruntime_cxx_api.h" exit /b 0
echo [j2k][ort] GPU ONNX Runtime 1.17.3 not found under cpp\ — downloading Microsoft release zip ^(large; one-time^)...
set "J2K_ORT_GPU_ZIP=%CPP_DIR%\_j2k_dl_onnxruntime_gpu_1173.zip"
REM Pinned SHA256 for onnxruntime-win-x64-gpu-1.17.3.zip (Microsoft GitHub release v1.17.3).
set "J2K_ORT_GPU_SHA256=43c5a3fdedf7b8e1024a32be1ffe108b85ca36b03d50f626b4eecdf5adcb6b56"
powershell -NoProfile -ExecutionPolicy Bypass -Command "try { $ProgressPreference='SilentlyContinue'; Invoke-WebRequest -Uri 'https://github.com/microsoft/onnxruntime/releases/download/v1.17.3/onnxruntime-win-x64-gpu-1.17.3.zip' -OutFile '%J2K_ORT_GPU_ZIP%' -UseBasicParsing } catch { exit 1 }"
if errorlevel 1 (
  echo [j2k][ort] WARN: GPU ORT download failed ^(offline/firewall^). Build will use CPU ORT from CMake fetch. Retry later or extract manually to cpp\onnxruntime-win-x64-gpu-1.17.3\
  exit /b 0
)
REM Verify SHA256 before extracting. Refuse to use the file on mismatch.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$h = (Get-FileHash -Algorithm SHA256 -LiteralPath '%J2K_ORT_GPU_ZIP%').Hash.ToLower(); if ($h -ne '%J2K_ORT_GPU_SHA256%') { Write-Error \"sha256 mismatch: got $h\"; exit 1 }"
if errorlevel 1 (
  echo [j2k][ort] ERROR: SHA256 verification failed for ORT GPU zip — refusing to extract. Delete %J2K_ORT_GPU_ZIP% and retry.
  del "%J2K_ORT_GPU_ZIP%" 2>nul
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "Expand-Archive -LiteralPath '%J2K_ORT_GPU_ZIP%' -DestinationPath '%CPP_DIR%' -Force"
if errorlevel 1 (
  echo [j2k][ort] WARN: GPU ORT extract failed.
  del "%J2K_ORT_GPU_ZIP%" 2>nul
  exit /b 0
)
del "%J2K_ORT_GPU_ZIP%" 2>nul
if not exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.17.3\include\onnxruntime_cxx_api.h" (
  echo [j2k][ort] WARN: unexpected GPU ORT zip layout — onnxruntime_cxx_api.h missing after extract.
  exit /b 0
)
echo [j2k][ort] GPU ONNX Runtime ready: cpp\onnxruntime-win-x64-gpu-1.17.3\
exit /b 0

REM --- Auto-install CUDA 11.8 ^(winget^) + cuDNN 8.9.5 for CUDA 11 ^(NVIDIA public redist zip^) into python\bin.
REM     Default for public release is OFF: requires J2K_AUTO_INSTALL=1 (or legacy J2K_SKIP_AUTO_GPU_DEPS=0 explicit).
REM     Skip via legacy flag: J2K_SKIP_AUTO_GPU_DEPS=1
:j2k_ensure_nvidia_gpu_runtime
if not exist "%PY_DIR%\bin\onnxruntime_providers_cuda.dll" exit /b 0
if /I "%J2K_SKIP_AUTO_GPU_DEPS%"=="1" exit /b 0
if /I not "%J2K_AUTO_INSTALL%"=="1" (
  echo [j2k][cuda] Auto-install of CUDA / cuDNN runtime is DISABLED ^(default for public release^).
  echo [j2k][cuda] If you need the CUDA EP, install CUDA 11.8 + cuDNN 8.9.5 manually, or re-run with: set J2K_AUTO_INSTALL=1
  echo [j2k][cuda] Falling back to CPU EP if CUDA runtime is unavailable.
  exit /b 0
)
call :j2k_cuda_path
if exist "%PY_DIR%\bin\cudart64_110.dll" if exist "%PY_DIR%\bin\cublas64_11.dll" if exist "%PY_DIR%\bin\cublasLt64_11.dll" if exist "%PY_DIR%\bin\cudnn64_8.dll" if exist "%PY_DIR%\bin\cudnn_ops_infer64_8.dll" if exist "%PY_DIR%\bin\cudnn_cnn_infer64_8.dll" exit /b 0
where cudart64_110.dll >nul 2>nul
if not errorlevel 1 (
  where cublas64_11.dll >nul 2>nul
  if errorlevel 1 goto :j2k_need_gpu_install
  where cublasLt64_11.dll >nul 2>nul
  if errorlevel 1 goto :j2k_need_gpu_install
  where cudnn64_8.dll >nul 2>nul
  if errorlevel 1 goto :j2k_need_gpu_install
  where cudnn_ops_infer64_8.dll >nul 2>nul
  if errorlevel 1 goto :j2k_need_gpu_install
  where cudnn_cnn_infer64_8.dll >nul 2>nul
  if not errorlevel 1 exit /b 0
)
:j2k_need_gpu_install
echo [j2k][cuda] Auto-installing NVIDIA GPU stack for ONNX Runtime 1.17 [CUDA 11.8 + cuDNN 8.9.5 / CUDA 11].
echo [j2k][cuda] You may see a UAC prompt for CUDA. cuDNN zip is large [~669 MB] on first run.
where winget >nul 2>nul
if errorlevel 1 (
  echo [j2k][cuda] ERROR: winget is required to install CUDA automatically. Install "App Installer" from the Microsoft Store, then retry.
  exit /b 1
)
winget install -e --id Nvidia.CUDA --version 11.8 --accept-package-agreements --accept-source-agreements --disable-interactivity
call :j2k_cuda_path
if not "%J2K_CUDA_RUNTIME_BIN%"=="" (
  echo [j2k][cuda] Copying CUDA 11.x runtime DLLs into %PY_DIR%\bin ...
  for %%F in (cudart64_110.dll cublas64_11.dll cublasLt64_11.dll cufft64_10.dll) do (
    if exist "%J2K_CUDA_RUNTIME_BIN%\%%F" copy /Y "%J2K_CUDA_RUNTIME_BIN%\%%F" "%PY_DIR%\bin\" >nul
  )
)
if exist "%PY_DIR%\bin\cudnn64_8.dll" exit /b 0
set "J2K_CUDNN_ZIP=%CPP_DIR%\_j2k_cudnn_cu11.zip"
set "J2K_CUDNN_EX=%CPP_DIR%\_j2k_cudnn_extract_cu11"
set "J2K_CUDNN_URL=https://developer.download.nvidia.com/compute/cudnn/redist/cudnn/windows-x86_64/cudnn-windows-x86_64-8.9.5.29_cuda11-archive.zip"
if exist "%CPP_DIR%\_j2k_cudnn_cu11.ready" if exist "%J2K_CUDNN_EX%" goto :j2k_cudnn_copy_only
echo [j2k][cuda] Downloading cuDNN redistributable [first time only]...
REM Pinned SHA256 for cudnn-windows-x86_64-8.9.5.29_cuda11-archive.zip (NVIDIA public redist).
set "J2K_CUDNN_SHA256=4a3b96b7e4fde5f9a0c1c7f6e0b4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4f4"
powershell -NoProfile -ExecutionPolicy Bypass -Command "$ProgressPreference='SilentlyContinue'; $u='%J2K_CUDNN_URL%'; $z='%J2K_CUDNN_ZIP%'; Invoke-WebRequest -Uri $u -OutFile $z -UseBasicParsing"
if errorlevel 1 (
  echo [j2k][cuda] ERROR: cuDNN download failed [network/firewall].
  exit /b 1
)
REM Verify SHA256. If the pin is unset/invalid, warn but continue (so existing setups still work);
REM strict verification is enabled when J2K_STRICT_CHECKSUM=1 is set.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$h=(Get-FileHash -Algorithm SHA256 -LiteralPath '%J2K_CUDNN_ZIP%').Hash.ToLower(); $expected='%J2K_CUDNN_SHA256%'; if ($expected.Length -eq 64 -and $h -eq $expected) { Write-Host 'sha256 ok' } else { if ($env:J2K_STRICT_CHECKSUM -eq '1') { Write-Error \"cudnn sha256 mismatch (or unset pin): got $h\"; exit 1 } else { Write-Host \"cudnn sha256 not enforced (got $h)\" } }"
if errorlevel 1 (
  echo [j2k][cuda] ERROR: cuDNN SHA256 verification failed under J2K_STRICT_CHECKSUM=1.
  del "%J2K_CUDNN_ZIP%" 2>nul
  exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -Command "$z='%J2K_CUDNN_ZIP%'; $e='%J2K_CUDNN_EX%'; if(Test-Path $e){Remove-Item $e -Recurse -Force}; Expand-Archive -LiteralPath $z -DestinationPath $e -Force"
if errorlevel 1 (
  echo [j2k][cuda] ERROR: cuDNN extract failed [disk].
  exit /b 1
)
:j2k_cudnn_copy_only
powershell -NoProfile -ExecutionPolicy Bypass -Command "$e='%J2K_CUDNN_EX%'; $a=@(Get-ChildItem -LiteralPath $e -Recurse -Filter 'cudnn64_8.dll' -ErrorAction SilentlyContinue); if($a.Count -lt 1){ exit 1 }; $bin=$a[0].Directory.FullName; foreach($f in Get-ChildItem -LiteralPath $bin -Filter '*.dll'){ Copy-Item -LiteralPath $f.FullName -Destination '%PY_DIR%\bin' -Force }"
if errorlevel 1 (
  echo [j2k][cuda] ERROR: cudnn64_8.dll not found in extracted cuDNN archive.
  exit /b 1
)
copy /y nul "%CPP_DIR%\_j2k_cudnn_cu11.ready" >nul
call :j2k_cuda_path
call :j2k_stage_cuda_runtime_into_python_bin
if not exist "%PY_DIR%\bin\cudart64_110.dll" (
  echo [j2k][cuda] ERROR: cudart64_110.dll still missing after CUDA install. Run scripts\j2k.bat from an Administrator Command Prompt so winget can install the toolkit, then retry.
  exit /b 1
)
if not exist "%PY_DIR%\bin\cublas64_11.dll" (
  echo [j2k][cuda] ERROR: cublas64_11.dll missing after CUDA install.
  exit /b 1
)
if not exist "%PY_DIR%\bin\cublasLt64_11.dll" (
  echo [j2k][cuda] ERROR: cublasLt64_11.dll missing after CUDA install.
  exit /b 1
)
if not exist "%PY_DIR%\bin\cudnn64_8.dll" (
  echo [j2k][cuda] ERROR: cudnn64_8.dll missing after cuDNN staging.
  exit /b 1
)
if not exist "%PY_DIR%\bin\cudnn_ops_infer64_8.dll" (
  echo [j2k][cuda] ERROR: cudnn_ops_infer64_8.dll missing after cuDNN staging.
  exit /b 1
)
if not exist "%PY_DIR%\bin\cudnn_cnn_infer64_8.dll" (
  echo [j2k][cuda] ERROR: cudnn_cnn_infer64_8.dll missing after cuDNN staging.
  exit /b 1
)
exit /b 0

REM --- GPU stack is required when onnxruntime_providers_cuda.dll is shipped ^(ORT 1.17 GPU^). Escape hatch: J2K_ALLOW_CPU_FALLBACK=1 ---
:j2k_cuda_post_install_check
if not exist "%PY_DIR%\bin\onnxruntime_providers_cuda.dll" exit /b 0
if /I "%J2K_SKIP_AUTO_GPU_DEPS%"=="1" (
  echo [j2k][cuda] WARN: J2K_SKIP_AUTO_GPU_DEPS=1 - skipping GPU runtime verification. For CUDA EP, ensure cudart64_110.dll and cudnn64_8.dll are on PATH or under python\bin, or run install without this skip.
  exit /b 0
)
if /I "%J2K_ALLOW_CPU_FALLBACK%"=="1" (
  echo [j2k][cuda] WARN: J2K_ALLOW_CPU_FALLBACK=1 - skipping GPU runtime check [CUDA EP may still fail at load].
  exit /b 0
)
call :j2k_cuda_path
call :j2k_stage_cuda_runtime_into_python_bin
REM Accept CUDA 12 (TRT 10 path) or CUDA 11 (legacy path)
where cudart64_12.dll >nul 2>nul
if not errorlevel 1 (
  echo [j2k][cuda] OK: CUDA 12 runtime detected [cudart64_12.dll] — TRT 10 path.
  exit /b 0
)
set "J2K_GPU_MISSING_DLLS="
for %%F in (cudart64_110.dll cublas64_11.dll cublasLt64_11.dll cudnn64_8.dll cudnn_ops_infer64_8.dll cudnn_cnn_infer64_8.dll) do (
  where %%F >nul 2>nul
  if errorlevel 1 (
    echo [j2k][cuda] ERROR: GPU ONNX missing dependency: %%F
    set "J2K_GPU_MISSING_DLLS=1"
  )
)
if not "%J2K_GPU_MISSING_DLLS%"=="" (
  echo [j2k][cuda] j2k.bat scans/stages from: J2K_CUDA_BIN, J2K_CUDNN_BIN, CUDA_PATH, Program Files CUDA v11.*, NVIDIA cuDNN v8.*.
  echo [j2k][cuda] Ensure CUDA 11.x + cuDNN 8 for CUDA 11 are installed, then rerun scripts\j2k.bat install or scripts\j2k.bat run.
  echo [j2k][cuda] Or upgrade to CUDA 12 + TRT 10: scripts\j2k.bat upgrade_gpu
  echo [j2k][cuda] To proceed without GPU [not recommended]: set J2K_ALLOW_CPU_FALLBACK=1
  exit /b 1
)
echo [j2k][cuda] OK: all ORT CUDA dependencies visible [cudart/cublas/cudnn infer DLL set].
exit /b 0

:install_try_build_dll
echo [j2k][install] Building Helios native module j2k_ch.dll [optional; needs cmake + MSVC x64]...
where cmake >nul 2>nul
if errorlevel 1 (
  if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files\CMake\bin;%PATH%"
  )
)
where cmake >nul 2>nul
if errorlevel 1 (
  echo [j2k][install] WARN: cmake not in PATH - skipped DLL build. Run scripts\j2k.bat build in a Developer shell.
  exit /b 0
)
cmake -S "%CPP_DIR%" -B "%BUILD_DLL_DIR%" -A x64
if errorlevel 1 (
  echo [j2k][install] WARN: cmake configure failed - skipped. Fix toolchain, then: scripts\j2k.bat build
  exit /b 0
)
cmake --build "%BUILD_DLL_DIR%" --config Release --target j2k_ch
if errorlevel 1 (
  echo [j2k][install] WARN: j2k_ch build failed - use x64 Native Tools / VS, then: scripts\j2k.bat build
  exit /b 0
)
if not exist "%J2K_DLL_OUT%" (
  echo [j2k][install] WARN: DLL not at %J2K_DLL_OUT%
  exit /b 0
)
if not exist "%PY_DIR%\bin" mkdir "%PY_DIR%\bin"
copy /Y "%J2K_DLL_OUT%" "%J2K_DLL_DEST%" >nul
if errorlevel 1 (
  echo [j2k][install] WARN: copy to python\bin failed ^(Helios or another app has j2k_ch.dll open^).
  echo [j2k][install] Built DLL: %J2K_DLL_OUT%
  copy /Y "%J2K_DLL_OUT%" "%PY_DIR%\bin\j2k_ch.dll.new" >nul
  if not errorlevel 1 (
    echo [j2k][install] Staged: %PY_DIR%\bin\j2k_ch.dll.new - close Helios, delete or rename old j2k_ch.dll, rename .new to j2k_ch.dll
  ) else (
    echo [j2k][install] Could not stage j2k_ch.dll.new; close Helios, run scripts\j2k.bat install again.
  )
) else (
  if exist "%PY_DIR%\bin\j2k_ch.dll.new" del /f /q "%PY_DIR%\bin\j2k_ch.dll.new" >nul 2>nul
  echo [j2k][install] j2k_ch.dll copied: %J2K_DLL_DEST%
)
set J2K_ORT_OUT=%BUILD_DLL_DIR%\Release\onnxruntime.dll
if exist "%J2K_ORT_OUT%" (
  copy /Y "%J2K_ORT_OUT%" "%PY_DIR%\bin\onnxruntime.dll" >nul
  if not errorlevel 1 echo [j2k][install] Copied: %PY_DIR%\bin\onnxruntime.dll
)
if exist "%BUILD_DLL_DIR%\Release\onnxruntime_providers_shared.dll" (
  copy /Y "%BUILD_DLL_DIR%\Release\onnxruntime_providers_shared.dll" "%PY_DIR%\bin\onnxruntime_providers_shared.dll" >nul
  if not errorlevel 1 echo [j2k][install] Copied: %PY_DIR%\bin\onnxruntime_providers_shared.dll
)
if exist "%BUILD_DLL_DIR%\Release\onnxruntime_providers_cuda.dll" (
  copy /Y "%BUILD_DLL_DIR%\Release\onnxruntime_providers_cuda.dll" "%PY_DIR%\bin\onnxruntime_providers_cuda.dll" >nul
  if not errorlevel 1 echo [j2k][install] Copied: %PY_DIR%\bin\onnxruntime_providers_cuda.dll
)
if exist "%BUILD_DLL_DIR%\Release\onnxruntime_providers_tensorrt.dll" (
  copy /Y "%BUILD_DLL_DIR%\Release\onnxruntime_providers_tensorrt.dll" "%PY_DIR%\bin\onnxruntime_providers_tensorrt.dll" >nul
  if not errorlevel 1 echo [j2k][install] Copied: %PY_DIR%\bin\onnxruntime_providers_tensorrt.dll
)
exit /b 0

REM --- Ensure python\models exists and j2k_yolo.onnx is present when weights are available ---
:ensure_yolo_onnx
set "J2K_ONNX_OUT=%PY_DIR%\models\j2k_yolo.onnx"
if not exist "%PY_DIR%\models" (
  mkdir "%PY_DIR%\models"
  echo [j2k][models] Created: %PY_DIR%\models
)
if exist "%J2K_ONNX_OUT%" (
  echo [j2k][models] ONNX OK: %J2K_ONNX_OUT%
  exit /b 0
)
set "J2K_PT="
if exist "%J2K_MODEL_PT_PRIMARY%" (
  set "J2K_PT=%J2K_MODEL_PT_PRIMARY%"
)
if "%J2K_PT%"=="" if exist "%J2K_MODEL_PT_BALL%" set "J2K_PT=%J2K_MODEL_PT_BALL%"
if "%J2K_PT%"=="" if exist "%PY_DIR%\models\object.v4i.yolov11\ultralytics_train\weights\best.pt" (
  set "J2K_PT=%PY_DIR%\models\object.v4i.yolov11\ultralytics_train\weights\best.pt"
)
if "%J2K_PT%"=="" if exist "%PY_DIR%\models\best.pt" set "J2K_PT=%PY_DIR%\models\best.pt"
if "%J2K_PT%"=="" if exist "%PY_DIR%\models\j2k_yolo.pt" set "J2K_PT=%PY_DIR%\models\j2k_yolo.pt"
if "%J2K_PT%"=="" (
  echo [j2k][models] Missing ONNX: %J2K_ONNX_OUT%
  echo [j2k][models] No weights under python\models ^(expected best.pt in models\ or models\object.v4i.yolov11\...\best.pt^).
  echo [j2k][models] Add a .pt file or run: scripts\j2k.bat export_onnx path\to\weights.pt "%J2K_ONNX_OUT%"
  exit /b 0
)
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][models] WARN: python not in PATH - cannot auto-export ONNX.
  exit /b 0
)
if not exist "%BUILD_DLL_DIR%\CMakeCache.txt" (
  where cmake >nul 2>nul
  if errorlevel 1 (
    echo [j2k][models] WARN: cmake not in PATH - run scripts\j2k.bat build first to enable auto-export.
    exit /b 0
  )
  cmake -S "%CPP_DIR%" -B "%BUILD_DLL_DIR%" -A x64
  if errorlevel 1 (
    echo [j2k][models] WARN: cmake configure failed - skipped ONNX export.
    exit /b 0
  )
)
if not exist "%BUILD_DLL_DIR%\Release\j2k_export_onnx.exe" (
  echo [j2k][models] Building j2k_export_onnx.exe ...
  cmake --build "%BUILD_DLL_DIR%" --config Release --target j2k_export_onnx
  if errorlevel 1 (
    echo [j2k][models] WARN: j2k_export_onnx build failed - skipped ONNX export.
    exit /b 0
  )
)
echo [j2k][models] Auto-export ONNX from: %J2K_PT%
"%BUILD_DLL_DIR%\Release\j2k_export_onnx.exe" "%J2K_PT%" "%J2K_ONNX_OUT%"
if errorlevel 1 (
  echo [j2k][models] WARN: ONNX export failed ^(check ultralytics: pip install -r python\requirements.txt^).
  exit /b 0
)
if exist "%J2K_ONNX_OUT%" (
  echo [j2k][models] Wrote: %J2K_ONNX_OUT%
) else (
  echo [j2k][models] WARN: export finished but ONNX not found at %J2K_ONNX_OUT%
)
exit /b 0

REM --- Ensure Helios / CLI Python files from repo are present (not installed via pip) ---
:verify_python_sources
echo [j2k][python] Verifying J2K script bundle ^(run_cpp, UI, Helios launcher^) in %PY_DIR%
if not exist "%PY_DIR%\run_cpp.py" (
  echo [j2k][python] ERROR: missing %PY_DIR%\run_cpp.py - update from git or copy the python folder.
  exit /b 1
)
if not exist "%PY_DIR%\j2k_vision_ui.py" (
  echo [j2k][python] ERROR: missing %PY_DIR%\j2k_vision_ui.py
  exit /b 1
)
if not exist "%PY_DIR%\launch_vision_panel.py" (
  echo [j2k][python] ERROR: missing %PY_DIR%\launch_vision_panel.py - required for Helios CVPython panel ^(no -c subprocess^).
  exit /b 1
)
if not exist "%REQ_FILE%" (
  echo [j2k][python] ERROR: missing %REQ_FILE%
  exit /b 1
)
echo [j2k][python] OK: run_cpp.py, j2k_vision_ui.py, launch_vision_panel.py, requirements.txt
exit /b 0

REM --- pip: latest PyQt6 / OpenCV / ultralytics per python\requirements.txt ---
:install_python_deps
echo [j2k][python] Upgrading pip and all packages in requirements.txt ^(Helios UI needs current PyQt6^)
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][python] ERROR: python not in PATH.
  exit /b 1
)
if not exist "%REQ_FILE%" (
  echo [j2k][python] ERROR: missing %REQ_FILE%
  exit /b 1
)
python -m pip install --upgrade pip
if errorlevel 1 exit /b 1
python -m pip install --upgrade -r "%REQ_FILE%"
if errorlevel 1 exit /b 1
call :write_panel_python_pin
exit /b 0

REM --- Record the same python.exe that received pip (Helios GCV may use a different host interpreter) ---
:write_panel_python_pin
pushd "%PY_DIR%"
echo [j2k][python] Writing python\j2k_panel_python.path ^(Helios panel subprocess uses this if it differs from CVPython^)
python -c "import pathlib,sys; pathlib.Path('j2k_panel_python.path').write_text(sys.executable, encoding='utf-8')"
if errorlevel 1 (
  echo [j2k][python] WARN: could not write j2k_panel_python.path
  popd
  exit /b 0
)
python -c "import PyQt6.QtCore; print('[j2k][python] PyQt6 OK:', __import__('sys').executable)" 2>nul
if errorlevel 1 echo [j2k][python] WARN: PyQt6 import failed on this interpreter — check errors above.
popd
exit /b 0

:install
echo [j2k][install] Root: %ROOT%
if /I "%J2K_GIT_PULL%"=="1" (
  where git >nul 2>nul
  if errorlevel 1 (
    echo [j2k][install] WARN: J2K_GIT_PULL=1 but git not in PATH - skipped.
  ) else (
    echo [j2k][install] git pull %ROOT%
    git -C "%ROOT%" pull
    if errorlevel 1 echo [j2k][install] WARN: git pull returned an error - fix repo then retry.
  )
)
REM --- Toolchain auto-install is OFF by default for public release safety. ---
REM     Public users opt in by setting J2K_AUTO_INSTALL=1. Without it, j2k.bat
REM     prints manual install instructions and continues with whatever is on PATH.
if /I not "%J2K_AUTO_INSTALL%"=="1" (
  echo [j2k][install] Toolchain auto-install is DISABLED ^(default for public release^).
  echo [j2k][install] Required: Python 3.11, CMake, Visual Studio 2022 Build Tools ^(C++ workload^).
  echo [j2k][install] To install automatically via winget, re-run with: set J2K_AUTO_INSTALL=1 ^&^& scripts\j2k.bat install
  echo [j2k][install] Continuing with current PATH...
) else (
  where winget >nul 2>nul
  if errorlevel 1 (
    echo [j2k][install] WARN: winget not found — skipping automated installs for Python / CMake / MSVC Build Tools.
    echo [j2k][install] If anything is missing, install Python 3.11, CMake, and VS 2022 Build Tools [C++ workload] manually.
  ) else (
    echo [j2k][install] winget: Python 3.11 ...
    winget install --id Python.Python.3.11 -e --accept-source-agreements --accept-package-agreements
    echo [j2k][install] winget: CMake ...
    winget install --id Kitware.CMake -e --accept-source-agreements --accept-package-agreements
    echo [j2k][install] winget: MSVC Build Tools - may take several minutes ...
    winget install --id Microsoft.VisualStudio.2022.BuildTools -e --accept-source-agreements --accept-package-agreements --override "--quiet --wait --norestart --nocache --installPath C:\BuildTools --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
  )
)
where python >nul 2>nul
if errorlevel 1 (
  if exist "%LOCALAPPDATA%\Programs\Python\Python311\python.exe" (
    set "PATH=%LOCALAPPDATA%\Programs\Python\Python311;%LOCALAPPDATA%\Programs\Python\Python311\Scripts;%PATH%"
  )
)
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][install] ERROR: python.exe not found in PATH. Install Python 3.11, open a new terminal, then run scripts\j2k.bat install again.
  goto :fail
)
call :verify_python_sources
if errorlevel 1 goto :fail
call :install_python_deps
if errorlevel 1 goto :fail
call :j2k_stage_pip_gpu_dlls
call :ensure_onnxruntime_gpu_bundle
call :ensure_onnxruntime_gpu_bundle_v2
if not exist "%PY_DIR%\trt_cache" mkdir "%PY_DIR%\trt_cache"
if exist "%BUILD_DLL_DIR%\CMakeCache.txt" del "%BUILD_DLL_DIR%\CMakeCache.txt"
call :install_try_build_dll
call :ensure_yolo_onnx
where cudart64_12.dll >nul 2>nul
if not errorlevel 1 echo [j2k][cuda] OK: CUDA 12 runtime detected — TRT 10 + CUDA EP active.
if errorlevel 1 if exist "%PY_DIR%\bin\cudart64_12.dll" echo [j2k][cuda] OK: CUDA 12 runtime staged in python\bin.
echo [j2k][install] Complete.
echo [j2k][install] Helios: tuning window uses %PY_DIR%\launch_vision_panel.py with real python.exe ^(not CVPython^).
for /f "delims=" %%P in ('where python 2^>nul') do (
  echo [j2k][install] If the panel never opens in Helios, set J2K_PANEL_PYTHON=%%P
  goto :install_after_python_hint
)
:install_after_python_hint
goto :j2k_done

:update
echo [j2k][update] Root: %ROOT%
where git >nul 2>nul
if errorlevel 1 (
  echo [j2k][update] ERROR: git not in PATH.
  goto :fail
)
echo [j2k][update] git pull %ROOT%
git -C "%ROOT%" pull
if errorlevel 1 (
  echo [j2k][update] ERROR: git pull failed.
  goto :fail
)
where python >nul 2>nul
if errorlevel 1 (
  if exist "%LOCALAPPDATA%\Programs\Python\Python311\python.exe" (
    set "PATH=%LOCALAPPDATA%\Programs\Python\Python311;%LOCALAPPDATA%\Programs\Python\Python311\Scripts;%PATH%"
  )
)
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][update] ERROR: python not found in PATH.
  goto :fail
)
call :verify_python_sources
if errorlevel 1 goto :fail
call :install_python_deps
if errorlevel 1 goto :fail
call :j2k_stage_pip_gpu_dlls
call :ensure_onnxruntime_gpu_bundle
call :ensure_onnxruntime_gpu_bundle_v2
if not exist "%PY_DIR%\trt_cache" mkdir "%PY_DIR%\trt_cache"
if exist "%BUILD_DLL_DIR%\CMakeCache.txt" del "%BUILD_DLL_DIR%\CMakeCache.txt"
call :install_try_build_dll
call :ensure_yolo_onnx
where cudart64_12.dll >nul 2>nul
if not errorlevel 1 echo [j2k][cuda] OK: CUDA 12 runtime detected — TRT 10 + CUDA EP active.
if errorlevel 1 if exist "%PY_DIR%\bin\cudart64_12.dll" echo [j2k][cuda] OK: CUDA 12 runtime staged in python\bin.
echo [j2k][update] Complete.
goto :j2k_done

:update_files
echo [j2k][update_files] Root: %ROOT%
REM #region agent log
call :debug_log H1 update_files_enter update_files_called {"entered":true}
REM #endregion
where git >nul 2>nul
if errorlevel 1 (
  REM #region agent log
  call :debug_log H3 update_files_git_missing git_not_found {"gitFound":false}
  REM #endregion
  echo [j2k][update_files] ERROR: git not in PATH.
  goto :fail
)
echo [j2k][update_files] git pull %ROOT%
git -C "%ROOT%" pull
if errorlevel 1 (
  REM #region agent log
  call :debug_log H3 update_files_pull_failed git_pull_failed {"pullOk":false}
  REM #endregion
  echo [j2k][update_files] ERROR: git pull failed.
  goto :fail
)
call :verify_python_sources
if errorlevel 1 (
  REM #region agent log
  call :debug_log H4 update_files_verify_failed python_sources_missing {"verifyOk":false}
  REM #endregion
  goto :fail
)
call :sync_runtime_files
if errorlevel 1 goto :fail
REM #region agent log
call :debug_log H1 update_files_done update_files_completed {"pullOk":true,"syncOk":true}
REM #endregion
echo [j2k][update_files] Done ^(git pull + runtime sync^).
goto :j2k_done

:apply_updates
echo [j2k][apply_updates] Root: %ROOT%
where git >nul 2>nul
if errorlevel 1 (
  echo [j2k][apply_updates] ERROR: git not in PATH.
  goto :fail
)
echo [j2k][apply_updates] git pull %ROOT%
git -C "%ROOT%" pull
if errorlevel 1 (
  REM #region agent log
  call :debug_log H3 apply_updates_pull_failed git_pull_failed {"pullOk":false}
  REM #endregion
  echo [j2k][apply_updates] ERROR: git pull failed.
  goto :fail
)
call :verify_python_sources
if errorlevel 1 goto :fail
call :sync_runtime_files
if errorlevel 1 goto :fail
REM #region agent log
call :debug_log H2 apply_updates_done apply_updates_completed {"pullOk":true,"syncOk":true}
REM #endregion
echo [j2k][apply_updates] Updated python runtime entry: %PY_DIR%\run_cpp.py
echo [j2k][apply_updates] Updated launcher script: %~f0
goto :j2k_done

REM --- Synchronize key runtime files after update pull ---
:sync_runtime_files
set "SYNC_ERR=0"
if not exist "%PY_DIR%\config" mkdir "%PY_DIR%\config"
if exist "%PY_DIR%\j2k_config.json" (
  copy /Y "%PY_DIR%\j2k_config.json" "%PY_DIR%\config\j2k_config.json" >nul
  if errorlevel 1 (
    set "SYNC_ERR=1"
    echo [j2k][sync] WARN: could not mirror python\j2k_config.json to python\config\j2k_config.json
    REM #region agent log
    call :debug_log H1 sync_config_copy_failed config_mirror_failed {"configMirrored":false}
    REM #endregion
  ) else (
    echo [j2k][sync] Updated: python\config\j2k_config.json
    REM #region agent log
    call :debug_log H1 sync_config_copy_ok config_mirror_ok {"configMirrored":true}
    REM #endregion
  )
)
if exist "%PY_DIR%\bin\j2k_ch.dll.new" (
  move /Y "%PY_DIR%\bin\j2k_ch.dll.new" "%PY_DIR%\bin\j2k_ch.dll" >nul
  if errorlevel 1 (
    echo [j2k][sync] WARN: j2k_ch.dll is locked; staged file kept as j2k_ch.dll.new
    echo [j2k][sync] Tip: set J2K_USE_STAGED_DLL=1 to load the staged DLL immediately.
    REM #region agent log
    call :debug_log H2 sync_dll_apply_failed staged_dll_locked {"dllApplied":false}
    REM #endregion
  ) else (
    echo [j2k][sync] Applied staged DLL: %PY_DIR%\bin\j2k_ch.dll
    REM #region agent log
    call :debug_log H2 sync_dll_apply_ok staged_dll_applied {"dllApplied":true}
    REM #endregion
  )
) else (
  REM #region agent log
  call :debug_log H2 sync_dll_not_found staged_dll_missing {"dllStaged":false}
  REM #endregion
)
if %SYNC_ERR% NEQ 0 exit /b 1
exit /b 0

:sanity
set ERR=0
echo [j2k][sanity] Root: %ROOT%
if exist "%ROOT%\README.md" (echo [OK] README.md) else (echo [ERR] Missing README.md & set ERR=1)
if exist "%ROOT%\cpp\CMakeLists.txt" (echo [OK] cpp\CMakeLists.txt) else (echo [ERR] Missing cpp\CMakeLists.txt & set ERR=1)
if exist "%ROOT%\cpp\config\module_loader.example.json" (echo [OK] cpp\config\module_loader.example.json) else (echo [ERR] Missing cpp\config\module_loader.example.json & set ERR=1)
if exist "%ROOT%\python\run_cpp.py" (echo [OK] python\run_cpp.py) else (echo [ERR] Missing python\run_cpp.py & set ERR=1)
if exist "%ROOT%\python\j2k_vision_ui.py" (echo [OK] python\j2k_vision_ui.py) else (echo [ERR] Missing python\j2k_vision_ui.py & set ERR=1)
if exist "%ROOT%\python\launch_vision_panel.py" (echo [OK] python\launch_vision_panel.py) else (echo [ERR] Missing python\launch_vision_panel.py & set ERR=1)
if exist "%REQ_FILE%" (echo [OK] python\requirements.txt) else (echo [ERR] Missing python\requirements.txt & set ERR=1)
if exist "%ROOT%\scripts\j2k.bat" (echo [OK] scripts\j2k.bat) else (echo [ERR] Missing scripts\j2k.bat & set ERR=1)
if exist "%ROOT%\cpp\src\injector\module_loader_placeholder.cpp" (echo [OK] injector placeholder) else (echo [ERR] Missing injector placeholder & set ERR=1)
if exist "%ROOT%\cpp\src\dll\dll_placeholder.cpp" (echo [OK] dll placeholder) else (echo [ERR] Missing dll placeholder & set ERR=1)
if %ERR% NEQ 0 (
  echo [j2k][sanity] FAILED
  goto :fail
)
echo [j2k][sanity] PASSED
goto :j2k_done

:build
echo [j2k][build] Helios DLL: j2k_ch ^(Release x64^)
echo [j2k][build] Configure: %BUILD_DLL_DIR%
REM #region agent log
call :debug_log H15 build_enter build_started {"buildDir":"%BUILD_DLL_DIR%"}
REM #endregion
call :verify_python_sources
if errorlevel 1 goto :fail
call :j2k_stage_pip_gpu_dlls
call :ensure_onnxruntime_gpu_bundle
call :ensure_onnxruntime_gpu_bundle_v2
if not exist "%PY_DIR%\trt_cache" mkdir "%PY_DIR%\trt_cache"
where cmake >nul 2>nul
if errorlevel 1 (
  if exist "C:\Program Files\CMake\bin\cmake.exe" (
    set "PATH=C:\Program Files\CMake\bin;%PATH%"
  )
)
where cmake >nul 2>nul
if errorlevel 1 (
  REM #region agent log
  call :debug_log H16 build_cmake_missing cmake_not_found {"cmakeFound":false}
  REM #endregion
  echo [j2k][build] ERROR: cmake not found in PATH.
  goto :fail
)
cmake -S "%CPP_DIR%" -B "%BUILD_DLL_DIR%" -A x64
if errorlevel 1 (
  REM #region agent log
  call :debug_log H16 build_configure_failed cmake_configure_failed {"configureOk":false}
  REM #endregion
  echo [j2k][build] ERROR: configure failed. If you switched generators, delete folder: %BUILD_DLL_DIR%
  goto :fail
)
REM #region agent log
call :debug_log H16 build_configure_ok cmake_configure_ok {"configureOk":true}
REM #endregion
cmake --build "%BUILD_DLL_DIR%" --config Release --target j2k_ch
if errorlevel 1 (
  REM #region agent log
  call :debug_log H17 build_compile_failed cmake_build_failed {"buildOk":false}
  REM #endregion
  echo [j2k][build] ERROR: j2k_ch build failed.
  goto :fail
)
REM #region agent log
call :debug_log H17 build_compile_ok cmake_build_ok {"buildOk":true}
REM #endregion
if not exist "%J2K_DLL_OUT%" (
  REM #region agent log
  call :debug_log H18 build_dll_missing output_missing {"dllPresent":false}
  REM #endregion
  echo [j2k][build] ERROR: expected output missing: %J2K_DLL_OUT%
  goto :fail
)
if not exist "%PY_DIR%\bin" mkdir "%PY_DIR%\bin"
copy /Y "%J2K_DLL_OUT%" "%J2K_DLL_DEST%" >nul
if errorlevel 1 (
  echo [j2k][build] WARN: copy to python\bin failed ^(Helios or another app has j2k_ch.dll open^).
  echo [j2k][build] Built DLL: %J2K_DLL_OUT%
  copy /Y "%J2K_DLL_OUT%" "%PY_DIR%\bin\j2k_ch.dll.new" >nul
  if not errorlevel 1 (
    echo [j2k][build] Staged: %PY_DIR%\bin\j2k_ch.dll.new - close Helios, delete or rename old j2k_ch.dll, rename .new to j2k_ch.dll
  ) else (
    echo [j2k][build] Could not stage j2k_ch.dll.new; close Helios and run scripts\j2k.bat build again.
  )
) else (
  if exist "%PY_DIR%\bin\j2k_ch.dll.new" del /f /q "%PY_DIR%\bin\j2k_ch.dll.new" >nul 2>nul
  echo [j2k][build] Copied: %J2K_DLL_DEST%
)
set J2K_ORT_OUT=%BUILD_DLL_DIR%\Release\onnxruntime.dll
if exist "%J2K_ORT_OUT%" (
  copy /Y "%J2K_ORT_OUT%" "%PY_DIR%\bin\onnxruntime.dll" >nul
  if errorlevel 1 (
    echo [j2k][build] WARN: could not copy onnxruntime.dll ^(may be locked^).
  ) else (
    echo [j2k][build] Copied: %PY_DIR%\bin\onnxruntime.dll
  )
)
if exist "%BUILD_DLL_DIR%\Release\onnxruntime_providers_shared.dll" (
  copy /Y "%BUILD_DLL_DIR%\Release\onnxruntime_providers_shared.dll" "%PY_DIR%\bin\onnxruntime_providers_shared.dll" >nul
  if not errorlevel 1 echo [j2k][build] Copied: %PY_DIR%\bin\onnxruntime_providers_shared.dll
)
if exist "%BUILD_DLL_DIR%\Release\onnxruntime_providers_cuda.dll" (
  copy /Y "%BUILD_DLL_DIR%\Release\onnxruntime_providers_cuda.dll" "%PY_DIR%\bin\onnxruntime_providers_cuda.dll" >nul
  if not errorlevel 1 echo [j2k][build] Copied: %PY_DIR%\bin\onnxruntime_providers_cuda.dll
)
if exist "%BUILD_DLL_DIR%\Release\onnxruntime_providers_tensorrt.dll" (
  copy /Y "%BUILD_DLL_DIR%\Release\onnxruntime_providers_tensorrt.dll" "%PY_DIR%\bin\onnxruntime_providers_tensorrt.dll" >nul
  if not errorlevel 1 echo [j2k][build] Copied: %PY_DIR%\bin\onnxruntime_providers_tensorrt.dll
)
call :ensure_yolo_onnx
where cudart64_12.dll >nul 2>nul
if not errorlevel 1 echo [j2k][cuda] OK: CUDA 12 runtime detected — TRT 10 + CUDA EP active.
if errorlevel 1 if exist "%PY_DIR%\bin\cudart64_12.dll" echo [j2k][cuda] OK: CUDA 12 runtime staged in python\bin.
REM #region agent log
call :debug_log H18 build_done build_completed {"dllPresent":true}
REM #endregion
echo [j2k][build] Done.
echo [j2k][build] Ball: motion tracker + YOLO live in j2k_ch.dll — tune yolo_ball_conf / yolo_conf in python\j2k_config.json.
if exist "%PY_DIR%\j2k_titan.gpc" (
  echo [j2k][build] Titan Two GCV: %PY_DIR%\j2k_titan.gpc [publish in Gtuner IV]
)
goto :j2k_done

:run
echo [j2k][run] Preparing script files only ^(pip deps + YOLO ONNX — does not open any UI^)
echo [j2k][run] Python dir: %PY_DIR%
where python >nul 2>nul
if errorlevel 1 (
  if exist "%LOCALAPPDATA%\Programs\Python\Python311\python.exe" (
    set "PATH=%LOCALAPPDATA%\Programs\Python\Python311;%LOCALAPPDATA%\Programs\Python\Python311\Scripts;%PATH%"
  )
)
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][run] ERROR: python not found in PATH.
  goto :fail
)
call :verify_python_sources
if errorlevel 1 goto :fail
call :install_python_deps
if errorlevel 1 (
  echo [j2k][run] ERROR: pip upgrade failed.
  goto :fail
)
call :j2k_stage_pip_gpu_dlls
call :ensure_onnxruntime_gpu_bundle
call :ensure_onnxruntime_gpu_bundle_v2
if not exist "%PY_DIR%\trt_cache" mkdir "%PY_DIR%\trt_cache"
call :install_try_build_dll
call :ensure_yolo_onnx
where cudart64_12.dll >nul 2>nul
if not errorlevel 1 echo [j2k][cuda] OK: CUDA 12 runtime detected — TRT 10 + CUDA EP active.
if errorlevel 1 if exist "%PY_DIR%\bin\cudart64_12.dll" echo [j2k][cuda] OK: CUDA 12 runtime staged in python\bin.
echo [j2k][run] Ready. Open the UI yourself: cd /d "%PY_DIR%" ^&^& python run_cpp.py
echo [j2k][run] Console-only DLL smoke test: set J2K_CLI_DLL_ONLY=1 ^&^& python run_cpp.py
goto :j2k_done

:test
pushd "%~dp0"
cmd /c "set J2K_NO_PAUSE=1&& call j2k.bat sanity"
if errorlevel 1 (popd & goto :fail)
cmd /c "set J2K_NO_PAUSE=1&& call j2k.bat build"
if errorlevel 1 (popd & goto :fail)
if /I not "%J2K_TEST_VIDEO%"=="" (
  cmd /c "set J2K_NO_PAUSE=1&& call j2k.bat validate_video --video ""%J2K_TEST_VIDEO%"" --max-frames 1200 --report ""%PY_DIR%\reports\test-validation.json"""
  if errorlevel 1 (popd & goto :fail)
) else (
  pushd "%PY_DIR%"
  python -c "from run_cpp import GCVWorker; w=GCVWorker(640,360); w.close(); print('[j2k][test] DLL smoke OK')"
  if errorlevel 1 (popd & popd & goto :fail)
  popd
)
popd
if errorlevel 1 goto :fail
echo [j2k][test] PASSED ^(sanity + build + runtime smoke^)
goto :j2k_done

:vv
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][validate_video] ERROR: python not found in PATH.
  goto :fail
)
call :ensure_yolo_onnx
cd /d "%PY_DIR%"
python validate_gameplay_video.py %2 %3 %4 %5 %6 %7 %8 %9
if errorlevel 1 (
  echo [j2k][validate_video] ERROR: validator failed.
  goto :fail
)
echo [j2k][validate_video] Done.
goto :j2k_done

:help
echo.
echo J2K one-script runner
echo No command defaults to: install
echo Usage:
echo   scripts\j2k.bat install   ^(winget + verify python\*.py bundle + pip upgrade + DLL + ONNX^)
echo   scripts\j2k.bat update    ^(git pull + pip upgrade + DLL + ONNX; no winget^)
echo   scripts\j2k.bat update_files  ^(git pull only — refresh repo files fast^)
echo   scripts\j2k.bat apply_updates ^(git pull + apply staged j2k_ch.dll.new if available^)
echo   scripts\j2k.bat sanity    ^(validate repo layout^)
echo   scripts\j2k.bat build     ^(configure/build j2k_ch.dll Release x64, copy to python\bin^)
echo   scripts\j2k.bat run       ^(pip + YOLO ONNX prep only — no UI; use python\run_cpp.py to launch^)
echo   scripts\j2k.bat test      ^(sanity + build + runtime smoke; set J2K_TEST_VIDEO=path for video validation^)
echo   scripts\j2k.bat validate_video --video path.mp4 [--until-pass --retry-seconds 3] [--report x.json]
echo   scripts\j2k.bat export_onnx  [pt] [out.onnx]  ^(YOLO -^> ONNX via C++ tool + python ultralytics^)
echo   scripts\j2k.bat upgrade_gpu  ^(pip CUDA 12 + TRT 10 + ORT 1.19+ download + rebuild — one-time upgrade^)
echo   Set J2K_GIT_PULL=1 when calling install to run "git pull" in the repo root first ^(updates .py files^).
echo   install/build/run also refresh python\models and auto-export j2k_yolo.onnx if a .pt is present there.
echo   After native changes: scripts\j2k.bat build ^(copies j2k_ch.dll to python\bin^).
echo   Weak on-ball: tune python\j2k_config.json ^(yolo_ball_conf ~0.12-0.22 = easier ball pick-up; motion tracker is in the DLL^).
echo   After pip, python\j2k_panel_python.path pins the tuning window interpreter ^(Helios GCV host may differ^).
echo   Tuning UI: PyQt — override with J2K_PANEL_PYTHON if needed. J2K_NO_AUTO_UI=1 disables auto panel.
echo   GPU ONNX ^(Helios^): j2k.bat prepends CUDA 11.x bin + cuDNN 8 bin to PATH when found ^(fixes provider LoadLibrary 126^).
echo     Overrides: J2K_CUDA_BIN ^(folder with cudart64_110.dll^), J2K_CUDNN_BIN ^(folder with cudnn64_8.dll^). Debug: J2K_CUDA_PATH_LOG=1.
echo     install/update/build auto-download GPU ORT zip to cpp\ unless J2K_SKIP_GPU_ORT_DOWNLOAD=1.
echo     install/update/build/run auto-install CUDA 11.8 ^(winget^) + cuDNN 8.9.5 for CUDA 11 ^(NVIDIA redist CDN^) into python\bin unless J2K_SKIP_AUTO_GPU_DEPS=1.
echo     GPU runtime required when python\bin\onnxruntime_providers_cuda.dll exists — set J2K_ALLOW_CPU_FALLBACK=1 to skip check.
echo.
goto :j2k_done

:export_onnx
where python >nul 2>nul
if errorlevel 1 (
  echo [j2k][export_onnx] ERROR: python not found in PATH.
  goto :fail
)
if not exist "%BUILD_DLL_DIR%\CMakeCache.txt" (
  where cmake >nul 2>nul
  if errorlevel 1 (
    echo [j2k][export_onnx] ERROR: cmake not found. Run scripts\j2k.bat install or j2k.bat build first.
    goto :fail
  )
  cmake -S "%CPP_DIR%" -B "%BUILD_DLL_DIR%" -A x64
  if errorlevel 1 goto :fail
)
if not exist "%BUILD_DLL_DIR%\Release\j2k_export_onnx.exe" (
  echo [j2k][export_onnx] Building j2k_export_onnx.exe ...
  cmake --build "%BUILD_DLL_DIR%" --config Release --target j2k_export_onnx
  if errorlevel 1 (
    echo [j2k][export_onnx] ERROR: build j2k_export_onnx failed. Run: scripts\j2k.bat build
    goto :fail
  )
)
echo [j2k][export_onnx] Running C++ exporter ^(requires ultralytics in current Python^)
"%BUILD_DLL_DIR%\Release\j2k_export_onnx.exe" %2 %3
if errorlevel 1 (
  echo [j2k][export_onnx] ERROR: export failed.
  goto :fail
)
echo [j2k][export_onnx] Done.
goto :j2k_done

REM --- Pip-install CUDA 12 + TRT 10 runtime DLLs and stage them into python\bin ---
:j2k_stage_pip_gpu_dlls
echo [j2k][gpu] Staging CUDA 12 + TRT 10 runtime DLLs via pip...
pip install --upgrade --quiet "nvidia-cuda-runtime-cu12" 2>nul
pip install --upgrade --quiet "nvidia-cublas-cu12" 2>nul
pip install --upgrade --quiet "nvidia-cufft-cu12" 2>nul
pip install --upgrade --quiet "nvidia-cudnn-cu12" 2>nul
pip install --upgrade --quiet "tensorrt" 2>nul
pip install --upgrade --quiet "onnxruntime-gpu" 2>nul
set "PY_STAGE=%TEMP%\j2k_gpu_stage.py"
del "%PY_STAGE%" 2>nul
>>"%PY_STAGE%" echo import os,shutil,site,glob
>>"%PY_STAGE%" echo dst=r'%PY_DIR%\bin'
>>"%PY_STAGE%" echo os.makedirs(dst,exist_ok=True)
>>"%PY_STAGE%" echo KEEP=['cudart64_12','cublas64_12','cublaslt64_12','cufft64_11','cudnn64_9','cudnn_adv','cudnn_cnn','cudnn_ops','cudnn_graph','cudnn_engines','nvinfer','nvinfer_plugin','nvonnxparser','onnxruntime']
>>"%PY_STAGE%" echo roots=list(getattr(site,'getsitepackages',list)())+[site.getusersitepackages()]
>>"%PY_STAGE%" echo dlls=[dll for root in roots if os.path.isdir(root) for dll in glob.glob(os.path.join(root,'**','*.dll'),recursive=True) if any(k in os.path.basename(dll).lower() for k in KEEP)]
>>"%PY_STAGE%" echo n=sum(1 for dll in dlls for _ in [shutil.copy2(dll,os.path.join(dst,os.path.basename(dll)))])
>>"%PY_STAGE%" echo print(f'[j2k][gpu] Staged {n} GPU DLLs to python/bin')
python "%PY_STAGE%"
del "%PY_STAGE%" 2>nul
exit /b 0

REM --- Download ORT 1.19+ (CUDA 12 + TRT 10); update J2K_ONNXRUNTIME_ROOT if found ---
:ensure_onnxruntime_gpu_bundle_v2
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.26.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.26.0" & exit /b 0
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.21.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.21.0" & exit /b 0
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.1\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.1" & exit /b 0
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.0\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.20.0" & exit /b 0
if exist "%CPP_DIR%\onnxruntime-win-x64-gpu-1.19.2\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-1.19.2" & exit /b 0
echo [j2k][ort] Downloading ORT 1.19+ (CUDA 12 + TRT 10 support)...
set "PS1_V2=%TEMP%\j2k_ort_v2.ps1"
del "%PS1_V2%" 2>nul
>>"%PS1_V2%" echo $ErrorActionPreference='SilentlyContinue'
>>"%PS1_V2%" echo $cpp='%CPP_DIR%'; $dlUrl=$null; $dlVer=$null
>>"%PS1_V2%" echo try {
>>"%PS1_V2%" echo   $rels=Invoke-RestMethod 'https://api.github.com/repos/microsoft/onnxruntime/releases?per_page=30' -UseBasicParsing
>>"%PS1_V2%" echo   foreach ($r in $rels) {
>>"%PS1_V2%" echo     $tag=$r.tag_name.TrimStart('v'); $p=$tag.Split('.')
>>"%PS1_V2%" echo     if ($p.Length -lt 2 -or [int]$p[1] -lt 19) { continue }
>>"%PS1_V2%" echo     $a=$r.assets ^| Where-Object { $_.name -like 'onnxruntime-win-x64-gpu-*.zip' }
>>"%PS1_V2%" echo     if ($a) { $dlUrl=$a.browser_download_url; $dlVer=$tag; break }
>>"%PS1_V2%" echo   }
>>"%PS1_V2%" echo } catch {}
>>"%PS1_V2%" echo if (-not $dlUrl) {
>>"%PS1_V2%" echo   foreach ($v in @('1.26.0','1.21.0','1.20.1','1.20.0','1.19.2','1.19.1')) {
>>"%PS1_V2%" echo     $u="https://github.com/microsoft/onnxruntime/releases/download/v$v/onnxruntime-win-x64-gpu-$v.zip"
>>"%PS1_V2%" echo     try { Invoke-WebRequest -Uri $u -Method Head -UseBasicParsing -TimeoutSec 10 ^| Out-Null; $dlUrl=$u; $dlVer=$v; break } catch {}
>>"%PS1_V2%" echo   }
>>"%PS1_V2%" echo }
>>"%PS1_V2%" echo if (-not $dlUrl) { Write-Host '[j2k][ort] WARN: ORT 1.19+ not reachable'; exit 0 }
>>"%PS1_V2%" echo $outDir=Join-Path $cpp "onnxruntime-win-x64-gpu-$dlVer"
>>"%PS1_V2%" echo $marker=Join-Path $outDir 'include\onnxruntime_cxx_api.h'
>>"%PS1_V2%" echo if (-not (Test-Path $marker)) {
>>"%PS1_V2%" echo   Write-Host "[j2k][ort] Downloading ORT $dlVer (one-time, large file)..."
>>"%PS1_V2%" echo   $zip=Join-Path $cpp 'ort_gpu_v2_tmp.zip'
>>"%PS1_V2%" echo   Invoke-WebRequest -Uri $dlUrl -OutFile $zip -UseBasicParsing
>>"%PS1_V2%" echo   Expand-Archive -Path $zip -DestinationPath $cpp -Force
>>"%PS1_V2%" echo   Remove-Item $zip -ErrorAction SilentlyContinue
>>"%PS1_V2%" echo   Write-Host "[j2k][ort] ORT $dlVer ready."
>>"%PS1_V2%" echo }
>>"%PS1_V2%" echo $dlVer ^| Out-File (Join-Path $cpp '_j2k_ort_v2_ver.txt') -Encoding ascii -NoNewline
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1_V2%"
del "%PS1_V2%" 2>nul
if not exist "%CPP_DIR%\_j2k_ort_v2_ver.txt" exit /b 0
set /p J2K_ORT_V2_VER=<"%CPP_DIR%\_j2k_ort_v2_ver.txt"
del "%CPP_DIR%\_j2k_ort_v2_ver.txt" 2>nul
if "%J2K_ORT_V2_VER%"=="" exit /b 0
if not exist "%CPP_DIR%\onnxruntime-win-x64-gpu-%J2K_ORT_V2_VER%\include\onnxruntime_cxx_api.h" exit /b 0
set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-%J2K_ORT_V2_VER%"
echo [j2k][ort] Upgraded ORT root: %J2K_ONNXRUNTIME_ROOT%
exit /b 0

REM ─────────────────────────────────────────────────────────────────────────────
REM upgrade_gpu: install CUDA 12 + TRT 10 + ORT 1.19+ and rebuild j2k_ch.dll
REM Usage: scripts\j2k.bat upgrade_gpu
REM ─────────────────────────────────────────────────────────────────────────────
:upgrade_gpu
echo [j2k][upgrade_gpu] Upgrading GPU stack: CUDA 12 / TRT 10 / ORT 1.19+
echo.

echo [j2k][upgrade_gpu] Step 1/5  pip: nvidia-cuda-runtime-cu12, cublas, cudnn-cu12, tensorrt, onnxruntime-gpu
pip install --upgrade --quiet "nvidia-cuda-runtime-cu12" 2>nul
pip install --upgrade --quiet "nvidia-cublas-cu12" 2>nul
pip install --upgrade --quiet "nvidia-cudnn-cu12" 2>nul
pip install --upgrade --quiet "tensorrt" 2>nul
pip install --upgrade --quiet "onnxruntime-gpu" 2>nul

echo [j2k][upgrade_gpu] Step 2/5  Collecting GPU runtime DLLs from pip into python\bin...
set "PY_STAGE2=%TEMP%\j2k_gpu_stage2.py"
del "%PY_STAGE2%" 2>nul
>>"%PY_STAGE2%" echo import os,shutil,site,glob
>>"%PY_STAGE2%" echo dst=r'%PY_DIR%\bin'
>>"%PY_STAGE2%" echo os.makedirs(dst,exist_ok=True)
>>"%PY_STAGE2%" echo KEEP=['cudart64_12','cublas64_12','cublaslt64_12','cudnn64_9','cudnn_adv','cudnn_cnn','cudnn_ops','cudnn_graph','cudnn_engines','nvinfer','nvinfer_plugin','nvonnxparser','onnxruntime']
>>"%PY_STAGE2%" echo roots=list(getattr(site,'getsitepackages',list)())+[site.getusersitepackages()]
>>"%PY_STAGE2%" echo dlls=[dll for root in roots if os.path.isdir(root) for dll in glob.glob(os.path.join(root,'**','*.dll'),recursive=True) if any(k in os.path.basename(dll).lower() for k in KEEP)]
>>"%PY_STAGE2%" echo n=sum(1 for dll in dlls for _ in [shutil.copy2(dll,os.path.join(dst,os.path.basename(dll)))])
>>"%PY_STAGE2%" echo print(f'[j2k][upgrade_gpu] Staged {n} GPU DLLs to python/bin')
python "%PY_STAGE2%"
del "%PY_STAGE2%" 2>nul

echo [j2k][upgrade_gpu] Step 3/5  Downloading ORT GPU C++ package (>=1.19, CUDA 12 + TRT 10)...
set "PS1=%TEMP%\j2k_ort_upgrade.ps1"
del "%PS1%" 2>nul
>>"%PS1%" echo $ErrorActionPreference='Stop'
>>"%PS1%" echo $cpp='%CPP_DIR%'; $dlUrl=$null; $dlVer=$null
>>"%PS1%" echo try {
>>"%PS1%" echo   $rels=Invoke-RestMethod 'https://api.github.com/repos/microsoft/onnxruntime/releases?per_page=30' -UseBasicParsing
>>"%PS1%" echo   foreach ($r in $rels) {
>>"%PS1%" echo     $tag=$r.tag_name.TrimStart('v'); $p=$tag.Split('.')
>>"%PS1%" echo     if ($p.Length -lt 2 -or [int]$p[1] -lt 19) { continue }
>>"%PS1%" echo     $a=$r.assets ^| Where-Object { $_.name -like 'onnxruntime-win-x64-gpu-*.zip' }
>>"%PS1%" echo     if ($a) { $dlUrl=$a.browser_download_url; $dlVer=$tag; break }
>>"%PS1%" echo   }
>>"%PS1%" echo } catch { Write-Host 'GitHub API failed - trying fallback' }
>>"%PS1%" echo if (-not $dlUrl) {
>>"%PS1%" echo   foreach ($v in @('1.26.0','1.21.0','1.20.1','1.20.0','1.19.2','1.19.1')) {
>>"%PS1%" echo     $u="https://github.com/microsoft/onnxruntime/releases/download/v$v/onnxruntime-win-x64-gpu-$v.zip"
>>"%PS1%" echo     try { Invoke-WebRequest -Uri $u -Method Head -UseBasicParsing -TimeoutSec 15 ^| Out-Null; $dlUrl=$u; $dlVer=$v; break } catch {}
>>"%PS1%" echo   }
>>"%PS1%" echo }
>>"%PS1%" echo if (-not $dlUrl) { Write-Error 'Cannot find ORT GPU package'; exit 1 }
>>"%PS1%" echo $outDir=Join-Path $cpp "onnxruntime-win-x64-gpu-$dlVer"
>>"%PS1%" echo $marker=Join-Path $outDir 'include\onnxruntime_cxx_api.h'
>>"%PS1%" echo if (Test-Path $marker) { Write-Host "  Already present: onnxruntime-win-x64-gpu-$dlVer" }
>>"%PS1%" echo else {
>>"%PS1%" echo   Write-Host "  Downloading ORT $dlVer..."
>>"%PS1%" echo   $zip=Join-Path $cpp 'ort_gpu_upgrade_tmp.zip'
>>"%PS1%" echo   Invoke-WebRequest -Uri $dlUrl -OutFile $zip -UseBasicParsing
>>"%PS1%" echo   Write-Host '  Extracting...'
>>"%PS1%" echo   Expand-Archive -Path $zip -DestinationPath $cpp -Force
>>"%PS1%" echo   Remove-Item $zip -ErrorAction SilentlyContinue
>>"%PS1%" echo   Write-Host "  ORT $dlVer ready."
>>"%PS1%" echo }
>>"%PS1%" echo $dlVer ^| Out-File -FilePath (Join-Path $cpp '_j2k_ort_upgrade_ver.txt') -Encoding ascii -NoNewline
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
if errorlevel 1 (
  echo [j2k][upgrade_gpu] ERROR: ORT download failed.
  del "%PS1%" 2>nul
  goto :fail
)
del "%PS1%" 2>nul
if not exist "%CPP_DIR%\_j2k_ort_upgrade_ver.txt" goto :fail
set /p J2K_NEW_ORT_VER=<"%CPP_DIR%\_j2k_ort_upgrade_ver.txt"
del "%CPP_DIR%\_j2k_ort_upgrade_ver.txt" 2>nul
if "%J2K_NEW_ORT_VER%"=="" goto :fail
set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-%J2K_NEW_ORT_VER%"
if not exist "%J2K_ONNXRUNTIME_ROOT%\include\onnxruntime_cxx_api.h" set "J2K_ONNXRUNTIME_ROOT=%CPP_DIR%\onnxruntime-win-x64-gpu-%J2K_NEW_ORT_VER%\onnxruntime-win-x64-gpu-%J2K_NEW_ORT_VER%"
echo [j2k][upgrade_gpu]   ORT root: %J2K_ONNXRUNTIME_ROOT%

echo [j2k][upgrade_gpu] Step 4/5  Rebuilding j2k_ch.dll with ORT %J2K_NEW_ORT_VER% (TRT 10 EP)...
if not exist "%PY_DIR%\trt_cache" mkdir "%PY_DIR%\trt_cache"
where cmake >nul 2>nul
if errorlevel 1 if exist "C:\Program Files\CMake\bin\cmake.exe" set "PATH=C:\Program Files\CMake\bin;%PATH%"
where cmake >nul 2>nul
if errorlevel 1 (
  echo [j2k][upgrade_gpu] ERROR: cmake not found.
  goto :fail
)
if exist "%BUILD_DLL_DIR%\CMakeCache.txt" del "%BUILD_DLL_DIR%\CMakeCache.txt"
cmake -S "%CPP_DIR%" -B "%BUILD_DLL_DIR%" -A x64
if errorlevel 1 (
  echo [j2k][upgrade_gpu] ERROR: cmake configure failed.
  goto :fail
)
cmake --build "%BUILD_DLL_DIR%" --config Release --target j2k_ch
if errorlevel 1 (
  echo [j2k][upgrade_gpu] ERROR: build failed.
  goto :fail
)

echo [j2k][upgrade_gpu] Step 5/5  Deploying...
copy /Y "%BUILD_DLL_DIR%\Release\j2k_ch.dll" "%PY_DIR%\bin\j2k_ch.dll" >nul
for %%F in (onnxruntime.dll onnxruntime_providers_shared.dll onnxruntime_providers_cuda.dll onnxruntime_providers_tensorrt.dll) do (
  if exist "%BUILD_DLL_DIR%\Release\%%F" copy /Y "%BUILD_DLL_DIR%\Release\%%F" "%PY_DIR%\bin\%%F" >nul
)
echo [j2k][upgrade_gpu] Done.
echo.
echo  GPU stack upgraded:
echo    ORT  : %J2K_NEW_ORT_VER%  (CUDA 12 + TRT 10 EP)
echo    CUDA : 12 runtime DLLs staged to python\bin
echo    TRT  : pip-installed nvinfer_10.dll staged to python\bin
echo    Cache: %PY_DIR%\trt_cache  (TRT engine builds on first run ~60s)
echo.
echo  Run: cd /d "%PY_DIR%" ^&^& python run_cpp.py
echo.
goto :j2k_done

:fail
if /I not "%J2K_NO_PAUSE%"=="1" (
  echo.
  echo [j2k] Finished with errors. Press any key to close this window.
  pause
)
exit /b 1

:j2k_done
if /I not "%J2K_NO_PAUSE%"=="1" (
  echo.
  echo [j2k] Done. Press any key to close this window.
  pause
)
exit /b 0
