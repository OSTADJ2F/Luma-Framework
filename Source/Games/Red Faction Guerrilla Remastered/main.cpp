///////////////////////////////////////////////////////////////////////
// Of course, before any of this, create a new project for your game!
// - Copy "Templates\VisualStudio\Luma-Template.zip" into "%USERPROFILE%\Documents\Visual Studio [current version]\Templates\ProjectTemplates" (and restart Visual Studio).
// - Right click "Games" in the solution explorer, Add -> New Project... -> Template (Luma Template Project for new games)
// - Make sure the location is "Source\Games"!
///////////////////////////////////////////////////////////////////////

// Specify a define with the name of the game here (GAME_*).
// This is optional but can be used to hardcode custom (per game) behaviours in the core library,
// or other reasons. We can't automate it through the project define system (based on the project name) because
// we need these defines to be upper case and space free.
#define GAME_TEMPLATE 1
#define ENABLE_BLOOM 1 // core registers the Bloom VS/Prefilter/Downsample/Upsample passes used by DrawBloom (Luma Bloom setting)
#if defined(_M_IX86) && !defined(LUMA_SEGMENTED_TRACE)
#define LUMA_SEGMENTED_TRACE 1 // x86: segmented trace storage avoids bad_alloc on MCP captures (local core patch)
#endif
#if defined(_M_IX86) && DEVELOPMENT
#define GAME_BURNOUT_PARADISE_REMASTERED 1 // Only used by trace.hpp: caps traced SRV/UAV slots at 16 so captures fit the x86 address space
#endif

//// Define all the global "core" defines before including its files: ////

// Enable support for Geometry Shaders (GS), rather rare.
#define GEOMETRY_SHADER_SUPPORT 0

// Enable to stop "You can now attach the debugger" popup.
#define DISABLE_AUTO_DEBUGGER 0

// DLSS/FSR are toggled via the "Luma Properties" page in the project properties (UseLumaNGX/UseLumaFSR).

// Instead of "manually" including the "core" library, we simply include its main code file (which is a header).
// The library in itself is standalone, as in, it compiles fine and could directly be used as a template addon etc if built as dll but,
// there's a major limitation in how libraries dependencies work by nature, and that is that you can only make
// one version of them for all other projects to use. For performance and tidiness reasons, we are interested in
// having global defines that can be turned on and off per game, as opposed to runtime (static) parameters.
// Hence why we specify the global defines before including the core Luma file (where near all of the generic Luma implementation is).
// If we wanted to use a library, we'd also need to add a core "main" definition in a cpp file, to link it properly.
// All externs that are currently defined in core would also need to be manually defined in each game's implementation (e.g. see "RESHADE_EXTERNS" above).
// The only disadvantage of not actually including the core as a library, is that we'll have to add the same include/library dependencies in our
// game project (e.g. add ReShade, DLSS, etc), and manually add all cpp files too.
//
// To compile in different modes (e.g. "DEVELOPMENT", "TEST" etc see "global_defines.h").
#include "..\..\Core\core.hpp"

// Anamorphic streak (KinoStreak port, MIT). Needs the core above.
#include "RFGStreak.hpp"

// Luma bloom: pyramid mip 0 is bound at kLumaBloomSlot for the tonemap draw. One sigma per mip, in mip texels (resolution-independent). Copied from Hitman Absolution.
static constexpr uint32_t kLumaBloomSlot = 4;
static constexpr uint32_t kStreakSlot = 3; // Anamorphic streak (full res), read by the tonemap HDR path
static constexpr float g_bloom_sigmas[] = {1.5f, 2.f, 2.f, 2.f, 1.f, 0.5f};
static constexpr uint32_t kTonemapHash = 0xDAC0B2C4;
constexpr uint32_t TONEMAP_TYPE_HASH = char_ptr_crc32("TONEMAP_TYPE");

// Mod-owned full-res target for the Luma bloom input: HDR scene (t0, /8) plus the particle layer (t6). Released with the Luma bloom resources.
struct RFGBloomSourceResources
{
   RFGStreakTarget target; // W x H, R16G16B16A16_FLOAT
   ComPtr<ID3D11Buffer> cb;
   ComPtr<ID3D11SamplerState> smp_point; // s0 (scene)
   ComPtr<ID3D11SamplerState> smp_linear; // s1 (exposure, particle layer)

