#ifndef RFG_OUTPUT_HLSL
#define RFG_OUTPUT_HLSL

// Shared HDR output path for the Red Faction Guerrilla tonemap.
// Ported from Hitman Absolution HMAOutput.hlsl. Gamut expansion, gamma-by-luminance and grain helpers are left out
// (no matching settings in this mod); their constants are fixed to the HMA defaults below.
// Includers must include "Common.hlsl" (mod) first; the shared headers are guarded and safe to repeat.
#include "../../Includes/Tonemap.hlsl"
#include "../../Includes/Oklab.hlsl"
#include "../../Includes/Reinhard.hlsl"

#define RFG_SHOULDER_START 0.75     // DisplayMapping 1: fraction of the display peak where the shoulder starts
#define RFG_HIGHLIGHT_PURITY 0.5    // DisplayMapping 1 and 2: amount of u'v' purity transferred from the source
#define RFG_HUE_LOCK 0.5            // Keeps the pre-mapping hue after the display curve
// Source peak as a multiple of the display peak (there is no histogram, so this is always the fallback).
#define RFG_EETF_FALLBACK_SRC_PEAK_SCALE 8.0

// Largest OKLCH chroma at (L, h) whose linear BT.709 (or BT.2020) conversion has no negative channel.
float RFG_MaxChromaOklch(float L, float h, bool bt2020)
{
  float lo = 0.0;
  float hi = 0.5 * max(L, 1.0);
  for (int i = 0; i < 14; i++)
  {
    float mid = 0.5 * (lo + hi);
    float3 rgb = bt2020 ? Oklab::oklch_to_linear_bt2020(float3(L, mid, h)) : Oklab::oklch_to_linear_srgb(float3(L, mid, h));
    if (min(rgb.r, min(rgb.g, rgb.b)) >= 0.0) lo = mid;
    else hi = mid;
  }
  return lo;
}

// Maps a linear BT.709 color into BT.2020 at constant OkLab lightness and hue, reducing chroma only where needed.
float3 RFG_GamutMapToBT2020(float3 lin709)
{
  float Y = GetLuminance(lin709);
  if (Y <= 0.0) return 0.0;
  float3 c2 = BT709_To_BT2020(lin709);
  // Margin: colors at the exact boundary crossed it after fp16 storage and the gamma encode (AP0 in gamut views).
  if (min(c2.r, min(c2.g, c2.b)) >= 0.005 * Y) return lin709;
  float3 lch = Oklab::linear_srgb_to_oklch(lin709);
  lch.y = min(lch.y, 0.98 * RFG_MaxChromaOklch(lch.x, lch.z, true));
  return BT2020_To_BT709(max(Oklab::oklch_to_linear_bt2020(lch), 0.0));
}

// Identity below the knee, smoothly approaching peak while retaining highlight separation.
float RFG_ShoulderToPeak(float x, float peak, float s, float strength)
{
  if (x <= s) return x;
  float excess = x - s; float room = max(peak - s, 1e-4);
  return s + room * (1.0 - pow(1.0 + excess / (room * strength), -strength));
}

// BT.2390 EETF (adaptive, PQ domain). x, srcPeak, dstPeak are in paper-white units; returns paper-white units.
float RFG_EETF_BT2390(float x, float srcPeak, float dstPeak)
{
  if (x <= 0.0) return x;
  float pw = LumaSettings.GamePaperWhiteNits;
  float srcPQ = Linear_to_PQ(max(srcPeak * pw / 10000.0, 1e-7), GCT_POSITIVE);
  float dstPQ = Linear_to_PQ(max(dstPeak * pw / 10000.0, 1e-7), GCT_POSITIVE);
  if (srcPQ <= dstPQ) return x;
  float e = Linear_to_PQ(x * pw / 10000.0, GCT_POSITIVE) / srcPQ;
  float maxLum = dstPQ / srcPQ;
  float KS = 1.5 * maxLum - 0.5;
  if (e > KS)
  {
    float t = (min(e, 1.0) - KS) / (1.0 - KS);
    float t2 = t * t; float t3 = t2 * t;
    e = (2.0 * t3 - 3.0 * t2 + 1.0) * KS + (t3 - 2.0 * t2 + t) * (1.0 - KS) + (-2.0 * t3 + 3.0 * t2) * maxLum;
  }
  float nits = PQ_to_Linear(e * srcPQ, GCT_POSITIVE) * 10000.0;
  return nits / pw;
}

