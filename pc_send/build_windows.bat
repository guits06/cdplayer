@echo off
REM ==============================================================================
REM Script de compilacion nativo C++ de Windows para RPi Audio Streamer (.exe)
REM Compila 'rpi_streamer_windows.exe' usando MinGW g++ o MSVC cl.exe
REM ==============================================================================

echo === Compilando RPi C++ Streamer para Windows ===

where g++ >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [1/2] Compilando con GCC / MinGW...
    g++ -O3 -std=c++17 rpi_streamer_windows.cpp -o rpi_streamer_windows.exe -lws2_32 -lole32
    if %ERRORLEVEL% EQU 0 (
        echo ============================================================
        echo [OK] Ejecutable C++ creado con exito: rpi_streamer_windows.exe
        echo ============================================================
        goto :done
    )
)

where cl >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    echo [1/2] Compilando con Microsoft Visual C++ (cl.exe)...
    cl /O2 /std:c++17 rpi_streamer_windows.cpp /Fe:rpi_streamer_windows.exe ws2_32.lib ole32.lib
    if %ERRORLEVEL% EQU 0 (
        echo ============================================================
        echo [OK] Ejecutable C++ creado con exito: rpi_streamer_windows.exe
        echo ============================================================
        goto :done
    )
)

echo [!] No se encontro g++ ni cl.exe en PATH. Instala MinGW-w64 o Visual Studio C++.

:done
pause
