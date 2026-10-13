# Host runtime: kernel/XAM HLE, memory, threads, file system, and (later) GPU/audio/input.
project(DAORecomp CXX)

file(GLOB_RECURSE DAO_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

# Dear ImGui's SDL3 backends for the launcher (the imgui library itself comes from xenia/),
# and its std::string text fields.
set(DAO_IMGUI_BACKENDS
    ${DAO_XENIA_THIRD_PARTY}/imgui/backends/imgui_impl_sdl3.cpp
    ${DAO_XENIA_THIRD_PARTY}/imgui/backends/imgui_impl_sdlrenderer3.cpp
    ${DAO_XENIA_THIRD_PARTY}/imgui/misc/cpp/imgui_stdlib.cpp)
set_source_files_properties(${DAO_IMGUI_BACKENDS} PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS -w)

# The icon (res/daorecomp.rc), in both builds.
set(DAO_ICON ${CMAKE_CURRENT_SOURCE_DIR}/res/daorecomp.rc)

# The processor check (in both builds): built for any x86-64 processor (the last
# -march wins), told which instructions DAO_MARCH takes for granted.
set(DAO_CPU_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/cpu_check.cpp)
if (DAO_MARCH STREQUAL "x86-64-v3")
    set(DAO_CPU_LEVEL 3)
elseif (DAO_MARCH STREQUAL "sandybridge")
    set(DAO_CPU_LEVEL 1)
else()
    set(DAO_CPU_LEVEL 0)
    message(WARNING "DAO_MARCH=${DAO_MARCH}: the startup processor check knows only sandybridge and x86-64-v3, so this build has none")
endif()
set_source_files_properties(${DAO_CPU_CHECK} PROPERTIES SKIP_PRECOMPILE_HEADERS ON
    COMPILE_OPTIONS -march=x86-64 COMPILE_DEFINITIONS DAO_CPU_LEVEL=${DAO_CPU_LEVEL})

# The launcher-only build's entry point (DAO_LAUNCHER_ONLY, see ../CMakeLists.txt).
set(DAO_SETUP_MAIN ${CMAKE_CURRENT_SOURCE_DIR}/launcher/setup_main.cpp)

if (DAO_LAUNCHER_ONLY)
    file(GLOB DAO_LAUNCHER_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/launcher/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/content/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mods/*.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/mods/convert/*.cpp)
    list(APPEND DAO_LAUNCHER_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/erf.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/kernel/title_update.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/patches/patches.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/settings.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/files.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/log.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hash.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/program.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/version.cpp
        ${DAO_CPU_CHECK})
    add_executable(DAORecomp ${DAO_LAUNCHER_SOURCES} ${DAO_IMGUI_BACKENDS} ${DAO_ICON})
    set(DAO_VERSION "dev" CACHE STRING "The release this is built from (e.g. v0.2.0)")
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/version.cpp PROPERTIES
        SKIP_PRECOMPILE_HEADERS ON COMPILE_DEFINITIONS "DAO_VERSION=\"${DAO_VERSION}\"")
    target_include_directories(DAORecomp PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${XENON_ROOT}/XenonUtils
        ${DAO_XENIA_THIRD_PARTY}/rapidjson/include
        ${DAO_XENIA_THIRD_PARTY}/imgui/misc/cpp)
    target_compile_definitions(DAORecomp PRIVATE
        DAO_LAUNCHER_ONLY NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_compile_options(DAORecomp PRIVATE
        -fno-strict-aliasing -Wno-unused-parameter -Wno-unused-variable -Wno-unused-function)
    # xenia-gpu: the texture layouts, for converting mods' textures.
    target_link_libraries(DAORecomp PRIVATE
        fmt::fmt xenia-gpu SDL3::SDL3-static imgui shell32 winhttp bcrypt)
    # A window program: no console when it's double-clicked. Its DLLs load from
    # System32 only, not from the Downloads folder it's run from.
    target_link_options(DAORecomp PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup /DEPENDENTLOADFLAG:0x800)
    target_precompile_headers(DAORecomp PRIVATE stdafx.h)
    return()
endif()

list(REMOVE_ITEM DAO_RUNTIME_SOURCES ${DAO_SETUP_MAIN})
add_executable(DAORecomp ${DAO_RUNTIME_SOURCES} ${DAO_IMGUI_BACKENDS} ${DAO_ICON})

# The release the installer builds (its tag), for the launcher's update check.
set(DAO_VERSION "dev" CACHE STRING "The release this is built from (e.g. v0.2.0)")
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/version.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON COMPILE_DEFINITIONS "DAO_VERSION=\"${DAO_VERSION}\"")

target_include_directories(DAORecomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XENON_ROOT}/XenonUtils
    ${DAO_XENIA_THIRD_PARTY}/rapidjson/include
    ${DAO_XENIA_THIRD_PARTY}/imgui/misc/cpp)

target_compile_definitions(DAORecomp PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS)

target_compile_options(DAORecomp PRIVATE
    -fno-strict-aliasing
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-function)

# XenonUtils provides XEX decryption/decompression (tiny-AES, lzx) for the loader.
target_link_libraries(DAORecomp PRIVATE
    DAORecompLib
    XenonUtils
    fmt::fmt dao-xenia
    SDL3::SDL3-static
    imgui
    dao-ffmpeg
    onecore
    xinput
    shell32
    winmm
    winhttp
    bcrypt)

# Recompiled code recurses on the host stack as deeply as the guest does. A window
# program, like the launcher-only build above: no console window behind it
# (DAO_CONSOLE=1 gets one, see main.cpp). Unlike the launcher-only build, no
# /DEPENDENTLOADFLAG: this one is run from its install folder, where players put
# mods such as OptiScaler as dxgi.dll (README.md).
target_link_options(DAORecomp PRIVATE /STACK:16777216 /MAP /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)

target_precompile_headers(DAORecomp PRIVATE stdafx.h)

# The temporal upscalers' and frame generation's DLLs go next to the executable;
# they're loaded only when enabled: AMD FidelityFX's signed loader, upscaler (FSR 3.1 /
# FSR 4) and frame generation (FSR 3.1), and Intel's libxess.dll (XeSS). (Their own
# rule, so a missing or changed DLL is copied without relinking the exe.)
set(_upscaler_dlls)
foreach(_src
        ${CMAKE_SOURCE_DIR}/tools/FidelityFX-SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_loader_dx12.dll
        ${CMAKE_SOURCE_DIR}/tools/FidelityFX-SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_dx12.dll
        ${CMAKE_SOURCE_DIR}/tools/FidelityFX-SDK/Kits/FidelityFX/signedbin/amd_fidelityfx_framegeneration_dx12.dll
        ${CMAKE_SOURCE_DIR}/tools/XeSS-SDK/bin/libxess.dll)
    get_filename_component(_dll ${_src} NAME)
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${_dll}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${_src} ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${_src})
    list(APPEND _upscaler_dlls ${CMAKE_CURRENT_BINARY_DIR}/${_dll})
endforeach()
add_custom_target(DAORecompUpscalerDlls DEPENDS ${_upscaler_dlls})
add_dependencies(DAORecomp DAORecompUpscalerDlls)
