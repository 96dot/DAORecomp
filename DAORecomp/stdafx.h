#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/core.h>

#include <ppc_config.h>
#include <ppc_context.h>
// The disc build's values; the runtime reads the running build's from
// kernel/game_build.h instead (so a leftover use doesn't compile).
#undef PPC_IMAGE_BASE
#undef PPC_IMAGE_SIZE
#undef PPC_CODE_BASE
#undef PPC_CODE_SIZE

#include <byteswap.h>
#include <xbox.h>

#include "log.h"
