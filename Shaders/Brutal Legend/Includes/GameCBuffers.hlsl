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
		float HighlightsStrength; // 0 = vanilla clip, 1 = full overbright range
		float VanillaHueRestore; // 0-1
		float BloomIntensity; // 1 = vanilla
		float GamutExpansion; // 0-1
		float XeGTAOActive; // set by c++ each frame: 1 when the XeGTAO texture is bound
		float XeGTAOIntensity;
		float XeGTAORadius;
		float ShadowSoftness; // 1 = vanilla penumbra width
		float VideoAutoHDREnable; // 0/1
		float VideoAutoHDR; // 0-1 strength
		float VideoDrawActive; // set by c++ per draw, never saved
		float SkyBrightness; // sky highlight headroom above SDR white, in paper white units (0 = vanilla clip)
		float SkyDepthActive; // set by c++ per tonemap draw: 1 when the scene depth is bound at t8
		float BlackFloor; // 0 = vanilla, 1 = strongest toe
		float XeGTAODarkness; // 1 = neutral, 0 = no AO, 2 = twice as dark
	};
	
	// Define the game specific cbuffer (instance/pass) data here
	struct LumaGameData
	{
		float Dummy; // hlsl doesn't support empty structs
	};
}

#endif // LUMA_GAME_CB_STRUCTS
