// Anamorphic streak composite (ps_5_0). Ported from KinoStreak Shader/Streak.cginc frag_composite.
// Copyright (c) Keijiro Takahashi, MIT License (see LICENSE_KinoStreak.txt).
// Kino adds tint * intensity and the source here. This port keeps the *5 scale only: the tonemap
// adds streak * StreakTint * StreakIntensity to the exposed HDR color, so the scene is not added twice.

#include "Includes/RFGStreak.hlsl"

Texture2D<float4> t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 pos : SV_Position) : SV_Target
{
  float2 uv = pos.xy * OutSize.zw;
  float3 c = t0.Sample(s0, uv).rgb;
  return float4(c * 5.0, 1.0);
}
