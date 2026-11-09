// Red Faction Guerrilla Remastered - anamorphic streak passes (port of KinoStreak, MIT, see LICENSE_KinoStreak.txt).
// Constant buffer shared by Luma_RFG_Streak_*.hlsl. Mirrored by RFGStreak.hpp (RFGStreakCBData).

cbuffer RFGStreakCB : register(b0)
{
  float4 InSize;  // xy = input texture size (px), zw = 1/xy
  float4 OutSize; // xy = output texture size (px), zw = 1/xy (uv = pos.xy * zw)
  float4 Params;  // x = threshold (paper white units), y = stretch (0..1)
};