   void Release()
   {
      RFGStreak_ResetTarget(target);
      cb.reset();
      smp_point.reset();
      smp_linear.reset();
   }
};

// Renders Shaders/Red Faction Guerrilla Remastered/Luma_RFG_BloomSource.hlsl into res.target. Returns its SRV (non-owning) or nullptr on failure.
static ID3D11ShaderResourceView* RFGBloomSource_Draw(ID3D11Device* device, ID3D11DeviceContext* ctx, DeviceData& device_data, RFGBloomSourceResources& res,
   ID3D11ShaderResourceView* scene_srv, ID3D11ShaderResourceView* exposure_srv, ID3D11ShaderResourceView* particles_srv)
{
   if (device == nullptr || ctx == nullptr || scene_srv == nullptr || exposure_srv == nullptr || particles_srv == nullptr)
      return nullptr;

   ID3D11PixelShader* ps = RFGStreak_FindPS(device_data, CompileTimeStringHash("RFG Bloom Source"));
   ID3D11VertexShader* vs_copy = RFGStreak_FindVS(device_data, CompileTimeStringHash("Copy VS"));
   if (!ps || !vs_copy)
      return nullptr;

   UINT w = 0, h = 0;
   if (!RFGStreak_GetSRVSize(scene_srv, w, h))
      return nullptr;

   // Scene-size independent objects, created once.
   if (!res.cb)
   {
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = sizeof(RFGStreakCBData);
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ComPtr<ID3D11Buffer> cb;
      if (FAILED(device->CreateBuffer(&desc, nullptr, cb.put())))
         return nullptr;
      res.cb = cb;
   }
   auto create_sampler = [&](D3D11_FILTER filter, ComPtr<ID3D11SamplerState>& out) -> bool
   {
      if (out)
         return true;
      D3D11_SAMPLER_DESC desc = {};
      desc.Filter = filter;
      desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.MaxAnisotropy = 1;
      desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      desc.MaxLOD = D3D11_FLOAT32_MAX;
      return SUCCEEDED(device->CreateSamplerState(&desc, out.put()));
   };
   if (!create_sampler(D3D11_FILTER_MIN_MAG_MIP_POINT, res.smp_point) || !create_sampler(D3D11_FILTER_MIN_MAG_MIP_LINEAR, res.smp_linear))
      return nullptr;

   if (!RFGStreak_EnsureTarget(device, res.target, w, h))
      return nullptr;

   DrawStateStack<DrawStateStackType::FullGraphics> draw_state_stack;
   draw_state_stack.Cache(ctx, device_data.uav_max_count);

   ID3D11SamplerState* samplers[2] = { res.smp_point.get(), res.smp_linear.get() };
   ctx->PSSetSamplers(0, 2, samplers);
   ID3D11Buffer* cb = res.cb.get();
   ctx->PSSetConstantBuffers(0, 1, &cb);

   RFGStreakCBData cbd = {};
   cbd.OutSize[0] = (float)w;
   cbd.OutSize[1] = (float)h;
   cbd.OutSize[2] = 1.f / (float)w;
   cbd.OutSize[3] = 1.f / (float)h;
   RFGStreak_Upload(ctx, cb, cbd);

   // Clear the tonemap's inputs, then bind t1 and t6. t0 is bound by DrawCustomPixelShader.
   ctx->OMSetRenderTargets(0, nullptr, nullptr);
   ID3D11ShaderResourceView* srv_nulls[8] = {};
   ctx->PSSetShaderResources(0, 8, srv_nulls);
   ID3D11ShaderResourceView* srv_exposure = exposure_srv;
   ID3D11ShaderResourceView* srv_particles = particles_srv;
   ctx->PSSetShaderResources(1, 1, &srv_exposure);
   ctx->PSSetShaderResources(6, 1, &srv_particles);

   DrawCustomPixelShader(ctx, device_data.default_depth_stencil_state.get(), device_data.default_blend_state.get(), nullptr,
      vs_copy, ps, scene_srv, res.target.rtv.get(), res.target.w, res.target.h, true, "RFG Bloom Source");

   draw_state_stack.Restore(ctx);

   return res.target.srv.get();
}

