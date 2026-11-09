#include "Includes/Common.hlsl"
#include "../Includes/ColorGradingLUT.hlsl" // RestoreHueAndChrominance
#include "Includes/RFGOutput.hlsl"

// Faithful translation of the dumped vanilla tonemap (Dump/0xDAC0B2C4.ps_4_0.cso).
// Register numbers match the dump: cb0 = common, cb2 = material (Bloom_amount = cb2[18].y), cb6 = instance tint.
// Vanilla path (TONEMAP_TYPE 0) is exact; the HDR path (TONEMAP_TYPE >= 1) is gated below.

cbuffer g_commonFragmentConstantsbuffer : register(b0) { float4 cb0[1]; }
cbuffer g_materialConstantsbuffer : register(b2) { float4 cb2[19]; }
cbuffer g_instanceFragmentConstantsbuffer : register(b6) { float4 cb6[1]; }

Texture2D<float4> t0 : register(t0); // Base (scene)
Texture2D<float4> t1 : register(t1); // Lum adapt (1x1, .y = k)
Texture2D<float4> t4 : register(t4); // Luma bloom pyramid mip 0 (bound by the mod only when LumaBloom is on)
Texture2D<float4> t5 : register(t5); // Bloom
Texture2D<float4> t6 : register(t6); // Particle (alpha-weighted grade term)
Texture2D<float4> t3 : register(t3); // Anamorphic streak (full res, bound by the mod only when AnamorphicStreak is on). Ported from KinoStreak (MIT).
SamplerState s0 : register(s0);
static const float3 RFG_STREAK_TINT = float3(0.55, 0.55, 1.0); // KinoStreak default tint
static const float RFG_KNEE = 0.5;
static const float RFG_SHOULDER_MAX = 3.0;
SamplerState s1 : register(s1);
SamplerState s5 : register(s5);
SamplerState s6 : register(s6);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target
{
  // Scene: c = lerp(L, c8, cb0.x) * tint
  float3 c8 = t0.Sample(s0, uv).xyz * 8.0;
  float L = dot(c8, float3(0.3, 0.59, 0.11));
  float3 c = lerp(L, c8, cb0[0].x) * cb6[0].xyz;

  // Particle term: g = lerp(gL, sat(t6), cb0.y), a = sat(t6.w)
  float4 g4 = saturate(t6.Sample(s6, uv));
  float gL = dot(g4.xyz, float3(0.3, 0.59, 0.11));
  float3 gS = lerp(gL, g4.xyz, cb0[0].y);
  float a = g4.w;

  float k = t1.Sample(s1, float2(0.5, 0.5)).y;

  float3 omg = 1.0 - gS;
#if TONEMAP_TYPE >= 1
  omg = max(omg, 0.001); // guard log(1-g) for g <= 0.999
#endif
  float3 lnTerm = log2(omg) * 0.693147 / k; // ln(1-g) / k

  float3 bloom = t5.Sample(s5, uv).xyz * cb2[18].y * 8.0;

  // Vanilla exponent: x = c*a - ln(1-g)/k + bloom; out = 1 - exp(-k*x)
  float3 x = c * a - lnTerm + bloom;
  float3 sdr = 1.0 - exp2(x * (-k) * 1.442695);

#if TONEMAP_TYPE >= 1
  // HDR: same x, with scene scaled by Exposure and bloom by BloomStrength. Linear hdr = k * x_hdr.
  float exposure = LumaSettings.GameSettings.Exposure;
  float bloomStrength = LumaSettings.GameSettings.BloomStrength;
  // Luma bloom replaces the game bloom term (same place in x). Stored /8, so *8 like the game bloom.
  // Keep the game bloom always (its wide 640x360 blur gives particle glows like the monitors their soft vanilla halo; replacing it left a
  // thin purple fringe). Luma bloom is added on top for HDR highlights. The bloom source is exposed, so /k puts it back into x units.
  float3 hdrBloom = bloom * bloomStrength;
  if (LumaSettings.GameSettings.LumaBloom != 0u)
    hdrBloom += t4.Sample(s5, uv).xyz / max(k, 1e-4) * LumaSettings.GameSettings.LumaBloomStrength;
  // Vanilla = g + (1-g)*(1-exp(-k*s)): fog/in-scatter g occludes the scene. Keep that split and only linearize the scene part
  // (adding -ln(1-g) unoccluded made dense fog/sky up to ~7x paper white: the "flashbang").
  // t6 also holds the sun disk/flare glow, which exceeds 1: vanilla's saturate clipped it to white, so in HDR the disk read darker
  // than the sky around it. Keep the occlusion weight saturated but let the glow itself go above 1 (saturation applied like vanilla).
  // Saturate like vanilla: the particle target is upgraded to fp16 by Luma, so stacked additive particles accumulate far above 1
  // (vanilla's 8-bit target clipped each blend), which blew out monitor/pickup glows. The screen blend below keeps the sun disk bright anyway.
  float3 gRaw = saturate(t6.Sample(s6, uv).xyz);
  float3 gHdr = lerp(dot(gRaw, float3(0.3, 0.59, 0.11)), gRaw, cb0[0].y);
  // Where the layer saturates (sun disk core) it would fully occlude the bright sky behind it: stop occluding there so the disk stays the brightest spot.
  // Vanilla is a screen blend: 1-(1-g)(1-s'). Its HDR form g + (1-g)*S darkens anything above 1 (S + g*(1-S) < S), which drew a dark
  // sun disk/outline. Screen against the SDR-range part of S only: identical to vanilla for S <= 1, never darker than S above it.
  float3 S = max(k * (c * a * exposure + hdrBloom), 0.0);
  // Vanilla's 1-exp(-S) shoulder is kept up to the knee; highlights extend smoothly toward RFG_SHOULDER_MAX
  // (linear S made the sky 2-3x vanilla).
  float YS = dot(S, float3(0.2126, 0.7152, 0.0722));
  float Yc;
  if (YS <= RFG_KNEE)
  {
    Yc = 1.0 - exp(-YS);
  }
  else
  {
    float yk = 1.0 - exp(-RFG_KNEE);
    float slope = exp(-RFG_KNEE);
    float d = (YS - RFG_KNEE) * slope;
    Yc = yk + d / (1.0 + d / (RFG_SHOULDER_MAX - yk));
  }
  S *= Yc / max(YS, 1e-6);
  float3 hdr = S + gHdr * (LumaSettings.GameSettings.Glow != 0u ? 1.0 : 0.0) * LumaSettings.GameSettings.ParticleBrightness * (1.0 - saturate(S));
  // Anamorphic streak: exposed linear (same units as hdr), added before the hue restore and display mapping.
  if (LumaSettings.GameSettings.AnamorphicStreak != 0u)
    hdr += t3.Sample(s0, uv).rgb * RFG_STREAK_TINT * LumaSettings.GameSettings.StreakIntensity;
  float s = LumaSettings.GameSettings.VanillaHueRestore;
  hdr = RestoreHueAndChrominance(hdr, sdr, 0.75 * s, s);
  hdr = max(hdr, 0.0);
  float3 o = RFG_DisplayOutput(hdr);
  float Lw = dot(o, float3(0.299, 0.587, 0.114));
  return float4(o, Lw / (1.0 + Lw));
#else
  return float4(sdr, dot(sdr, float3(0.299, 0.587, 0.114)));
#endif
}
