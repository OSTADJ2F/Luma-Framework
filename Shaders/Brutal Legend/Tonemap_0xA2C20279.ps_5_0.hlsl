#include "Includes/Common.hlsl"
#include "../Includes/DICE.hlsl"
#include "../Includes/ColorGradingLUT.hlsl"
#include "Includes/GamutExpansion.hlsl"

Texture2D<float4> t2 : register(t2); // Noise / dither
Texture2D<float4> t3 : register(t3); // Scene
Texture2D<float4> t4 : register(t4); // Bloom
#if TONEMAP_TYPE >= 1
Texture2D<float> t8 : register(t8); // Scene depth (1.0 = far plane / sky), bound only when SkyDepthActive > 0.5

#ifndef SKY_DEPTH_THRESHOLD
#define SKY_DEPTH_THRESHOLD 0.99999
#endif
#ifndef SKY_BLOOM_SCALE
#define SKY_BLOOM_SCALE 1.0 // <1 made fogged mountains (full bloom) glow against the sky
#endif
#ifndef SKY_HUE_RESTORE_SCALE
#define SKY_HUE_RESTORE_SCALE 0.5
#endif
#endif

SamplerState s2_s : register(s2);
SamplerState s3_s : register(s3);
SamplerState s4_s : register(s4);

cbuffer cb4 : register(b4)
{
  float4 cb4[236];
}

cbuffer cb3 : register(b3)
{
  float4 cb3[77];
}

float4 ApplyMask(float4 v, float4 andMask, float4 orMask)
{
  return asfloat((asuint(v) & asuint(andMask)) | asuint(orMask));
}

// Vanilla per channel pow: exp2(log2(|x|) * e), with the DXBC -inf guard
float VanillaPowChannel(float x, float e)
{
  float l = log2(abs(x));
  l = (asuint(l) == 0xff800000u) ? -9999999933815812510711506376257961984.0 : l;
  return exp2(l * e);
}

float3 SignPow(float3 x, float e)
{
  return pow(abs(x), e) * sign(x);
}

// Vanilla alpha-driven bloom blend, colour matrix and grade offset. x = scene*cb4[116].x + bloom
float3 GradePre(float3 x, float4 bloomS)
{
  float2 bt = bloomS.w + float2(-0.65, -0.92);
  float2 bw = saturate(float2(6.0, 10.0) * bt + float2(0.65, 0.92));
  x *= 1.0 - bw.x * 0.4;
  x *= 1.0 - bw.y;

  float3 e = x * cb4[117].x;
  float blendW = saturate(dot(cb4[120].xyz, e));
  float3 m = e.x * cb4[16].xyz + e.y * cb4[17].xyz + e.z * cb4[18].xyz;
  float3 offs = m + cb4[19].xyz;
  return blendW * (x * cb4[117].x - offs) + offs; // Pre-clamp colour
}

#if TONEMAP_TYPE >= 1
#ifndef BLACK_FLOOR_TOE_RANGE
#define BLACK_FLOOR_TOE_RANGE 0.15 // gamma-space range the toe acts on
#endif
#ifndef BLOOM_OVERFLOW_SCALE
#define BLOOM_OVERFLOW_SCALE 1.0 // scale of bloom above white, added linearly
#endif
// Cubic toe: 0->0, t->t, slope 1 at t, slope a at 0. Monotonic.
float BlackFloorToe(float x, float strength)
{
  float t = BLACK_FLOOR_TOE_RANGE;
  if (x >= t || strength <= 0.0)
    return x;
  float u = max(x, 0.0) / t;
  float a = 1.0 - saturate(strength);
  return t * (a * u + 2.0 * (1.0 - a) * u * u + (a - 1.0) * u * u * u);
}

// Black Floor max-channel toe
float3 ApplyBlackFloor(float3 c)
{
  float cMax = max(max(c.r, c.g), c.b);
  if (cMax > 0)
    c *= BlackFloorToe(cMax, LumaSettings.GameSettings.BlackFloor) / cMax;
  return c;
}
#endif

