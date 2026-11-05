@echo off
setlocal
echo ===================================================================
echo  Building NativeSC2 (Win32 x86 Compatibility Shim for SC2)
echo ===================================================================

cd /d "%~dp0"

if not exist "bin" mkdir "bin"

echo [1/2] Compiling NativeSC2_core.dll (Base 0x10000000)...
clang++ -m32 -shared -O2 -g -Wno-deprecated -Wno-microsoft-cast -D_CRT_SECURE_NO_WARNINGS -Iinclude -Isrc\renderer ^
    src\loader\core_main.cpp ^
    src\loader\xbe_loader.cpp ^
    src\kernel\xboxkrnl_shim.c ^
    src\kernel\xapi_shim.cpp ^
    src\renderer\nv2a_bridge.c ^
    src\renderer\d3d9_device.cpp ^
    src\renderer\d3d9_texture.cpp ^
    src\renderer\d3d9_pushbuffer.cpp ^
    src\renderer\namco_primitives.cpp ^
    src\profiler\cpu_profiler.cpp ^
    src\audio\audio_bridge.c ^
    -o bin\NativeSC2_core.dll ^
    -Wl,/BASE:0x10000000 ^
    -Wl,/MAP:bin\NativeSC2_core.map ^
    -Wl,/LARGEADDRESSAWARE ^
    -lkernel32 -luser32 -lgdi32 -lwinmm -lopengl32 -ld3d9

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] NativeSC2_core.dll build failed!
    exit /b %ERRORLEVEL%
)

echo [2/2] Compiling NativeSC2.exe (Low-Memory Loader, Base 0x00010000, 128MB BSS)...
clang -m32 -O2 -fno-builtin src\loader\loader.c ^
    -o bin\NativeSC2.exe ^
    -Wl,/SUBSYSTEM:CONSOLE ^
    -Wl,/BASE:0x00010000 ^
    -Wl,/FIXED ^
    -Wl,/DYNAMICBASE:NO ^
    -Wl,/LARGEADDRESSAWARE ^
    -Wl,/STACK:0x1000000 ^
    -Wl,/ENTRY:rawMain ^
    -Wl,/NODEFAULTLIB ^
    -lkernel32 -luser32

if %ERRORLEVEL% NEQ 0 (
    echo [ERROR] NativeSC2.exe loader build failed!
    exit /b %ERRORLEVEL%
)

if exist "bin\plus\" (
    copy /Y "bin\NativeSC2.exe" "bin\plus\NativeSC2.exe" >nul
    copy /Y "bin\NativeSC2_core.dll" "bin\plus\NativeSC2_core.dll" >nul
    echo [SYNC] Updated bin\plus\NativeSC2.exe and bin\plus\NativeSC2_core.dll
)

echo.
echo [SUCCESS] Successfully built NativeSC2_core.dll and NativeSC2.exe!
endlocal
