// Anamorphic streak prefilter (ps_5_0). Ported from KinoStreak Shader/Streak.cginc frag_prefilter.
// Copyright (c) Keijiro Takahashi, MIT License (see LICENSE_KinoStreak.txt).
// Input t0 = HDR scene (stored /8, as the tonemap reads it). Input t1 = 1x1 exposure, .y = k.
// Output = exposed linear (paper white = 1), thresholded, at half height.

#include "Includes/RFGStreak.hlsl"

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
SamplerState s0 : register(s0);

float4 main(float4 pos : SV_Position) : SV_Target
{
  float2 uv = pos.xy * OutSize.zw;

  // Kino: average two vertically offset taps (vscale 1.5 on the source texel height).
  const float vscale = 1.5;
  float dy = InSize.w * vscale / 2.0;
  float3 c0 = t0.Sample(s0, float2(uv.x, uv.y - dy)).rgb;
  float3 c1 = t0.Sample(s0, float2(uv.x, uv.y + dy)).rgb;
  float3 c = (c0 + c1) * 0.5;

  // Undo the /8 storage and apply the exposure.
  float k = t1.Sample(s0, float2(0.5, 0.5)).y;
  c *= 8.0 * k;

  // Kino threshold: keep only the part of the max channel above the threshold.
  float br = max(c.r, max(c.g, c.b));
  c *= max(0.0, br - Params.x) / max(br, 1e-5);

  return float4(c, 1.0);
}