void main(
  float4 v0 : SV_POSITION0,
  float4 v1 : TEXCOORD8,
  float4 v2 : COLOR0,
  float4 v3 : COLOR1,
  float4 v4 : TEXCOORD9,
  float4 v5 : TEXCOORD0,
  float4 v6 : TEXCOORD1,
  float4 v7 : TEXCOORD2,
  float4 v8 : TEXCOORD3,
  float4 v9 : TEXCOORD4,
  float4 v10 : TEXCOORD5,
  float4 v11 : TEXCOORD6,
  float4 v12 : TEXCOORD7,
  out float4 o0 : SV_TARGET0)
{
#if TONEMAP_TYPE >= 1
  float skyMask = 0;
  if (LumaSettings.GameSettings.SkyDepthActive > 0.5)
    skyMask = t8.SampleLevel(s3_s, v5.xy, 0) >= SKY_DEPTH_THRESHOLD ? 1.0 : 0.0;
#endif

  // Scene and bloom
  float4 sceneS = ApplyMask(t3.Sample(s3_s, v5.xy), cb3[50], cb3[51]);
  float4 bloomS = ApplyMask(t4.Sample(s4_s, v6.xy), cb3[52], cb3[53]);
  float3 bloom = bloomS.xyz;
#if TONEMAP_TYPE >= 1
  bloom *= LumaSettings.GameSettings.BloomIntensity;
  bloom *= lerp(1.0, SKY_BLOOM_SCALE, skyMask);
#endif
  float3 x = sceneS.xyz * cb4[116].x + bloom;

  // Vanilla alpha blend, colour matrix and grade offset (scene + bloom)
  float3 pre = GradePre(x, bloomS);

#if TONEMAP_TYPE >= 1
  pre = ApplyBlackFloor(pre);
  // Same grade with the scene only (no bloom): bloom is added in linear after the expansion
  float3 preScene = ApplyBlackFloor(GradePre(sceneS.xyz * cb4[116].x, bloomS));
  preScene = lerp(preScene, pre, skyMask); // Sky keeps bloom inside its soft shoulder (Sky Brightness cap)
#if DEVELOPMENT
  if (LumaSettings.DevSetting01 > 0.5) preScene = pre; // A/B: old gamma-space bloom composite
#endif
#endif

  // Noise (dither texture)
  float2 nuv = (v5.xy + cb4[121].zw) * cb4[121].xy;
  float4 nS = ApplyMask(t2.Sample(s2_s, nuv), cb3[48], cb3[49]);
  float3 noise = (nS.xyz * 2.0 - 1.0) * cb4[118].x;

#if TONEMAP_TYPE >= 1
  // SDR chain: vanilla clamp. HDR chain: only the lower clamp.
  float3 sdrG = SignPow(min(max(pre, cb4[122].x), cb4[122].y) * cb4[119].xyz + noise, cb4[123].x);
  float3 hdrG = SignPow(max(preScene, cb4[122].x) * cb4[119].xyz + noise, cb4[123].x);

  float3 sdrLin = gamma_to_linear(sdrG, GCT_MIRROR);
  float hs = LumaSettings.GameSettings.HighlightsStrength;
  float3 hiPart = 1.0 + (hdrG - 1.0) * 2.2 * hs;
  float sb = max(LumaSettings.GameSettings.SkyBrightness, 1e-4);
  float3 skyHi = 1.0 + sb * (1.0 - exp(-(max(hdrG - 1.0, 0.0) * 2.2) / sb));
  hiPart = lerp(hiPart, skyHi, skyMask);
  float3 loPart = gamma_to_linear(min(hdrG, 1.0), GCT_MIRROR);
  float3 hdrLin = lerp(hiPart, loPart, step(hdrG, 1.0));

  // Bloom added in linear: the brightening vanilla shows below white, plus bloom above white (linear, not the highlight slope)
  float3 gFull = max(pre, 0) * cb4[119].xyz;
  float3 gScene = max(preScene, 0) * cb4[119].xyz;
  float3 bloomLin = gamma_to_linear(min(gFull, 1.0), GCT_MIRROR) - gamma_to_linear(min(gScene, 1.0), GCT_MIRROR);
  bloomLin *= 1.0 - skyMask;
  bloomLin += max(max(gFull, 1.0) - max(gScene, 1.0), 0.0) * BLOOM_OVERFLOW_SCALE;
#if DEVELOPMENT
  if (LumaSettings.DevSetting01 <= 0.5)
#endif
  hdrLin += max(bloomLin, 0.0);

  float s = LumaSettings.GameSettings.VanillaHueRestore;
  float sHue = s * lerp(1.0, SKY_HUE_RESTORE_SCALE, skyMask);
  hdrLin = RestoreHueAndChrominance(hdrLin, sdrLin, 0.75 * sHue, sHue);

  float3 result = max(sdrLin, 0.0);
  if (LumaSettings.DisplayMode == 1)
  {
    // Hue-exact Oklch chroma expansion, bounded by the BT.2020 gamut
    float ge = LumaSettings.GameSettings.GamutExpansion;
    float3 c2020 = ExpandGamutBT709ToBT2020(max(hdrLin, 0.0), ge);
    c2020 = SimpleGamutClip(GamutCompressBT2020(c2020), true);

    const float paperWhite = LumaSettings.GamePaperWhiteNits / sRGB_WhiteLevelNits;
    const float peakWhite = LumaSettings.PeakWhiteNits / sRGB_WhiteLevelNits;
    DICESettings settings = DefaultDICESettings(DICE_TYPE_BY_LUMINANCE_PQ_CORRECT_CHANNELS_BEYOND_PEAK_WHITE);
    settings.ShoulderStart = paperWhite / peakWhite;
    result = DICETonemap(BT2020_To_BT709(c2020) * paperWhite, peakWhite, settings) / paperWhite;
    // Final BT.2020 cap
    result = BT2020_To_BT709(SimpleGamutClip(GamutCompressBT2020(BT709_To_BT2020(result)), true));
  }

  // Inverse of Display Composition's decode (gamma 2.2 in 0-1, sRGB above 1 with GAMMA_CORRECTION_RANGE_TYPE 1), so the peak lands exactly
  o0.rgb = ColorGradingLUTTransferFunctionInCorrected(result, GAMMA_CORRECTION_TYPE, VANILLA_ENCODING_TYPE);
  o0.a = sqrt(dot(saturate(sdrG), float3(0.3, 0.59, 0.11)));
#else
  // Exact vanilla
  float3 g = min(max(pre, cb4[122].x), cb4[122].y) * cb4[119].xyz + noise;
  float3 o = float3(
    VanillaPowChannel(g.x, cb4[123].x),
    VanillaPowChannel(g.y, cb4[123].x),
    VanillaPowChannel(g.z, cb4[123].x));
  o0.xyz = o;

  float d = dot(o, float3(0.3, 0.59, 0.11));
  float rs = rsqrt(abs(d));
  rs = (asuint(rs) == 0x7f800000u) ? 9999999933815812510711506376257961984.0 : rs;
  o0.w = (0.0 < abs(rs)) ? (1.0 / rs) : 9999999933815812510711506376257961984.0;
#endif
}
