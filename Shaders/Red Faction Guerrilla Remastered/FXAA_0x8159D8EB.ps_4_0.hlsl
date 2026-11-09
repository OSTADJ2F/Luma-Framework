// FXAA 3.x pixel shader, translated from Dump/0x8159D8EB.ps_4_0.cso.
// TONEMAP_TYPE 0: 1:1 translation of the original asm.
// TONEMAP_TYPE >= 1: the input alpha is an encoded luma L/(1+L) while rgb is HDR,
// so the luma used for the final ratio is encoded the same way.

Texture2D<float4> t0 : register(t0);
SamplerState s0 : register(s0);

cbuffer cb0 : register(b0)
{
  float4 cb0[2];
}

#ifndef TONEMAP_TYPE
#define TONEMAP_TYPE 0
#endif

void main(
  float4 v0 : SV_POSITION0,
  float2 v1 : TEXCOORD0,
  out float4 o0 : SV_TARGET0)
{
  float2 uv = v1.xy;

  // N = (0,-1), W = (-1,0), C = center, E = (1,0), S = (0,1)
  float4 N = t0.SampleLevel(s0, uv, 0, int2(0, -1));
  float4 W = t0.SampleLevel(s0, uv, 0, int2(-1, 0));
  float4 C = t0.SampleLevel(s0, uv, 0);
  float4 E = t0.SampleLevel(s0, uv, 0, int2(1, 0));
  float4 S = t0.SampleLevel(s0, uv, 0, int2(0, 1));

  float lumaMin = min(min(N.w, W.w), min(E.w, S.w));
  lumaMin = min(lumaMin, C.w);
  float lumaMax = max(max(N.w, W.w), max(E.w, S.w));
  lumaMax = max(lumaMax, C.w);
  float lumaRange = lumaMax - lumaMin;
  float edgeThreshold = max(lumaMax * 0.166667, 0.083333);

  float3 outRgb = C.xyz;

  if (lumaRange >= edgeThreshold)
  {
    float4 NW = t0.SampleLevel(s0, uv, 0, int2(-1, -1));
    float4 NE = t0.SampleLevel(s0, uv, 0, int2(1, -1));
    float4 SW = t0.SampleLevel(s0, uv, 0, int2(-1, 1));
    float4 SE = t0.SampleLevel(s0, uv, 0, int2(1, 1));

    float lumaSum = N.w + W.w + E.w + S.w;
    float subpix = abs(lumaSum * 0.25 - C.w) / lumaRange;
    subpix = saturate(subpix - 0.25);
    subpix = subpix * 1.333333;
    subpix = min(subpix, 0.75);

    // Edge orientation: second differences along the rows / columns.
    float a = (NW.w - 2.0 * N.w) + NE.w;
    float b = (W.w - 2.0 * C.w) + E.w;
    float c = (SW.w - 2.0 * S.w) + SE.w;
    float d = (NW.w - 2.0 * W.w) + SW.w;
    float e = (N.w - 2.0 * C.w) + S.w;
    float f = (NE.w - 2.0 * E.w) + SE.w;
    float edgeH = (2.0 * abs(b) + abs(a)) + abs(c);
    float edgeV = abs(f) + (2.0 * abs(e) + abs(d));
    bool isH = edgeV >= edgeH;

    float stepLen = isH ? -cb0[1].y : -cb0[1].x;
    float lumaN1 = isH ? N.w : W.w;
    float lumaN2 = isH ? S.w : E.w;
    float gradN = lumaN1 - C.w;
    float gradP = lumaN2 - C.w;
    float avgN = (C.w + lumaN1) * 0.5;
    float avgP = (C.w + lumaN2) * 0.5;
    bool steepN = abs(gradN) >= abs(gradP);
    float lumaNN = steepN ? avgN : avgP;
    float gradScaled = max(abs(gradP), abs(gradN));
    stepLen = steepN ? stepLen : -stepLen;
    float gradThr = gradScaled * 0.25;
    float halfStep = stepLen * 0.5;

    float2 offsetPos = float2(uv.x + (isH ? 0.0 : halfStep), uv.y + (isH ? halfStep : 0.0));
    float2 dirv = isH ? float2(cb0[1].x, 0.0) : float2(0.0, cb0[1].y);
    float2 posN = offsetPos - 1.5 * dirv;
    float2 posP = offsetPos + 1.5 * dirv;

    bool doneN = false;
    bool doneP = false;
    float lumaEndN = 0.0;
    float lumaEndP = 0.0;

    [loop]
    for (int i = 0; i < 6; i++)
    {
      float4 s7 = t0.SampleLevel(s0, posN, 0);
      float4 s8 = t0.SampleLevel(s0, posP, 0);
      float dN = s7.w - lumaNN;
      float dP = s8.w - lumaNN;
      bool reachN = abs(dN) >= gradThr;
      bool reachP = abs(dP) >= gradThr;
      bool advN = !doneN && reachN;
      bool advP = !doneP && reachP;
      float2 pN7 = advN ? (posN + dirv) : posN;
      float2 pP8 = advP ? (posP - dirv) : posP;

      if (reachN && reachP)
      {
        posN = pN7;
        posP = pP8;
        lumaEndN = s7.w;
        lumaEndP = s8.w;
        break;
      }

      float2 rN = pN7 - 2.0 * dirv;
      float2 rP = pP8 + 2.0 * dirv;
      posN = reachN ? pN7 : rN;
      posP = reachP ? pP8 : rP;
      doneN = reachN;
      doneP = reachP;
      lumaEndN = s7.w;
      lumaEndP = s8.w;
    }

    float distN = isH ? (uv.x - posN.x) : (uv.y - posN.y);
    float distP = isH ? (posP.x - uv.x) : (posP.y - uv.y);
    float lumaEnd = (distN < distP) ? lumaEndN : lumaEndP;

    bool wrongCenter = (C.w - lumaNN) < 0.0;
    bool wrongEnd = (lumaEnd - lumaNN) < 0.0;
    float offStep = (wrongCenter == wrongEnd) ? 0.0 : stepLen;

    float total = distN + distP;
    float dmin = min(distN, distP);
    float pixOff = dmin * (-1.0 / total) + 0.5;
    pixOff = subpix * 0.125 + pixOff;
    offStep = offStep * pixOff;

    float2 finalUV = float2(uv.x + (isH ? 0.0 : offStep), uv.y + (isH ? offStep : 0.0));
    float4 F = t0.SampleLevel(s0, finalUV, 0);

    float lum = dot(F.xyz, float3(0.299, 0.587, 0.114));

    float ratio;
#if TONEMAP_TYPE >= 1
    // Encode the HDR luma the same way as the alpha channel (L/(1+L)).
    float lumE = max(lum, 0.0);
    lumE = lumE / (1.0 + lumE);
    float denom = max(lumE, 1e-6);
    ratio = (subpix * (lumaSum * 0.25 - lumE) + lumE) / denom;
#else
    ratio = (subpix * (lumaSum * 0.25 - lum) + lum) / lum;
#endif
    ratio = min(ratio, 4.0);

    outRgb = ratio * F.xyz;
  }

  o0 = float4(outRgb, 1.0);
}
