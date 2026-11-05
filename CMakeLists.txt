cmake_minimum_required(VERSION 3.16)
set(VCPKG_APPLOCAL_DEPS OFF CACHE BOOL "" FORCE)
project(sc2_xb_shim LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Output both NativeSC2.exe and NativeSC2_core.dll directly into bin/
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
foreach(CONFIG_TYPE ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${CONFIG_TYPE} CONFIG_UPPER)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${CONFIG_UPPER} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

add_compile_definitions(_CRT_SECURE_NO_WARNINGS)

# ===================================================================
# 1. NativeSC2_core.dll (Base 0x10000000)
# ===================================================================
add_library(NativeSC2_core SHARED
    src/loader/core_main.cpp
    src/loader/xbe_loader.cpp
    src/loader/mod_engine.cpp
    src/kernel/xboxkrnl_shim.c
    src/kernel/xapi_shim.cpp
    src/renderer/nv2a_bridge.c
    src/renderer/d3d9_device.cpp
    src/renderer/d3d9_texture.cpp
    src/renderer/d3d9_pushbuffer.cpp
    src/renderer/namco_primitives.cpp
    src/profiler/cpu_profiler.cpp
    src/audio/audio_bridge.c
)

target_include_directories(NativeSC2_core PRIVATE
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/src/renderer"
)

target_link_libraries(NativeSC2_core PRIVATE
    kernel32
    user32
    gdi32
    winmm
    opengl32
    d3d9
)

if(MSVC)
    target_compile_options(NativeSC2_core PRIVATE
        $<$<CXX_COMPILER_ID:Clang>:-Wno-deprecated -Wno-microsoft-cast>
    )
    target_link_options(NativeSC2_core PRIVATE
        /BASE:0x10000000
        /LARGEADDRESSAWARE
    )
else()
    target_compile_options(NativeSC2_core PRIVATE
        -m32 -O2 -g -Wno-deprecated -Wno-microsoft-cast
    )
    target_link_options(NativeSC2_core PRIVATE
        -m32
        -Wl,/BASE:0x10000000
        -Wl,/LARGEADDRESSAWARE
    )
endif()

# ===================================================================
# 2. NativeSC2.exe (Low-Memory Loader, Base 0x00010000, 128MB BSS)
# ===================================================================
add_executable(NativeSC2
    src/loader/loader.c
)

target_link_libraries(NativeSC2 PRIVATE
    kernel32
    user32
)

if(MSVC)
    # Disable stack security cookies (/GS-), default library directives (/Zl), and builtin loop-to-strlen (/Oi- / -fno-builtin) for /NODEFAULTLIB
    target_compile_options(NativeSC2 PRIVATE
        /GS- /Zl /Oi- /Gs9999999
        $<$<C_COMPILER_ID:Clang>:-clang:-fno-builtin>
    )
    # Clear default CMake linker flags that pull in CRT libraries
    set_target_properties(NativeSC2 PROPERTIES LINK_FLAGS "")
    target_link_options(NativeSC2 PRIVATE
        /SUBSYSTEM:CONSOLE
        /BASE:0x00010000
        /FIXED
        /DYNAMICBASE:NO
        /LARGEADDRESSAWARE
        /STACK:0x1000000
        /ENTRY:rawMain
        /NODEFAULTLIB
    )
else()
    target_compile_options(NativeSC2 PRIVATE -m32 -O2 -fno-builtin)
    target_link_options(NativeSC2 PRIVATE
        -m32
        -Wl,/SUBSYSTEM:CONSOLE
        -Wl,/BASE:0x00010000
        -Wl,/FIXED
        -Wl,/DYNAMICBASE:NO
        -Wl,/LARGEADDRESSAWARE
        -Wl,/STACK:0x1000000
        -Wl,/ENTRY:rawMain
        -Wl,/NODEFAULTLIB
    )
endif()
