// DAO recomp: the dao_temporal_upscaler option (defined in
// gpu/d3d12/dao_upscale.cc), for the upscaler and for Streamline, which starts
// before the device. Chosen before the game starts: a change takes a restart.
#ifndef XENIA_DAO_TEMPORAL_UPSCALER_H_
#define XENIA_DAO_TEMPORAL_UPSCALER_H_

#include <string>

#include "xenia/base/cvar.h"

DECLARE_string(dao_temporal_upscaler);

namespace xe::dao_options {

enum class TemporalUpscaler { kOff, kAuto, kFsr4, kFsr3, kXess, kDlss };

// The option's value; anything else is auto, as it always was taken.
inline TemporalUpscaler GetTemporalUpscaler() {
  const std::string& name = cvars::dao_temporal_upscaler;
  return name == "off"    ? TemporalUpscaler::kOff
         : name == "fsr4" ? TemporalUpscaler::kFsr4
         : name == "fsr3" ? TemporalUpscaler::kFsr3
         : name == "xess" ? TemporalUpscaler::kXess
         : name == "dlss" ? TemporalUpscaler::kDlss
                          : TemporalUpscaler::kAuto;
}

}  // namespace xe::dao_options

#endif  // XENIA_DAO_TEMPORAL_UPSCALER_H_