struct TemplateGameDeviceData final : public GameDeviceData
{
   bool has_drawn_tonemap = false;
   // Non-owning view onto core's DrawBloom mip 0 (core-managed). Rebuilt every tonemap draw.
   ComPtr<ID3D11ShaderResourceView> srv_luma_bloom;
   RFGStreakResources streak_res;
   RFGBloomSourceResources bloom_source_res;
};

class TemplateGame final : public Game // Go view "Source/Core/includes/game.h" for all the overridable virtual functions
{
   // Optional helper to hide ugly casts
   static TemplateGameDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<TemplateGameDeviceData*>(device_data.game);
   }

   // Karis average, then the bloom pyramid. The input is the HDR scene (PS t0 of the tonemap, stored /8) with the particle layer (t6) folded in, so the particles bloom too.
   // The result is bound at kLumaBloomSlot for the draw.
   static void BindLumaBloom(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, ID3D11ShaderResourceView* srv_scene, ID3D11ShaderResourceView* srv_exposure, ID3D11ShaderResourceView* srv_particles)
   {
      auto& gd = GetGameDeviceData(device_data);
      gd.srv_luma_bloom.reset(); // DrawBloom AddRef's its mip 0 into this
      if (srv_scene)
      {
         // Falls back to the plain scene if the exposure or particle layer is missing.
         ID3D11ShaderResourceView* srv_input = nullptr;
         if (srv_exposure && srv_particles)
            srv_input = RFGBloomSource_Draw(native_device, native_device_context, device_data, gd.bloom_source_res, srv_scene, srv_exposure, srv_particles);
         if (!srv_input)
            srv_input = srv_scene;

         DrawStateStack<DrawStateStackType::FullGraphics> bloom_state;
         bloom_state.Cache(native_device_context, device_data.uav_max_count);

         ComPtr<ID3D11ShaderResourceView> srv_karis;
         DrawKarisAverage(native_device, native_device_context, device_data, srv_input, srv_karis.put());
         if (srv_karis)
            DrawBloom(native_device, native_device_context, device_data, srv_karis.get(), (int)std::size(g_bloom_sigmas), g_bloom_sigmas, gd.srv_luma_bloom.put());

         bloom_state.Restore(native_device_context);
      }
      ID3D11ShaderResourceView* bloom_srv = gd.srv_luma_bloom.get();
      native_device_context->PSSetShaderResources(kLumaBloomSlot, 1, &bloom_srv);
   }

   // Anamorphic streak off the HDR scene (PS t0 of the tonemap, stored /8) and the exposure (PS t1). The result is bound at kStreakSlot for the draw.
   static void BindRFGStreak(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data, ID3D11ShaderResourceView* srv_scene, ID3D11ShaderResourceView* srv_exposure)
   {
      auto& gd = GetGameDeviceData(device_data);
      ID3D11ShaderResourceView* streak_srv = nullptr;
      if (srv_scene && srv_exposure)
      {
         DrawStateStack<DrawStateStackType::FullGraphics> streak_state;
         streak_state.Cache(native_device_context, device_data.uav_max_count);

         RFGStreak_Draw(native_device, native_device_context, device_data, gd.streak_res,
            cb_luma_global_settings.GameSettings.StreakThreshold, cb_luma_global_settings.GameSettings.StreakStretch,
            srv_scene, srv_exposure, &streak_srv);

         streak_state.Restore(native_device_context);
      }
      native_device_context->PSSetShaderResources(kStreakSlot, 1, &streak_srv);
   }

