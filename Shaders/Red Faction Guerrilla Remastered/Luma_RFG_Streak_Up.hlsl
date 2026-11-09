// Anamorphic streak upsampler (ps_5_0). Ported from KinoStreak Shader/Streak.cginc frag_up.
// Copyright (c) Keijiro Takahashi, MIT License (see LICENSE_KinoStreak.txt).
// t0 = smaller level (last), t1 = the matching downsample level (high). Output = lerp(high, last, stretch).

#include "Includes/RFGStreak.hlsl"

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
SamplerState s0 : register(s0);

float4 main(float4 pos : SV_Position) : SV_Target
{
  float2 uv = pos.xy * OutSize.zw;

  // Kino samples the same texel with weights 1/4, 1/2, 1/4, so the sum is a single sample.
  float3 low = t0.Sample(s0, uv).rgb;
  float3 high = t1.Sample(s0, uv).rgb;

  return float4(lerp(high, low, Params.y), 1.0);
}
