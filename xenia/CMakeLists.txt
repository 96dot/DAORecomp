# Xenia's GPU (Direct3D 12 backend), UI/presenter and base libraries, vendored from
# tools/xenia-canary (Xenia is BSD-licensed, see tools/xenia-canary/LICENSE) and
# patched to run inside the DAO runtime: scripts/patch_xenia.py,
# src/xenia/memory.h (memory shim) and src/xenia/dao_host.h (runtime services).
#
# Third-party libraries come from tools/xenia-canary/third_party (fetched by
# install.py, DAO_XENIA_THIRD_PARTY). Shaders are precompiled to DXBC with
# scripts/build_xenia_shaders.py (FXC from the Windows SDK).

set(XE_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(XE_SRC ${XE_ROOT}/src/xenia)
set(XE_TP ${DAO_XENIA_THIRD_PARTY})
# Xenia includes them as "third_party/<library>/...".
get_filename_component(XE_TP_PARENT ${XE_TP} DIRECTORY)

# Sources of one Xenia directory, filtered like Xenia's xe_platform_sources: no
# tests/mains/demos, only the Windows and x64 variants of platform files.
function(dao_xenia_sources target dir)
    file(GLOB _all ${dir}/*.cc ${dir}/*.cpp ${dir}/*.c ${dir}/*.h ${dir}/*.inc)
    set(_sources)
    foreach(src ${_all})
        get_filename_component(_name ${src} NAME_WE)
        if(_name MATCHES "_(main|test|demo)$")
            continue()
        endif()
        if(_name MATCHES "_(linux|posix|gnulinux|x11|gtk|android|mac|arm64)$")
            continue()
        endif()
        list(APPEND _sources ${src})
    endforeach()
    target_sources(${target} PRIVATE ${_sources})
endfunction()

# Defines that headers depend on (seen by users of the libraries too).
set(DAO_XENIA_PUBLIC_DEFINES
    XE_OPTION_PROFILING=0
    XE_OPTION_PROFILING_UI=0
    IMGUI_DISABLE_DEFAULT_FONT
    IMGUI_USE_STB_SPRINTF
    IMGUI_DISABLE_OBSOLETE_FUNCTIONS)

# Build settings of Xenia's own sources only (UNICODE must not leak into the runtime).
set(DAO_XENIA_DEFINES
    _LIB
    _CRT_NONSTDC_NO_DEPRECATE
    _CRT_SECURE_NO_WARNINGS
    WIN32
    _WIN64=1
    _AMD64=1
    UNICODE
    _UNICODE)

function(dao_xenia_defaults target)
    target_include_directories(${target} PUBLIC ${XE_ROOT} ${XE_ROOT}/src)
    target_include_directories(${target} SYSTEM PUBLIC ${XE_TP} ${XE_TP_PARENT})
    target_compile_definitions(${target} PUBLIC ${DAO_XENIA_PUBLIC_DEFINES} PRIVATE ${DAO_XENIA_DEFINES})
    target_compile_options(${target} PRIVATE /utf-8 -mavx -w)
endfunction()

# ---------------------------------------------------------------- third party

add_library(fmt STATIC ${XE_TP}/fmt/src/format.cc ${XE_TP}/fmt/src/os.cc)
target_include_directories(fmt PUBLIC ${XE_TP}/fmt/include)
target_compile_options(fmt PRIVATE /utf-8 -w)
add_library(fmt::fmt ALIAS fmt)

add_library(xxhash STATIC ${XE_TP}/xxhash/xxhash.c)
target_include_directories(xxhash PUBLIC ${XE_TP}/xxhash)
target_compile_options(xxhash PRIVATE -w)
add_library(xxHash::xxhash ALIAS xxhash)

add_library(dxbc STATIC ${XE_TP}/dxbc/DXBCChecksum.cpp)
target_include_directories(dxbc PUBLIC ${XE_TP}/dxbc)
target_compile_options(dxbc PRIVATE -w)

set(HAVE_SYS_UIO_H_01 0)
set(PROJECT_VERSION_MAJOR 1)
set(PROJECT_VERSION_MINOR 2)
set(PROJECT_VERSION_PATCH 1)
configure_file(${XE_TP}/snappy/snappy-stubs-public.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/snappy_gen/snappy-stubs-public.h)
add_library(snappy STATIC
    ${XE_TP}/snappy/snappy-sinksource.cc
    ${XE_TP}/snappy/snappy-stubs-internal.cc
    ${XE_TP}/snappy/snappy.cc)
target_include_directories(snappy PUBLIC ${XE_TP}/snappy ${CMAKE_CURRENT_BINARY_DIR}/snappy_gen)
target_compile_options(snappy PRIVATE -w)

add_library(imgui STATIC
    ${XE_TP}/imgui/imgui.cpp
    ${XE_TP}/imgui/imgui_demo.cpp
    ${XE_TP}/imgui/imgui_draw.cpp
    ${XE_TP}/imgui/imgui_tables.cpp
    ${XE_TP}/imgui/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${XE_TP}/imgui PRIVATE ${XE_TP}/stb)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_FONT IMGUI_USE_STB_SPRINTF IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(imgui PRIVATE -w)

# ---------------------------------------------------------------- xenia

add_library(xenia-base STATIC)
dao_xenia_sources(xenia-base ${XE_SRC}/base)
get_target_property(_base_srcs xenia-base SOURCES)
list(FILTER _base_srcs EXCLUDE REGEX "(console_app_main_|main_init_|main_win\\.cc)")
set_target_properties(xenia-base PROPERTIES SOURCES "${_base_srcs}")
target_link_libraries(xenia-base PUBLIC fmt)
dao_xenia_defaults(xenia-base)

add_library(xenia-ui STATIC)
dao_xenia_sources(xenia-ui ${XE_SRC}/ui)
get_target_property(_ui_srcs xenia-ui SOURCES)
list(FILTER _ui_srcs EXCLUDE REGEX "(windowed_app_main_|file_picker_|microprofile_drawer)")
set_target_properties(xenia-ui PROPERTIES SOURCES "${_ui_srcs}")
target_link_libraries(xenia-ui PUBLIC xenia-base imgui PRIVATE dwmapi dxgi winmm)
dao_xenia_defaults(xenia-ui)

add_library(xenia-ui-d3d12 STATIC)
dao_xenia_sources(xenia-ui-d3d12 ${XE_SRC}/ui/d3d12)
target_include_directories(xenia-ui-d3d12 SYSTEM PUBLIC ${XE_TP}/DirectX-Headers/include ${XE_TP}/DirectXShaderCompiler/include)
target_link_libraries(xenia-ui-d3d12 PUBLIC xenia-base xenia-ui PRIVATE d3d12 dxgi dxguid)
dao_xenia_defaults(xenia-ui-d3d12)

add_library(xenia-gpu STATIC)
dao_xenia_sources(xenia-gpu ${XE_SRC}/gpu)
target_link_libraries(xenia-gpu PUBLIC dxbc fmt snappy xxhash xenia-base xenia-ui)
dao_xenia_defaults(xenia-gpu)

add_library(xenia-gpu-d3d12 STATIC)
dao_xenia_sources(xenia-gpu-d3d12 ${XE_SRC}/gpu/d3d12)
target_link_libraries(xenia-gpu-d3d12 PUBLIC xenia-gpu xenia-ui-d3d12)
dao_xenia_defaults(xenia-gpu-d3d12)
# AMD FidelityFX API headers for the temporal upscaler (gpu/d3d12/dao_upscale.cc);
# the DLLs are loaded at run time.
set(DAO_FFX_SDK ${CMAKE_SOURCE_DIR}/tools/FidelityFX-SDK/Kits/FidelityFX)
target_include_directories(xenia-gpu-d3d12 SYSTEM PRIVATE ${DAO_FFX_SDK}/api/include ${DAO_FFX_SDK}/upscalers/include)

# Everything the runtime links.
add_library(dao-xenia INTERFACE)
target_link_libraries(dao-xenia INTERFACE xenia-gpu-d3d12 xenia-gpu xenia-ui-d3d12 xenia-ui xenia-base
    ntdll ws2_32 xinput comctl32 shcore shlwapi dxguid bcrypt)

# FFmpeg subset for XMA decoding (Xenia's xmaframes decoder).
include(${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg.cmake)
add_library(dao-ffmpeg INTERFACE)
target_include_directories(dao-ffmpeg SYSTEM INTERFACE ${XE_TP}/ffmpeg-xenia ${XE_TP}/FFmpeg)
target_link_libraries(dao-ffmpeg INTERFACE libavcodec libavutil bcrypt)
