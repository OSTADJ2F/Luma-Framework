// SMAA implementation (replaces the game's FXAA, see "Replace FXAA with SMAA" in main.cpp)
// See Demo for the reference https://github.com/iryoku/smaa

#include "Includes/Common.hlsl"

// Metrics come from a dedicated cbuffer at b1 (filled by DrawSMAA's cb_smaa_metrics), not from the game's cbuffers.
cbuffer SMAAMetricsCB : register(b1)
{
   float4 SMAAMetricsValue; // (1/width, 1/height, width, height)
};

#define SMAA_RT_METRICS SMAAMetricsValue
#define SMAA_PRESET_ULTRA
#define SMAA_PREDICATION       0
#define SMAAGather(tex, coord) tex.Gather(LinearSampler, coord, 0)

// The blend happens in linear (HDR, negatives kept): encode it back to the game's gamma 2.2 mirror space, alpha is passed through.
#define SMAA_NEIGHBORHOOD_OUTPUT(color, position) color.rgb = linear_to_gamma(color.rgb, GCT_MIRROR);

#include "../Includes/SMAA_Passes.hlsl"
