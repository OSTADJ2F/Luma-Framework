// Anamorphic streak downsampler (ps_5_0). Ported from KinoStreak Shader/Streak.cginc frag_down.
// Copyright (c) Keijiro Takahashi, MIT License (see LICENSE_KinoStreak.txt).
// Horizontal 6-tap box filter at odd texel offsets (hscale 1.25). Input t0 = previous level.

#include "Includes/RFGStreak.hlsl"

Texture2D<float4> t0 : register(t0);
SamplerState s0 : register(s0);

float4 main(float4 pos : SV_Position) : SV_Target
{
  float2 uv = pos.xy * OutSize.zw;

  const float hscale = 1.25;
  float dx = InSize.z * hscale;

  float3 c0 = t0.Sample(s0, float2(uv.x - dx * 5.0, uv.y)).rgb;
  float3 c1 = t0.Sample(s0, float2(uv.x - dx * 3.0, uv.y)).rgb;
  float3 c2 = t0.Sample(s0, float2(uv.x - dx * 1.0, uv.y)).rgb;
  float3 c3 = t0.Sample(s0, float2(uv.x + dx * 1.0, uv.y)).rgb;
  float3 c4 = t0.Sample(s0, float2(uv.x + dx * 3.0, uv.y)).rgb;
  float3 c5 = t0.Sample(s0, float2(uv.x + dx * 5.0, uv.y)).rgb;

  float3 c = (c0 + c1 + c2 + c3 + c4 + c5) / 6.0;
  return float4(c, 1.0);
}
