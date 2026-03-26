@echo off
setlocal EnableExtensions EnableDelayedExpansion

if /I "%~1"=="_run_node" goto :run_node

set "ROOT=%~dp0"
cd /d "%ROOT%"

echo [INFO] Workspace: %CD%

set "EXE="
if exist "build\Desktop_Qt_5_15_2_MinGW_64_bit-Debug\clipboard_sync.exe" set "EXE=build\Desktop_Qt_5_15_2_MinGW_64_bit-Debug\clipboard_sync.exe"
if not defined EXE if exist "build\clipboard_sync.exe" set "EXE=build\clipboard_sync.exe"
if not defined EXE if exist "build\Release\clipboard_sync.exe" set "EXE=build\Release\clipboard_sync.exe"
if not defined EXE if exist "build\Debug\clipboard_sync.exe" set "EXE=build\Debug\clipboard_sync.exe"

if not defined EXE (
    echo [INFO] Existing executable not found, trying to build...
    call :find_qt_prefix
    call :try_build
)

if not defined EXE (
    echo [ERROR] Could not find or build clipboard_sync.exe.
    echo [ERROR] Please install CMake + Qt 5.15.2 MinGW kit, then run this script again.
    exit /b 1
)

call :prepare_runtime_path

echo [INFO] Using executable: %EXE%
echo [INFO] Launching two local nodes...

start "CSYNC-A 45454->45455" cmd /k call "%~f0" _run_node A 45454 45455 "%CD%\%EXE%" "%RUNTIME_PREFIX%"
start "CSYNC-B 45455->45454" cmd /k call "%~f0" _run_node B 45455 45454 "%CD%\%EXE%" "%RUNTIME_PREFIX%"

timeout /t 2 >nul
call :post_launch_healthcheck

echo [INFO] Started.
echo [INFO] Node A: listen 45454, peer 45455
echo [INFO] Node B: listen 45455, peer 45454
echo [INFO] Close both terminal windows to stop the nodes.
exit /b 0

:run_node
setlocal EnableExtensions
set "NODE_NAME=%~2"
set "LISTEN_PORT=%~3"
set "PEER_PORT=%~4"
set "EXE_PATH=%~5"
set "RUNTIME_PREFIX=%~6"

set "CSYNC_LISTEN_PORT=%LISTEN_PORT%"
set "CSYNC_PEER_HOST=127.0.0.1"
set "CSYNC_PEER_PORT=%PEER_PORT%"
set "CSYNC_NODE_ID=node-%NODE_NAME%"
if /I "%NODE_NAME%"=="B" (
    set "CSYNC_ENABLE_MONITOR=0"
) else (
    set "CSYNC_ENABLE_MONITOR=1"
)

if defined RUNTIME_PREFIX set "PATH=%RUNTIME_PREFIX%%PATH%"

echo [INFO] Starting node %NODE_NAME%...
echo [INFO] monitor=%CSYNC_ENABLE_MONITOR%
if not exist "%EXE_PATH%" (
    echo [ERROR] Executable not found: %EXE_PATH%
    echo [HINT] Rebuild project or fix EXE path.
    exit /b 1
)

"%EXE_PATH%"
set "EXIT_CODE=%ERRORLEVEL%"
echo [WARN] Node %NODE_NAME% exited with code %EXIT_CODE%
echo [HINT] Read error lines above. Usually missing Qt/MinGW runtime DLL, port bind failure, or bad exe path.
exit /b 0

:post_launch_healthcheck
set "NODE_COUNT=0"
for /f %%N in ('tasklist /FI "IMAGENAME eq clipboard_sync.exe" /NH ^| find /C /I "clipboard_sync.exe"') do set "NODE_COUNT=%%N"

echo [CHECK] clipboard_sync.exe process count: !NODE_COUNT!
if !NODE_COUNT! LSS 2 (
    echo [WARN] Less than 2 node processes are running.
    echo [HINT] Open both CSYNC windows and check error lines; if DLL missing, run windeployqt or fix PATH.
)

call :check_port_listening 45454 "Node A"
call :check_port_listening 45455 "Node B"
exit /b 0

:check_port_listening
set "PORT=%~1"
set "LABEL=%~2"
netstat -ano | findstr /R /C:":%PORT% " | findstr /I "LISTENING" >nul
if errorlevel 1 (
    echo [WARN] %LABEL% is not listening on port %PORT%.
) else (
    echo [CHECK] %LABEL% is listening on port %PORT%.
)
exit /b 0

:find_qt_prefix
if defined CMAKE_PREFIX_PATH (
    if exist "%CMAKE_PREFIX_PATH%\lib\cmake\Qt5\Qt5Config.cmake" (
        set "QT_PREFIX=%CMAKE_PREFIX_PATH%"
    )
)

if not defined QT_PREFIX if defined QT_DIR (
    if exist "%QT_DIR%\lib\cmake\Qt5\Qt5Config.cmake" (
        set "QT_PREFIX=%QT_DIR%"
    )
)

if not defined QT_PREFIX (
    for %%P in (
        "D:\Qt\5.15.2\mingw81_64"
        "C:\Qt\5.15.2\mingw81_64"
        "D:\Tools\IDEs\QT\5.15.2\mingw81_64"
        "C:\Tools\IDEs\QT\5.15.2\mingw81_64"
    ) do (
        if not defined QT_PREFIX if exist "%%~P\lib\cmake\Qt5\Qt5Config.cmake" (
            set "QT_PREFIX=%%~P"
        )
    )
)

if defined QT_PREFIX (
    echo [INFO] Qt prefix: %QT_PREFIX%
) else (
    echo [WARN] Qt prefix not found from CMAKE_PREFIX_PATH/QT_DIR/common locations.
)
exit /b 0

:try_build
where cmake >nul 2>&1
if errorlevel 1 (
    echo [WARN] cmake is not in PATH, skip build.
    exit /b 0
)

if not defined QT_PREFIX (
    echo [WARN] Qt prefix unavailable, skip build.
    exit /b 0
)

set "BUILD_DIR=build\local-mingw-debug"
echo [INFO] Configuring: %BUILD_DIR%
cmake -S . -B "%BUILD_DIR%" -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH="%QT_PREFIX%"
if errorlevel 1 (
    echo [WARN] CMake configure failed. Common reason: mingw32-make not in PATH.
    exit /b 0
)

echo [INFO] Building...
cmake --build "%BUILD_DIR%" -j
if errorlevel 1 (
    echo [WARN] Build failed.
    exit /b 0
)

if exist "%BUILD_DIR%\clipboard_sync.exe" (
    set "EXE=%BUILD_DIR%\clipboard_sync.exe"
)
exit /b 0

:prepare_runtime_path
set "RUNTIME_PREFIX="
if defined QT_PREFIX if exist "%QT_PREFIX%\bin" set "RUNTIME_PREFIX=%QT_PREFIX%\bin;"

for %%M in (
    "%QT_PREFIX%\..\..\Tools\mingw810_64\bin"
    "%QT_PREFIX%\..\..\Tools\mingw1120_64\bin"
    "%QT_PREFIX%\..\..\Tools\mingw1310_64\bin"
) do (
    if exist "%%~M" set "RUNTIME_PREFIX=%RUNTIME_PREFIX%%%~M;"
)
exit /b 0
