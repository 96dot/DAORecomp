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
        ${CMAKE_CURRENT_SOURCE_DIR}/log.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hash.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/program.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/version.cpp)
    add_executable(DAORecomp ${DAO_LAUNCHER_SOURCES} ${DAO_IMGUI_BACKENDS})
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
add_executable(DAORecomp ${DAO_RUNTIME_SOURCES} ${DAO_IMGUI_BACKENDS})

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

# Recompiled code recurses on the host stack as deeply as the guest does.
target_link_options(DAORecomp PRIVATE /STACK:16777216 /MAP)

target_precompile_headers(DAORecomp PRIVATE stdafx.h)

# AMD FidelityFX (FSR 3.1 / FSR 4 temporal upscaling): the signed loader and
# upscaler DLLs go next to the executable; they're loaded only when enabled.
foreach(dll amd_fidelityfx_loader_dx12.dll amd_fidelityfx_upscaler_dx12.dll)
    add_custom_command(TARGET DAORecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_SOURCE_DIR}/tools/FidelityFX-SDK/Kits/FidelityFX/signedbin/${dll}
            $<TARGET_FILE_DIR:DAORecomp>)
endforeach()
