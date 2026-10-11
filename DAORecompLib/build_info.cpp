// Compiled once per library, with DAO_BUILD_INFO (the descriptor's name) and
// DAO_BUILD_MAPPINGS (the build's function table) defined by CMakeLists.txt.
#include "build_info.h"

extern const DaoBuildInfo DAO_BUILD_INFO = {
    uint32_t(PPC_IMAGE_BASE),
    uint32_t(PPC_IMAGE_SIZE),
    uint32_t(PPC_CODE_BASE),
    uint32_t(PPC_CODE_SIZE),
    DAO_BUILD_MAPPINGS,
};
