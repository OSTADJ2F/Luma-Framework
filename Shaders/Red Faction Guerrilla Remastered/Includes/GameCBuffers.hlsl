#ifndef LUMA_GAME_CB_STRUCTS
#define LUMA_GAME_CB_STRUCTS

#ifdef __cplusplus
// This include is needed to allow reading shader types from c++.
#include "../../../Source/Core/includes/shader_types.h"
#endif

// Mirrors c++ name spaces.
namespace CB
{
	// Define the game specific cbuffer settings here. They don't need 4 bytes alignment.
	struct LumaGameSettings
	{
		float VanillaHueRestore; // 0..1, pulls hue and chrominance toward the vanilla SDR grade
		float BloomStrength;     // 0..2, scales the bloom term in the HDR path
		float Exposure;          // 0.25..4, scales the scene term in the HDR path
		uint DisplayMapping;     // 0 = DICE, 1 = Reinhard piecewise, 2 = BT.2390
		uint LumaBloom;          // 0 = game bloom, 1 = Luma bloom pyramid (tonemap HDR path only)
		float LumaBloomStrength; // 0..2, scales the Luma bloom term
		uint AnamorphicStreak;   // 0 = off, 1 = on (HDR tonemap path; also hides flare sprite 0xC1D2B908)
		float StreakThreshold;   // 0..10, streak prefilter threshold in paper white units
		float StreakStretch;     // 0..1, streak upsample stretch
		float StreakIntensity;   // 0..2, streak strength applied in the tonemap
		uint Glow;               // 0 = off, 1 = on (HDR tonemap path; sun glow and lens flare layer)
		float ParticleBrightness; // 0..2, scales the particle layer (glow term) in the HDR tonemap path. 1 = vanilla
	};

	// Define the game specific cbuffer (instance/pass) data here
	struct LumaGameData
	{
		float Dummy; // hlsl doesn't support empty structs
	};
}

#endif // LUMA_GAME_CB_STRUCTS
