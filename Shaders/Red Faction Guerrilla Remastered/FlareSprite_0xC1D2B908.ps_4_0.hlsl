// Particle flare sprite (ps_4_0), faithful translation of Dump/0xC1D2B908.ps_4_0.cso.
// TONEMAP_TYPE 0: 1:1 translation of the original asm (register numbers match the dump: cb3 = particle constants, cb6 = instance tint).
// TONEMAP_TYPE >= 1: when AnamorphicStreak is on, the sprite is hidden (returns zero). Blend is ONE / INV_SRC_ALPHA, so zero is invisible. Otherwise rgb is max(x, 0) instead of saturate.
// The anamorphic streak replaces this out-of-place flare.

#include "Includes/Common.hlsl"

#ifndef TONEMAP_TYPE
#define TONEMAP_TYPE 0
#endif

cbuffer g_particleFragmentConstantsbuffer : register(b3) { float4 cb3[8]; }
cbuffer g_instanceFragmentConstantsbuffer : register(b6) { float4 cb6[1]; }

Texture2D<float4> t0 : register(t0); // Diffuse_Map_1
Texture2D<float4> t2 : register(t2); // Particle_fog
SamplerState s0 : register(s0);
SamplerState s2 : register(s2);

float4 main(float4 pos : SV_Position, float4 v1 : TEXCOORD0, float v2 : TEXCOORD1, float4 v3 : TEXCOORD2) : SV_Target
{
  // sample r0, v3.xy, t0, s0
  float4 r0 = t0.Sample(s0, v3.xy);

  // dp3 r1.x = L; add r1.yzw = c - L; mad r1.xyz = L + v3.z * (c - L)
  float L = dot(r0.rgb, float3(0.3, 0.59, 0.11));
  float3 r1 = lerp(L, r0.rgb, v3.z);

  // dp3 r2.xyz = colorCorrectionMatrix (rows cb3[5..7]) * r1; max r0.xyz = max(r2, 0). r0.w keeps the diffuse alpha.
  float3 cc = float3(dot(r1, cb3[5].xyz), dot(r1, cb3[6].xyz), dot(r1, cb3[7].xyz));
  r0.rgb = max(cc, 0.0);

  // sample r1, (0.5, 0.5), t2, s2
  float4 fog = t2.Sample(s2, float2(0.5, 0.5));

  // mad r1 = -v1 * r0 + r1
  float4 r1b = -v1 * r0 + fog;
  // mul r0 = r0 * v1
  r0 = r0 * v1;
  // mad r0 = v2.x * r1 + r0
  r0 = v2 * r1b + r0;
  // mul r0 = r0 * cb6[0]
  r0 = r0 * cb6[0];

#if TONEMAP_TYPE >= 1
  if (LumaSettings.GameSettings.AnamorphicStreak != 0u)
    return float4(0.0, 0.0, 0.0, 0.0);
#endif

  // mul_sat o0.xyz = r0.w * r0.xyz
  float3 o_rgb;
#if TONEMAP_TYPE >= 1
  o_rgb = max(r0.w * r0.rgb, 0.0); // not saturated: keeps HDR values (streak is off here)
#else
  o_rgb = saturate(r0.w * r0.rgb);
#endif
  // mad_sat o0.w = v3.w * -r0.w + r0.w
  float o_a = saturate(v3.w * -r0.w + r0.w);

  return float4(o_rgb, o_a);
}
