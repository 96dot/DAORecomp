# The recompiled game code (generated into DAO_PPC_DIR by install.py or scripts/recomp.cmd).
project(DAORecompLib CXX)

file(GLOB DAO_PPC_SOURCES CONFIGURE_DEPENDS
    ${DAO_PPC_DIR}/ppc_recomp.*.cpp
    ${DAO_PPC_DIR}/ppc_func_mapping.cpp)

add_library(DAORecompLib STATIC ${DAO_PPC_SOURCES})

target_include_directories(DAORecompLib PUBLIC
    ${DAO_PPC_DIR}
    ${XENON_ROOT}/thirdparty/simde)

target_compile_options(DAORecompLib PRIVATE
    -fno-strict-aliasing
    /fp:strict
    -Wno-unused-label
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    /FI${CMAKE_CURRENT_SOURCE_DIR}/mmio_hook.h)

target_precompile_headers(DAORecompLib PRIVATE ${DAO_PPC_DIR}/ppc_recomp_shared.h)