public:
   // You can define any data to initialize once here
   void OnInit(bool async) override
   {
      // You can add shader defines that will end up in the advanced settings for users to modify here.
      // These will be defined in the shaders, so they can be used to do static branches in them.
      // Ideally they should also be defined in the game's Settings.hlsl file.
      std::vector<ShaderDefineData> game_shader_defines_data = {
         {"TONEMAP_TYPE", /*default value*/ '1', true, false, /*tooltip*/ "0 - Vanilla SDR\n1 - Luma HDR (Vanilla+)", /*max value*/ 1},
      };
      shader_defines_data.append_range(game_shader_defines_data);
      assert(shader_defines_data.size() < MAX_SHADER_DEFINES); // Make sure there's room for at least one extra custom define to add for development (this isn't really relevant outside of development)

      // Define these according to the game's original technical details and the mod's implementation (see their declarations for more).
      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('1'); // Vanilla swapchain is R8G8B8A8_UNORM_SRGB, so post processing is linear. What space are the colors in? Was the swapchain linear (sRGB texture format)? Did we change post processing to store in linear space?
      GetShaderDefineData(EARLY_DISPLAY_ENCODING_HASH).SetDefaultValue('0'); // Whether we do gamma correction and paper white scaling during post processing or we delay them until the final display composition pass
      GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0'); // What SDR transfer curve was the game using? Most modern games used sRGB in SDR
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('1'); // What SDR transfer curve to we want to emulate? This is relevant even if we work in linear space, as there can be a gamma mismatch on it
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('0'); // How does the UI draw in?

      // Customize the cbuffers indexes here. They should be between 0 and 13 ("D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT - 1"),
      // set them to invalid values (e.g. -1) to not set them (they won't be uploaded to the GPU, and thus not usable in shaders). All their values need to be different if they are valid.
      // These will be automatically sent to the GPU for every shader pass the mod overrides.
      luma_settings_cbuffer_index = 13;
      luma_data_cbuffer_index = 12; // Needed for debugging textures and having custom per pass data (including handling the final pass properly).
      luma_ui_cbuffer_index = -1; // Optional, see "UI_DRAW_TYPE" (this is for type 1)

      // Init the game settings here, you could also load them from ini in "LoadConfigs()".
      // These can be defined in a header shared between shaders and c++, and are sent to the GPU every time they change.
      //
      // It should be in "Shaders/Game/Includes/GameCBuffers.hlsl".
      //
      // To include it, the easiest way is adding as Forced Include within the Project Properties.
      // C/C++ -> All Options -> Forced Include File --> "../../../Shaders/$(ProjectName)/Includes/GameCBuffers.hlsl;" (w/o quotation marks)
      //
      // Otherwise, manually add it to the project files and include that at the top (before anything else, as the game cb settings struct needs to be defined),
      // or make a copy of it in c++ (making sure it's mirrored to hlsl), and define "LUMA_GAME_CB_STRUCTS" to avoid it being re-defined.
      //
      // Set "device_data.cb_luma_global_settings_dirty" to true if you changed values and need a re-upload to the GPU before OnPresent() (i.e. during OnDrawOrDispatch()).
      default_luma_global_game_settings.VanillaHueRestore = cb_luma_global_settings.GameSettings.VanillaHueRestore = 1.f;
      default_luma_global_game_settings.BloomStrength = cb_luma_global_settings.GameSettings.BloomStrength = 1.f;
      default_luma_global_game_settings.Exposure = cb_luma_global_settings.GameSettings.Exposure = 1.f;
      default_luma_global_game_settings.DisplayMapping = cb_luma_global_settings.GameSettings.DisplayMapping = 2u;
      default_luma_global_game_settings.LumaBloom = cb_luma_global_settings.GameSettings.LumaBloom = 0u;
      default_luma_global_game_settings.LumaBloomStrength = cb_luma_global_settings.GameSettings.LumaBloomStrength = 1.f;
      default_luma_global_game_settings.AnamorphicStreak = cb_luma_global_settings.GameSettings.AnamorphicStreak = 1u;
      default_luma_global_game_settings.StreakThreshold = cb_luma_global_settings.GameSettings.StreakThreshold = 1.f;
      default_luma_global_game_settings.StreakStretch = cb_luma_global_settings.GameSettings.StreakStretch = 0.75f;
      default_luma_global_game_settings.StreakIntensity = cb_luma_global_settings.GameSettings.StreakIntensity = 0.25f;
      default_luma_global_game_settings.Glow = cb_luma_global_settings.GameSettings.Glow = 1u;
      default_luma_global_game_settings.ParticleBrightness = cb_luma_global_settings.GameSettings.ParticleBrightness = 1.f;

      RFG_RegisterStreakShaders();
      native_shaders_definitions.emplace(CompileTimeStringHash("RFG Bloom Source"), ShaderDefinition{ "Luma_RFG_BloomSource", reshade::api::pipeline_subobject_type::pixel_shader });
   }

   // This needs to be overridden with your own "GameDeviceData" sub-class (destruction is automatically handled)
   void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
   {
      device_data.game = new TemplateGameDeviceData;
   }

   DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

      // Random pixel shader hash as an example
      static uint32_t pixel_shader_hash_tonemap = Shader::Hash_StrToNum("8B2321A2");

      // Here you can track and customize shader passes by hash, you can do whatever you want in it
      if (!game_device_data.has_drawn_tonemap && original_shader_hashes.Contains(pixel_shader_hash_tonemap, reshade::api::shader_stage::pixel))
      {
         game_device_data.has_drawn_tonemap = true;

         device_data.has_drawn_main_post_processing = true;
      }

      // Luma bloom on the tonemap pass: the HDR scene at t0 (stored /8) feeds the pyramid, bound at t4 for this draw only when on.
      // Uses the context the draw callback gives (the tonemap can run on a deferred context).
      if (original_shader_hashes.Contains(kTonemapHash, reshade::api::shader_stage::pixel)
         && cb_luma_global_settings.GameSettings.LumaBloom != 0 && GetShaderDefineCompiledNumericalValue(TONEMAP_TYPE_HASH) >= 1)
      {
         ComPtr<ID3D11ShaderResourceView> srv_scene, srv_exposure, srv_particles;
         native_device_context->PSGetShaderResources(0, 1, srv_scene.put());
         native_device_context->PSGetShaderResources(1, 1, srv_exposure.put());
         native_device_context->PSGetShaderResources(6, 1, srv_particles.put());
         BindLumaBloom(native_device, native_device_context, device_data, srv_scene.get(), srv_exposure.get(), srv_particles.get());
      }

      // Anamorphic streak on the tonemap pass (HDR path): scene at t0 (stored /8), exposure at t1, streak bound at t3 for this draw only when on.
      if (original_shader_hashes.Contains(kTonemapHash, reshade::api::shader_stage::pixel)
         && cb_luma_global_settings.GameSettings.AnamorphicStreak != 0 && GetShaderDefineCompiledNumericalValue(TONEMAP_TYPE_HASH) >= 1)
      {
         ComPtr<ID3D11ShaderResourceView> srv_scene, srv_exposure;
         native_device_context->PSGetShaderResources(0, 1, srv_scene.put());
         native_device_context->PSGetShaderResources(1, 1, srv_exposure.put());
         BindRFGStreak(native_device, native_device_context, device_data, srv_scene.get(), srv_exposure.get());
      }

      return DrawOrDispatchOverrideType::None; // Don't cancel the original draw call
   }
   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

      game_device_data.has_drawn_tonemap = false;
      device_data.has_drawn_main_post_processing = true;

      // LumaBloom off: give the address space back (core's DrawKarisAverage output, ~66 MB at 4K). Core only drops the UAV on swapchain init, so both views go here.
      if (cb_luma_global_settings.GameSettings.LumaBloom == 0u)
      {
         game_device_data.srv_luma_bloom.reset();
         game_device_data.bloom_source_res.Release();
         auto& mr = device_data.managed_resources;
         mr.unordered_access_views[CompileTimeStringHash("luma_karis_average")].reset();
         mr.shader_resource_views[CompileTimeStringHash("luma_karis_average")].reset();
      }

      // Streak off: give back the streak targets.
      if (cb_luma_global_settings.GameSettings.AnamorphicStreak == 0u)
         game_device_data.streak_res.Release();
   }

   void LoadConfigs() override
   {
      reshade::api::effect_runtime* runtime = nullptr;

      // ReShade.ini is ready here, so load its values.
      reshade::get_config_value(runtime, NAME, "VanillaHueRestore", cb_luma_global_settings.GameSettings.VanillaHueRestore);
      reshade::get_config_value(runtime, NAME, "BloomStrength", cb_luma_global_settings.GameSettings.BloomStrength);
      reshade::get_config_value(runtime, NAME, "Exposure", cb_luma_global_settings.GameSettings.Exposure);
      reshade::get_config_value(runtime, NAME, "DisplayMapping", cb_luma_global_settings.GameSettings.DisplayMapping);
      reshade::get_config_value(runtime, NAME, "LumaBloom", cb_luma_global_settings.GameSettings.LumaBloom);
      reshade::get_config_value(runtime, NAME, "LumaBloomStrength", cb_luma_global_settings.GameSettings.LumaBloomStrength);
      cb_luma_global_settings.GameSettings.LumaBloomStrength = std::clamp(cb_luma_global_settings.GameSettings.LumaBloomStrength, 0.f, 2.f);
      reshade::get_config_value(runtime, NAME, "AnamorphicStreak", cb_luma_global_settings.GameSettings.AnamorphicStreak);
      reshade::get_config_value(runtime, NAME, "StreakThreshold", cb_luma_global_settings.GameSettings.StreakThreshold);
      reshade::get_config_value(runtime, NAME, "StreakStretch", cb_luma_global_settings.GameSettings.StreakStretch);
      reshade::get_config_value(runtime, NAME, "StreakIntensity", cb_luma_global_settings.GameSettings.StreakIntensity);
      reshade::get_config_value(runtime, NAME, "Glow", cb_luma_global_settings.GameSettings.Glow);
      reshade::get_config_value(runtime, NAME, "ParticleBrightness", cb_luma_global_settings.GameSettings.ParticleBrightness);
      cb_luma_global_settings.GameSettings.ParticleBrightness = std::clamp(cb_luma_global_settings.GameSettings.ParticleBrightness, 0.f, 2.f);
      cb_luma_global_settings.GameSettings.StreakThreshold = std::clamp(cb_luma_global_settings.GameSettings.StreakThreshold, 0.f, 10.f);
      cb_luma_global_settings.GameSettings.StreakStretch = std::clamp(cb_luma_global_settings.GameSettings.StreakStretch, 0.f, 1.f);
      cb_luma_global_settings.GameSettings.StreakIntensity = std::clamp(cb_luma_global_settings.GameSettings.StreakIntensity, 0.f, 2.f);
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      reshade::api::effect_runtime* runtime = nullptr;
      
      // Draw whatever ImGui to let users modify settings.
      if (ImGui::SliderFloat("Vanilla Hue Restore", &cb_luma_global_settings.GameSettings.VanillaHueRestore, 0.f, 1.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "VanillaHueRestore", cb_luma_global_settings.GameSettings.VanillaHueRestore);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Pulls hue and chrominance of the HDR tonemap toward the vanilla SDR grade (1 = fully restored).");
      DrawResetButton(cb_luma_global_settings.GameSettings.VanillaHueRestore, default_luma_global_game_settings.VanillaHueRestore, "VanillaHueRestore", runtime);

      if (ImGui::SliderFloat("Bloom Strength", &cb_luma_global_settings.GameSettings.BloomStrength, 0.f, 2.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "BloomStrength", cb_luma_global_settings.GameSettings.BloomStrength);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Scales the bloom contribution in the HDR path.");
      DrawResetButton(cb_luma_global_settings.GameSettings.BloomStrength, default_luma_global_game_settings.BloomStrength, "BloomStrength", runtime);

      if (ImGui::SliderFloat("Exposure", &cb_luma_global_settings.GameSettings.Exposure, 0.25f, 4.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "Exposure", cb_luma_global_settings.GameSettings.Exposure);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Scales the scene contribution in the HDR path.");
      DrawResetButton(cb_luma_global_settings.GameSettings.Exposure, default_luma_global_game_settings.Exposure, "Exposure", runtime);

      int display_mapping = (int)cb_luma_global_settings.GameSettings.DisplayMapping;
      if (ImGui::Combo("Display Mapping", &display_mapping, "DICE (per channel)\0Reinhard Piecewise\0BT.2390 (adaptive)\0"))
      {
         cb_luma_global_settings.GameSettings.DisplayMapping = (uint32_t)display_mapping;
         reshade::set_config_value(runtime, NAME, "DisplayMapping", cb_luma_global_settings.GameSettings.DisplayMapping);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Tone mapping of the display peak. DICE = per channel. Reinhard Piecewise = by luminance. BT.2390 = adaptive roll-off to the display peak.");
      DrawResetButton(cb_luma_global_settings.GameSettings.DisplayMapping, default_luma_global_game_settings.DisplayMapping, "DisplayMapping", runtime);

      bool luma_bloom = cb_luma_global_settings.GameSettings.LumaBloom != 0;
      if (ImGui::Checkbox("Luma Bloom", &luma_bloom))
      {
         cb_luma_global_settings.GameSettings.LumaBloom = luma_bloom ? 1u : 0u;
         reshade::set_config_value(runtime, NAME, "LumaBloom", cb_luma_global_settings.GameSettings.LumaBloom);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Off = game bloom. On = Luma bloom pyramid (HDR path only).");
      DrawResetButton(cb_luma_global_settings.GameSettings.LumaBloom, default_luma_global_game_settings.LumaBloom, "LumaBloom", runtime);

      if (cb_luma_global_settings.GameSettings.LumaBloom != 0)
      {
         if (ImGui::SliderFloat("Luma Bloom Strength", &cb_luma_global_settings.GameSettings.LumaBloomStrength, 0.f, 2.f, "%.3f"))
            reshade::set_config_value(runtime, NAME, "LumaBloomStrength", cb_luma_global_settings.GameSettings.LumaBloomStrength);
         DrawResetButton(cb_luma_global_settings.GameSettings.LumaBloomStrength, default_luma_global_game_settings.LumaBloomStrength, "LumaBloomStrength", runtime);
      }

      bool anamorphic_streak = cb_luma_global_settings.GameSettings.AnamorphicStreak != 0;
      if (ImGui::Checkbox("Anamorphic Streak", &anamorphic_streak))
      {
         cb_luma_global_settings.GameSettings.AnamorphicStreak = anamorphic_streak ? 1u : 0u;
         reshade::set_config_value(runtime, NAME, "AnamorphicStreak", cb_luma_global_settings.GameSettings.AnamorphicStreak);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Horizontal lens streak (KinoStreak, MIT), HDR path only. Also hides the game's flare sprite.");
      DrawResetButton(cb_luma_global_settings.GameSettings.AnamorphicStreak, default_luma_global_game_settings.AnamorphicStreak, "AnamorphicStreak", runtime);

      bool glow = cb_luma_global_settings.GameSettings.Glow != 0;
      if (ImGui::Checkbox("Glow (Deprecated)", &glow))
      {
         cb_luma_global_settings.GameSettings.Glow = glow ? 1u : 0u;
         reshade::set_config_value(runtime, NAME, "Glow", cb_luma_global_settings.GameSettings.Glow);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Deprecated: toggles the whole offscreen particle layer (smoke, dust, explosions, sun glow, lens flares), not just glow. Leave on. (HDR only)");
      DrawResetButton(cb_luma_global_settings.GameSettings.Glow, default_luma_global_game_settings.Glow, "Glow", runtime);

      if (ImGui::SliderFloat("Particle Brightness", &cb_luma_global_settings.GameSettings.ParticleBrightness, 0.f, 2.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "ParticleBrightness", cb_luma_global_settings.GameSettings.ParticleBrightness);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Scales the particle layer (smoke, dust, glows, pickup effects) in HDR. 1 = vanilla.");
      DrawResetButton(cb_luma_global_settings.GameSettings.ParticleBrightness, default_luma_global_game_settings.ParticleBrightness, "ParticleBrightness", runtime);

      if (cb_luma_global_settings.GameSettings.AnamorphicStreak != 0)
      {
         if (ImGui::SliderFloat("Streak Threshold", &cb_luma_global_settings.GameSettings.StreakThreshold, 0.f, 10.f, "%.3f"))
            reshade::set_config_value(runtime, NAME, "StreakThreshold", cb_luma_global_settings.GameSettings.StreakThreshold);
         DrawResetButton(cb_luma_global_settings.GameSettings.StreakThreshold, default_luma_global_game_settings.StreakThreshold, "StreakThreshold", runtime);

         if (ImGui::SliderFloat("Streak Stretch", &cb_luma_global_settings.GameSettings.StreakStretch, 0.f, 1.f, "%.3f"))
            reshade::set_config_value(runtime, NAME, "StreakStretch", cb_luma_global_settings.GameSettings.StreakStretch);
         DrawResetButton(cb_luma_global_settings.GameSettings.StreakStretch, default_luma_global_game_settings.StreakStretch, "StreakStretch", runtime);

         if (ImGui::SliderFloat("Streak Intensity", &cb_luma_global_settings.GameSettings.StreakIntensity, 0.f, 2.f, "%.3f"))
            reshade::set_config_value(runtime, NAME, "StreakIntensity", cb_luma_global_settings.GameSettings.StreakIntensity);
         DrawResetButton(cb_luma_global_settings.GameSettings.StreakIntensity, default_luma_global_game_settings.StreakIntensity, "StreakIntensity", runtime);
      }
   }

   void PrintImGuiAbout() override
   {
      // Remember to credit Luma developers, the game mod creators, and all third party code that is used (plus, optionally, testers too)
      ImGui::Text("Template Luma mod - about and credits section", "");
   }
};

// This is where everything starts from, the very first call to the dll.
BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
   if (ul_reason_for_call == DLL_PROCESS_ATTACH)
   {
      // Setup the globals (e.g. name etc). It's good to do this stuff before registering the ReShade addon, to make sure the names are up to date.
      const char* project_name = PROJECT_NAME;
      const char* cleared_project_name = (project_name[0] == '_') ? (project_name + 1) : project_name; // Remove the potential "_" at the beginning. This can include spaces!

      uint32_t mod_version = 1; // Increase this to reset the game settings and shader binaries after making large changes to your mod
      Globals::SetGlobals(cleared_project_name, "Red Faction Guerrilla Remastered Luma mod", "" /*E.g. Nexus link*/, mod_version);

      // The following can be toggled in the dev settings (it generally only fully applies after changing the game's resolution).
      // Default these to "false" for the mod to not do "anything" that might cause issues with the game by default.
      swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
      swapchain_upgrade_type = SwapchainUpgradeType::scRGB;
      texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;
      // Texture upgrades (8 bit unorm and 11 bit float etc to 16 bit float)
      texture_upgrade_formats = {
            reshade::api::format::r8g8b8a8_unorm,
            reshade::api::format::r8g8b8a8_unorm_srgb,
            reshade::api::format::r8g8b8a8_typeless,
            reshade::api::format::r8g8b8x8_unorm,
            reshade::api::format::r8g8b8x8_unorm_srgb,
            reshade::api::format::b8g8r8a8_unorm,
            reshade::api::format::b8g8r8a8_unorm_srgb,
            reshade::api::format::b8g8r8a8_typeless,
            reshade::api::format::b8g8r8x8_unorm,
            reshade::api::format::b8g8r8x8_unorm_srgb,
            reshade::api::format::b8g8r8x8_typeless,

            reshade::api::format::r10g10b10a2_unorm,
            reshade::api::format::r10g10b10a2_typeless,

            //reshade::api::format::r16g16b16a16_unorm,

            reshade::api::format::r11g11b10_float,
      };
      // Set this to the size and type of the main color grading LUT you might want to upgrade (optional)
      texture_format_upgrades_lut_size = 32;
      texture_format_upgrades_lut_dimensions = LUTDimensions::_2D;
		// In case the textures failed to upgrade, tweak the filtering conditions to be more lenient (e.g. aspect ratio checks etc).
      texture_format_upgrades_2d_size_filters = 0 | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio;

      // Update samples to override the bias, based on the rendering resolution etc
      enable_samplers_upgrade = false;

#if DEVELOPMENT // If you want to track any shader names over time, you can hardcode them here by hash (they can be a useful reference in the pipeline)
      forced_shader_names.emplace(std::stoul("FD2925B4", nullptr, 16), "Tracked Shader Name");
#endif

#if !DEVELOPMENT // Put shaders that a previous version of the mod used but has ever since been deleted here, so that users updating the mod from an older version won't accidentally load them
      old_shader_file_names.emplace("Bloom_0xDC9373A8.ps_5_0.hlsl");
#endif

      // Create your game sub-class instance (it will be automatically destroyed on exit).
      // You do not need to do this if you have no custom data to store.
      game = new TemplateGame();
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}
