@echo off
setlocal

cd /d "%~dp0"

if not exist build (
    mkdir build
)

set "GPP=D:\Tools\IDEs\QT\Tools\mingw810_64\bin\g++.exe"

if not exist "%GPP%" (
    echo [ERROR] g++ not found: "%GPP%"
    exit /b 1
)

"%GPP%" ^
  -std=c++17 ^
  -Isrc ^
  src/core/file_transfer_hash.cpp ^
  src/core/file_transfer_state_machine.cpp ^
  src/demo/pure_cpp_file_transfer_demo.cpp ^
  -o build/pure_cpp_file_transfer_demo.exe

if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo [OK] Built: build\pure_cpp_file_transfer_demo.exe
exit /b 0

:: 等同于去命令行执行这个命令：
::  & "D:/Tools/IDEs/QT/Tools/mingw810_64/bin/g++.exe" `
::  -std=c++17 `
::  -Isrc `
::  src/core/file_transfer_hash.cpp `
::  src/core/file_transfer_state_machine.cpp `
::  src/demo/pure_cpp_file_transfer_demo.cpp `
::  -o build/pure_cpp_file_transfer_demo.exe

