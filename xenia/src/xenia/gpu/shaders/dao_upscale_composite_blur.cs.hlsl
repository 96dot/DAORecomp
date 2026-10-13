// dao_upscale_composite.cs with motion blur: a pipeline of its own, so the
// composite without blur doesn't pay for its registers.
#define DAO_MOTION_BLUR 1
#include "dao_upscale_composite.hlsli"
