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
#define SKY_BLOOM_SCALE 0.25
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

  // Alpha driven blend of the bloom alpha (vanilla)
  float2 bt = bloomS.w + float2(-0.65, -0.92);
  float2 bw = saturate(float2(6.0, 10.0) * bt + float2(0.65, 0.92));
  x *= 1.0 - bw.x * 0.4;
  x *= 1.0 - bw.y;

  // Vanilla color matrix and grade offset
  float3 e = x * cb4[117].x;
  float blendW = saturate(dot(cb4[120].xyz, e));
  float3 m = e.x * cb4[16].xyz + e.y * cb4[17].xyz + e.z * cb4[18].xyz;
  float3 offs = m + cb4[19].xyz;
  float3 pre = blendW * (x * cb4[117].x - offs) + offs; // Pre-clamp colour

  // Noise (dither texture)
  float2 nuv = (v5.xy + cb4[121].zw) * cb4[121].xy;
  float4 nS = ApplyMask(t2.Sample(s2_s, nuv), cb3[48], cb3[49]);
  float3 noise = (nS.xyz * 2.0 - 1.0) * cb4[118].x;

#if TONEMAP_TYPE >= 1
  // SDR chain: vanilla clamp. HDR chain: only the lower clamp.
  float3 sdrG = SignPow(min(max(pre, cb4[122].x), cb4[122].y) * cb4[119].xyz + noise, cb4[123].x);
  float3 hdrG = SignPow(max(pre, cb4[122].x) * cb4[119].xyz + noise, cb4[123].x);

  float3 sdrLin = gamma_to_linear(sdrG, GCT_MIRROR);
  float hs = LumaSettings.GameSettings.HighlightsStrength;
  float3 hiPart = 1.0 + (hdrG - 1.0) * 2.2 * hs;
  float sb = max(LumaSettings.GameSettings.SkyBrightness, 1e-4);
  float3 skyHi = 1.0 + sb * (1.0 - exp(-(max(hdrG - 1.0, 0.0) * 2.2) / sb));
  hiPart = lerp(hiPart, skyHi, skyMask);
  float3 loPart = gamma_to_linear(min(hdrG, 1.0), GCT_MIRROR);
  float3 hdrLin = lerp(hiPart, loPart, step(hdrG, 1.0));

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
