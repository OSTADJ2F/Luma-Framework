#include "Includes/Common.hlsl"

// Shadow resolve (replaces 0x203D9FAF). Faithful translation of the vanilla pixel shader, with a wider,
// resolution independent soft filter behind "ENABLE_SHADOW_FILTER" (0 = vanilla 10 tap code).
#ifndef ENABLE_SHADOW_FILTER
#define ENABLE_SHADOW_FILTER 1
#endif

Texture2D<float4> t0 : register(t0); // Scene depth
Texture2D<float4> t1 : register(t1); // Shadow atlas: 4 cascades (1024x1024 each) stacked vertically, R32_FLOAT depth

SamplerState s0_s : register(s0);
SamplerState s1_s : register(s1);

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

// 1 = lit (vanilla binary test)
float ShadowLit(float atlasDepth, float refDepth)
{
  return ((atlasDepth - refDepth) >= 0.0) ? 1.0 : 0.0;
}

float AtlasTap(float2 uv)
{
  return ApplyMask(t1.Sample(s1_s, uv), cb3[46], cb3[47]).x;
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
  // View depth from scene depth
  float depthS = ApplyMask(t0.Sample(s0_s, v5.xy), cb3[44], cb3[45]).x;
  float d = depthS + cb4[16].z;
  float w = (0.0 < abs(d)) ? rcp(d) : 9999999933815812510711506376257961984.0;
  float zView = w * cb4[16].w;

  // Cascade index = number of split planes (cb4[18].xyz) the view depth is past
  float3 splitDelta = cb4[18].xyz - zView;
  float3 splitPast = (splitDelta >= 0.0) ? 0.0 : 1.0;
  int cascade = (int)(splitPast.x + splitPast.y + splitPast.z);

  // Per cascade 4x4 matrix stored as cb4[116 + 4 * cascade .. + 3]: rows give atlas U, atlas V and reference depth
  int cb0 = 116 + 4 * cascade;
  float4 rowU = float4(cb4[cb0].x, cb4[cb0 + 1].x, cb4[cb0 + 2].x, cb4[cb0 + 3].x);
  float4 rowV = float4(cb4[cb0].y, cb4[cb0 + 1].y, cb4[cb0 + 2].y, cb4[cb0 + 3].y);
  float4 rowD = float4(cb4[cb0].z, cb4[cb0 + 1].z, cb4[cb0 + 2].z, cb4[cb0 + 3].z);

  float2 viewXY = cb4[17].zw * v5.zw + cb4[17].xy;
  float4 pos = float4(viewXY * (-zView), -zView, 1.0);

  float fade = saturate(zView * cb4[19].x - cb4[19].y);
  float refDepth = saturate(dot(pos, rowD));
  float2 uv = float2(saturate(dot(pos, rowU)), saturate(dot(pos, rowV)));

#if !ENABLE_SHADOW_FILTER
  // Vanilla: 3x3 taps on the atlas with a (1-f, 1, f) weighting per axis, normalised by 4
  float lC  = ShadowLit(AtlasTap(uv), refDepth);
  float lL  = ShadowLit(AtlasTap(uv + float2(-0.000977, 0.0)), refDepth);
  float lR  = ShadowLit(AtlasTap(uv + float2(0.000977, 0.0)), refDepth);
  float lDL = ShadowLit(AtlasTap(uv + float2(-0.000977, -0.000244)), refDepth);
  float lD  = ShadowLit(AtlasTap(uv + float2(0.0, -0.000244)), refDepth);
  float lDR = ShadowLit(AtlasTap(uv + float2(0.000977, -0.000244)), refDepth);
  float lUL = ShadowLit(AtlasTap(uv + float2(-0.000977, 0.000244)), refDepth);
  float lU  = ShadowLit(AtlasTap(uv + float2(0.0, 0.000244)), refDepth);
  float lUR = ShadowLit(AtlasTap(uv + float2(0.000977, 0.000244)), refDepth);

  float2 f = frac(frac(uv * float2(1024.0, 4096.0) + 0.5) + 0.5);
  float rowMid = (1.0 - f.x) * lL + lC + f.x * lR;
  float rowLo  = (1.0 - f.x) * lDL + lD + f.x * lDR;
  float rowHi  = (1.0 - f.x) * lUL + lU + f.x * lUR;
  float Lf = 0.25 * ((1.0 - f.y) * rowLo + rowMid + f.y * rowHi);
#else
  // Box filter over the atlas texels covered by a box W texels wide (W = 2 at vanilla width, 1024 wide atlas).
  // Area weighted average of the binary lit test. Width scales with the atlas size, so results match at any resolution.
  uint atlasW, atlasH;
  t1.GetDimensions(atlasW, atlasH);
  float atlasScale = (float)atlasW / 1024.0;
  float boxW = 2.0 * clamp(LumaSettings.GameSettings.ShadowSoftness, 0.5, 2.0) * atlasScale;
  float2 p = uv * float2(atlasW, atlasH);
  // The mod removes dgVoodoo's +0.5 px viewport offset on the resized atlas, so shift the lookup back by the same world-space amount (half an original texel).
  if (atlasScale > 1.0) p -= 0.5 * atlasScale;
  float2 boxMin = p - boxW * 0.5;
  float2 boxMax = p + boxW * 0.5;
  int2 kMin = (int2)floor(boxMin);
  int2 kMax = (int2)floor(boxMax);

  // Bleed guard: keep rows inside the cascade quarter that contains this pixel
  int cascadeQ = clamp((int)floor(uv.y * 4.0), 0, 3);
  int rowsPerCascade = (int)(atlasH >> 2);
  int rowMin = cascadeQ * rowsPerCascade;
  int rowMax = rowMin + rowsPerCascade - 1;

  float sumLit = 0.0;
  [loop]
  for (int ky = 0; ky < 9; ky++)
  {
    int ty = kMin.y + ky;
    if (ty > kMax.y) break;
    float wy = clamp(min((float)ty + 1.0, boxMax.y) - max((float)ty, boxMin.y), 0.0, 1.0);
    int tyC = clamp(ty, rowMin, rowMax);
    [loop]
    for (int kx = 0; kx < 9; kx++)
    {
      int tx = kMin.x + kx;
      if (tx > kMax.x) break;
      float wx = clamp(min((float)tx + 1.0, boxMax.x) - max((float)tx, boxMin.x), 0.0, 1.0);
      int txC = clamp(tx, 0, (int)atlasW - 1);
      float atlasDepth = ApplyMask(t1.Load(int3(txC, tyC, 0)), cb3[46], cb3[47]).x;
      sumLit += wx * wy * ShadowLit(atlasDepth, refDepth);
    }
  }
  float Lf = sumLit / (boxW * boxW);
#endif

  // Vanilla mix with the distance fade, then the vanilla alpha test
  float S = fade * (1.0 - Lf) + Lf;
  uint alphaQ = (uint)max(S * 255.0 + 0.0001, 0.0);
  if (alphaQ >= asuint(cb3[8].z))
    discard;

  o0 = float4(S, S, S, S);
}
