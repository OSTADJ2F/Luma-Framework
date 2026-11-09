// Red Faction Guerrilla Remastered - Luma HDR bloom pyramid (core DrawBloom). Input = the tonemap pass's HDR scene (PS t0 of 0xDAC0B2C4),
// Karis-averaged. The scene is stored /8 in that texture (the tonemap multiplies by 8), so the thresholds are the HMA values divided by 8.

// clang-format off
#include "Includes/Common.hlsl" // game-local: pulls GameCBuffers (LumaSettings) before the shared includes
// clang-format on

// Input is exposed paper-white units (Luma_RFG_BloomSource), incl. the particle layer (0..1). Added on top of the game bloom, so it only
// blooms what goes above SDR white (HDR highlights); the game bloom keeps handling the soft glows.
#define LUMA_BLOOM_THRESHOLD 1.0
#define LUMA_BLOOM_SOFT_KNEE 0.5

#include "../Includes/Bloom.hlsl"
