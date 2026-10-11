# Host runtime: kernel/XAM HLE, memory, threads, file system, and (later) GPU/audio/input.
project(DAORecomp CXX)

file(GLOB_RECURSE DAO_RUNTIME_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/*.h)

# Dear ImGui's SDL3 backends for the launcher (the imgui library itself comes from xenia/).
set(DAO_IMGUI_BACKENDS
    ${DAO_XENIA_THIRD_PARTY}/imgui/backends/imgui_impl_sdl3.cpp
    ${DAO_XENIA_THIRD_PARTY}/imgui/backends/imgui_impl_sdlrenderer3.cpp)
set_source_files_properties(${DAO_IMGUI_BACKENDS} PROPERTIES SKIP_PRECOMPILE_HEADERS ON COMPILE_OPTIONS -w)

add_executable(DAORecomp ${DAO_RUNTIME_SOURCES} ${DAO_IMGUI_BACKENDS})

# The release the installer builds (its tag), for the launcher's update check.
set(DAO_VERSION "dev" CACHE STRING "The release this is built from (e.g. v0.2.0)")
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/version.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON COMPILE_DEFINITIONS "DAO_VERSION=\"${DAO_VERSION}\"")

target_include_directories(DAORecomp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XENON_ROOT}/XenonUtils
    ${DAO_XENIA_THIRD_PARTY}/rapidjson/include)

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
