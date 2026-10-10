# Toolchain: clang-cl + lld-link against an MSVC CRT and Windows SDK splatted by xwin
# (https://github.com/Jake-Shadle/xwin, /winsysroot layout). Builds the Windows
# executable the same way on Windows and Linux hosts, without Visual Studio.
#
#   -DCMAKE_TOOLCHAIN_FILE=cmake/clang-cl-xwin.cmake
#   -DDAO_LLVM_BIN=<dir with clang-cl, lld-link, llvm-lib, llvm-rc>
#   -DDAO_WINSYSROOT=<xwin splat --use-winsysroot-style output>
#
# The installer (install.py) sets both.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

# try_compile projects re-read this file; hand them the same settings.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DAO_LLVM_BIN DAO_WINSYSROOT)

foreach(var DAO_LLVM_BIN DAO_WINSYSROOT)
    if(NOT ${var} AND DEFINED ENV{${var}})
        set(${var} "$ENV{${var}}")
    endif()
    if(NOT ${var})
        message(FATAL_ERROR "clang-cl-xwin.cmake: set ${var}")
    endif()
    file(TO_CMAKE_PATH "${${var}}" ${var})
endforeach()

if(CMAKE_HOST_WIN32)
    set(_exe ".exe")
else()
    set(_exe "")
endif()

set(CMAKE_C_COMPILER "${DAO_LLVM_BIN}/clang-cl${_exe}")
set(CMAKE_CXX_COMPILER "${DAO_LLVM_BIN}/clang-cl${_exe}")
set(CMAKE_LINKER "${DAO_LLVM_BIN}/lld-link${_exe}")
set(CMAKE_AR "${DAO_LLVM_BIN}/llvm-lib${_exe}")
set(CMAKE_RC_COMPILER "${DAO_LLVM_BIN}/llvm-rc${_exe}")
set(CMAKE_MT "${DAO_LLVM_BIN}/llvm-mt${_exe}")
set(CMAKE_C_COMPILER_TARGET x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

# Only the splatted CRT/SDK: with a sysroot, clang-cl ignores %INCLUDE% and any
# installed Visual Studio. (/X would drop the sysroot headers too.)
set(_sysroot "-winsysroot \"${DAO_WINSYSROOT}\"")
set(CMAKE_C_FLAGS_INIT "${_sysroot}")
set(CMAKE_CXX_FLAGS_INIT "${_sysroot}")

file(GLOB _sdk_includes "${DAO_WINSYSROOT}/Windows Kits/10/Include/*")
list(SORT _sdk_includes)
list(GET _sdk_includes -1 _sdk_include)
set(CMAKE_RC_FLAGS_INIT "/I \"${_sdk_include}/um\" /I \"${_sdk_include}/shared\"")

# lld-link finds the libraries itself. No manifest step (llvm-mt may lack libxml2);
# the executables set what a manifest would (DPI awareness) at run time.
foreach(kind EXE SHARED MODULE)
    set(CMAKE_${kind}_LINKER_FLAGS_INIT "\"/winsysroot:${DAO_WINSYSROOT}\" /manifest:no")
endforeach()

# xwin leaves out the debug CRT; probe with the release one.
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

set(CMAKE_FIND_ROOT_PATH "${DAO_WINSYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