// Keeps targetBT709's luminance and hue, and sets its CIE u'v' distance from white to the source's (scaled by amount).
float3 RFG_TransferPurityFP(float3 targetBT709, float3 sourceBT709, float amount)
{
  float3 xyzT = mul(BT709_2_XYZ, targetBT709);
  float3 xyzS = mul(BT709_2_XYZ, sourceBT709);
  float denT = max(xyzT.x + 15.0 * xyzT.y + 3.0 * xyzT.z, 1e-7);
  float denS = max(xyzS.x + 15.0 * xyzS.y + 3.0 * xyzS.z, 1e-7);
  float2 uvT = float2(4.0 * xyzT.x, 9.0 * xyzT.y) / denT;
  float2 uvS = float2(4.0 * xyzS.x, 9.0 * xyzS.y) / denS;
  float2 w = float2(0.1978, 0.4683);
  float rt = length(uvT - w);
  float rs = length(uvS - w);
  if (rt < 1e-6 || xyzT.y <= 0.0)
    return targetBT709;
  float2 uv = w + (uvT - w) * lerp(1.0, min(rs / rt, 1.0), saturate(amount));
  float Y = xyzT.y;
  float v = max(uv.y, 1e-7);
  float X = Y * 9.0 * uv.x / (4.0 * v);
  float Z = Y * (12.0 - 3.0 * uv.x - 20.0 * uv.y) / (4.0 * v);
  float3 c = mul(XYZ_2_BT709, float3(X, Y, Z));
  // Gamut compress: desaturate toward the same luminance until no channel is negative (keeps hue and luminance).
  float3 c2 = BT709_To_BT2020(c);
  float minC = min(c2.r, min(c2.g, c2.b));
  return minC < 0.0 ? Y + (c - Y) * (Y / max(Y - minC, 1e-7)) : c;
}

// Linear scene (1 = paper white) to the encoded swapchain value. Input must be >= 0.
float3 RFG_DisplayOutput(float3 lin)
{
  // Hue reference is taken before the display curve so the hue lock can restore it afterwards.
  float3 hueRef = lin;
  lin = RFG_GamutMapToBT2020(lin);
  float peak = max(LumaSettings.PeakWhiteNits / max(LumaSettings.GamePaperWhiteNits, 1e-4), 1e-4);
  uint mapping = LumaSettings.GameSettings.DisplayMapping;
  if (mapping == 0u)
  {
    lin = Tonemap_DICE(lin, peak);
  }
  else if (mapping == 1u)
  {
    float shoulder = RFG_SHOULDER_START * peak;
    float strength = 0.45; // no histogram: the HMA default strength
    float Y = GetLuminance(lin);
    if (Y > 0.0)
    {
      float3 lumMapped = lin * (RFG_ShoulderToPeak(Y, peak, shoulder, strength) / Y);
      float3 ch2020 = max(BT709_To_BT2020(lin), 0.0);
      float3 ch = BT2020_To_BT709(float3(RFG_ShoulderToPeak(ch2020.r, peak, shoulder, strength), RFG_ShoulderToPeak(ch2020.g, peak, shoulder, strength), RFG_ShoulderToPeak(ch2020.b, peak, shoulder, strength)));
      lin = RFG_TransferPurityFP(lumMapped, ch, RFG_HIGHLIGHT_PURITY);
    }
  }
  else
  {
    // No histogram: source peak is always the fallback multiple of the display peak.
    float srcPeak = max(peak * RFG_EETF_FALLBACK_SRC_PEAK_SCALE, peak * 1.01);
    float Y = GetLuminance(lin);
    if (Y > 0.0)
    {
      float3 lumMapped = lin * (RFG_EETF_BT2390(Y, srcPeak, peak) / Y);
      float3 ch2020 = max(BT709_To_BT2020(lin), 0.0);
      float3 ch = BT2020_To_BT709(float3(RFG_EETF_BT2390(ch2020.r, srcPeak, peak), RFG_EETF_BT2390(ch2020.g, srcPeak, peak), RFG_EETF_BT2390(ch2020.b, srcPeak, peak)));
      lin = RFG_TransferPurityFP(lumMapped, ch, RFG_HIGHLIGHT_PURITY);
    }
  }
  if (RFG_HUE_LOCK > 0.0 && GetLuminance(hueRef) > 0.0)
    lin = RestoreHueAndChrominance(lin, hueRef, RFG_HUE_LOCK, 0.0, 0.0, FLT_MAX, 0.0, CS_DEFAULT);
  lin = RFG_GamutMapToBT2020(lin);
  // Channel cap in BT.2020: soft knee toward luminance so the cap is gentle and runs last.
  float3 c2 = BT709_To_BT2020(lin);
  float m = max(c2.r, max(c2.g, c2.b));
  float Yl = GetLuminance(lin);
  float knee = max(0.75 * peak, Yl);
  if (m > knee && m > Yl)
  {
    float e = m - knee; float room = max(peak - knee, 1e-4);
    float mT = knee + e / (1.0 + e / room);
    lin = Yl + (lin - Yl) * saturate((mT - Yl) / (m - Yl));
  }
#if UI_DRAW_TYPE == 2 // The UI draws on top in gamma space; display composition scales everything by UI/game paper white, so pre-divide the scene
  lin *= LumaSettings.GamePaperWhiteNits / LumaSettings.UIPaperWhiteNits;
#endif
  return lin; // RFG post chain is linear (sRGB swapchain, POST_PROCESS_SPACE_TYPE 1): no gamma encode
}

#endif // RFG_OUTPUT_HLSL
