#pragma once
// The game's menus' colours, for the launcher and the on-screen keyboard: cream
// serif text and gold edges.

#include <algorithm>

#include <imgui.h>

namespace dao::ui::palette
{
    constexpr ImU32 kText = IM_COL32(236, 226, 204, 255);
    constexpr ImU32 kTextDim = IM_COL32(190, 178, 154, 255);
    constexpr ImU32 kTextOff = IM_COL32(120, 110, 96, 255);
    constexpr ImU32 kGold = IM_COL32(176, 146, 92, 255);
    constexpr ImU32 kGoldBright = IM_COL32(232, 204, 140, 255);
    constexpr ImU32 kShadow = IM_COL32(0, 0, 0, 200);

    // `color` with its alpha scaled by `alpha` (0 to 1).
    inline ImU32 WithAlpha(ImU32 color, float alpha)
    {
        const float a = float(color >> IM_COL32_A_SHIFT & 0xFF) * std::clamp(alpha, 0.0f, 1.0f);
        return (color & ~IM_COL32_A_MASK) | ImU32(a + 0.5f) << IM_COL32_A_SHIFT;
    }
}
