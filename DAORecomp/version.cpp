#include "version.h"

// Only this file sees DAO_VERSION (and it skips the precompiled header), so a new
// version rebuilds nothing else.
#ifndef DAO_VERSION
#define DAO_VERSION "dev"
#endif

namespace dao
{
    const char* Version()
    {
        return DAO_VERSION;
    }
}
