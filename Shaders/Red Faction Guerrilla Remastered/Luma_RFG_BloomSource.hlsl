// Luma bloom source for RFG (ps_5_0). Folds the particle layer (smoke, dust, glows, pickup effects) into the HDR scene,
// so the Luma bloom pyramid sees it like the game's own bloom does. Output is written in the same /8 unexposed units as t0.
// Input t0 = HDR scene (stored /8, unexposed, as the tonemap reads it). Input t1 = 1x1 exposure, .y = k.
// Input t6 = particle layer, display-referred 0..1, screen-blended over the scene, t6.a multiplies the scene.
// t0 * 8 * k = exposed scene; the particle layer is already display-referred (0..1), so both are summed in exposed paper-white units.

#include "Includes/RFGStreak.hlsl"

Texture2D<float4> t0 : register(t0);
Texture2D<float4> t1 : register(t1);
Texture2D<float4> t6 : register(t6);
SamplerState s0 : register(s0); // point, scene
SamplerState s1 : register(s1); // linear, exposure and particle layer

float4 main(float4 pos : SV_Position) : SV_Target
{
  float2 uv = pos.xy * OutSize.zw;
  float3 scene = t0.Sample(s0, uv).rgb;
  float k = t1.Sample(s1, float2(0.5, 0.5)).y;
  float4 particles = t6.Sample(s1, uv);
  // Exposed paper-white units (what the player sees, before display mapping), so the bloom threshold is scene-independent.
  // The tonemap divides the bloom by k to put it back into its pre-exposure x units.
  float3 result = scene * saturate(particles.a) * 8.0 * k + saturate(particles.rgb);
  return float4(result, 1.0);
}
