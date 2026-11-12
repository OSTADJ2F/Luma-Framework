// Decodes the game's gamma 2.2 scene (sign mirrored, HDR) for SMAA (see DrawSMAAForFXAA in main.cpp):
// uav0 = linear copy (HDR and negatives kept, alpha untouched) for the blending,
// uav1 = saturated gamma copy (alpha untouched) for the SDR-range edge detection.

#include "Includes/Common.hlsl"

Texture2D<float4> scene_tex : register(t0);
RWTexture2D<float4> linear_out : register(u0);
RWTexture2D<float4> gamma_sdr_out : register(u1);

[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
   uint width, height;
   scene_tex.GetDimensions(width, height);
   if (dtid.x >= width || dtid.y >= height)
      return;

   float4 color = scene_tex.Load(int3(dtid.xy, 0));
   linear_out[dtid.xy] = float4(gamma_to_linear(color.rgb, GCT_MIRROR), color.a);
   gamma_sdr_out[dtid.xy] = float4(saturate(color.rgb), color.a);
}
