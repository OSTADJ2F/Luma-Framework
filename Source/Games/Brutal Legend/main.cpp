///////////////////////////////////////////////////////////////////////
// Of course, before any of this, create a new project for your game!
// - Copy "Templates\VisualStudio\Luma-Template.zip" into "%USERPROFILE%\Documents\Visual Studio [current version]\Templates\ProjectTemplates" (and restart Visual Studio).
// - Right click "Games" in the solution explorer, Add -> New Project... -> Template (Luma Template Project for new games)
// - Make sure the location is "Source\Games"!
///////////////////////////////////////////////////////////////////////

// Brütal Legend - Luma HDR mod (Double Fine Lua engine, 32-bit, DX9 -> D3D11 via dgVoodoo2).
// Hashes will be dgVoodoo-TRANSLATED and change per wrapper build: key the proven builds like MoHA/ME1/TW2/BL2
// (2.87.3 emits ps_5_0, 2.81.3 - the Proton build - emits ps_4_0), any other build needs a re-dump.

// No DEVELOPMENT auto-debugger MessageBox on DLL attach: invisible under borderless/fullscreen and it blocks the
// loader (ReShade times out the addon load -> error 1114). Same failure as BL2/TW2/MoHA under dgVoodoo.
#define DISABLE_AUTO_DEBUGGER 1

// Specify a define with the name of the game here (GAME_*).
// This is optional but can be used to hardcode custom (per game) behaviours in the core library,
// or other reasons. We can't automate it through the project define system (based on the project name) because
// we need these defines to be upper case and space free.
#define GAME_BRUTAL_LEGEND 1

//// Define all the global "core" defines before including its files: ////

// Enable support for Geometry Shaders (GS), rather rare.
#define GEOMETRY_SHADER_SUPPORT 0

// Enable Luma's SMAA (core registers its passes; used by "Replace FXAA with SMAA", see DrawSMAAForFXAA()).
#define ENABLE_SMAA 1

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

// XeGTAO replaces the game's AO generation/blur/upsample (see gtao.h and Shaders/Brutal Legend/Luma_BL_XeGTAO.hlsl).
#include "gtao.h"

#include "sdl_redirect.h"

#include <atomic>
#if DEVELOPMENT
#include <mutex>
#include <set>
#include <tuple>
#endif

// dgVoodoo2 2.87.5/2.87.6 bug fix: its GetTexture indexes the device's bound-texture table with the caller's raw
// Stage. Brütal Legend's multiplatform RHI layer probes stages 257-260 (0x100 + its internal slot ids); the MS
// runtime rejects those with D3DERR_INVALIDCALL, but dgVoodoo reads out of bounds and AVs in the type check
// ("mov eax,[ecx+0x18]") when level stream-in leaves a freed resource pointer in the adjacent device memory
// (d3d9.dll+0x9B0F8 on 2.87.6, +0x9AE13 on 2.87.5; same bytes, verified from WER dumps of both).
// The DLLs are AV-evasion-packed on disk (blank sections, no raw code at the faulting RVA), so this patches the
// live image instead: a detour on the 7-byte unchecked table load into a nearby cave that bounds-checks Stage.
// The out-parameter is zeroed by the original code AHEAD of the patch site, so the rejected path hands the engine
// a NULL texture, exactly like the vanilla runtime, and the game's own null-checks then skip the slot.
// Version-mismatch behavior: no unique signature hit = logged and left untouched (today's crash, no regression).
static bool TryPatchDgVoodooGetTextureBounds()
{
   static bool done = false;
   if (done)
      return true;

   HMODULE module = GetModuleHandleW(L"d3d9.dll"); // only the game folder's d3d9.dll exists on this path (dgVoodoo)
   if (module == nullptr)
      return false; // not loaded yet: stay retryable (dgVoodoo normally loads before ReShade inits, so this is just belt-and-braces)

   // mov ecx,[esi+eax*4 - disp] ; pop esi ; test ecx,ecx ; je rel8/rel32 ; mov eax,[ecx+0x18] ; sub eax,3
   uint8_t* found = nullptr;
   int matches = 0;
   for (uint8_t* addr = static_cast<uint8_t*>(static_cast<void*>(module));;)
   {
      MEMORY_BASIC_INFORMATION mbi = {};
      if (VirtualQuery(addr, &mbi, sizeof(mbi)) == 0)
         break;
      if (mbi.AllocationBase != static_cast<void*>(module))
         break; // walked out of the mapped image
      if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0)
      {
         uint8_t* p = static_cast<uint8_t*>(mbi.BaseAddress);
         const uint8_t* const end = p + mbi.RegionSize;
         while (end - p >= 24)
         {
            if (p[0] == 0x8B && p[1] == 0x8C && p[2] == 0x86 && p[7] == 0x5E && p[8] == 0x85 && p[9] == 0xC9 &&
                ((p[10] == 0x74 && p[12] == 0x8B && p[13] == 0x41 && p[14] == 0x18 && p[15] == 0x83 && p[16] == 0xE8 && p[17] == 0x03) ||
                 (p[10] == 0x0F && p[11] == 0x84 && p[18] == 0x8B && p[19] == 0x41 && p[20] == 0x18 && p[21] == 0x83 && p[22] == 0xE8 && p[23] == 0x03)))
            {
               ++matches;
               found = p;
            }
            ++p;
         }
      }
      addr = static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
   }

   // Torn-write note: the 7-byte patch is written non-atomically, but this runs inside dgVoodoo's own
   // Direct3DCreate9 -> D3D11 device creation -> ReShade init -> OnInit, i.e. BEFORE the game received the
   // D3D9 object and started calling it from its render thread, so no thread can be executing the site.
   if (matches != 1)
   {
      done = true; // terminal: wrong dgVoodoo build, retrying per-draw would only spam the log
      reshade::log::message(reshade::log::level::warning, std::format("[BL] dgVoodoo GetTexture fix: signature matched {} time(s) (expected 1) - dgVoodoo build not recognized, level loads may crash", matches).c_str());
      return false;
   }

   const int32_t disp = *reinterpret_cast<int32_t*>(found + 3);
   uint8_t* const back = found + 7; // the displaced instruction's successor ("pop esi")

   // A rel32 detour needs the cave within +/-2GB of the patch site, and with LAA the default VirtualAlloc
   // can land above 2GB - probe candidates at fixed offsets outward from the site instead.
   // VirtualAlloc rounds a non-null address DOWN to the 64 KB allocation granularity, so align the candidates
   // ourselves or the returned pointer will never equal the hint.
   static constexpr uintptr_t granularity = 0x10000;
   static constexpr uintptr_t granularity_mask = granularity - 1;
   uint8_t* cave = nullptr;
   for (int k = 1; k <= 512 && cave == nullptr; ++k)
   {
      static constexpr size_t step = 0x40000; // 256 KB per step, 128 MB reach either way
      for (int direction = 0; direction < 2 && cave == nullptr; ++direction)
      {
         uintptr_t candidate_addr = direction == 0
            ? (reinterpret_cast<uintptr_t>(found) + step * k) & ~granularity_mask
            : (reinterpret_cast<uintptr_t>(found) - step * k) & ~granularity_mask;
         if (candidate_addr < granularity || candidate_addr > 0xFFFF0000u)
            continue;
         void* candidate = reinterpret_cast<void*>(candidate_addr);
         if (VirtualAlloc(candidate, 0x40, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE) == candidate)
            cave = static_cast<uint8_t*>(candidate);
      }
   }
   if (cave == nullptr)
   {
      done = true;
      reshade::log::message(reshade::log::level::warning, "[BL] dgVoodoo GetTexture fix: no reachable cave address, workaround inactive");
      return false;
   }

   uint8_t* w = cave;
   *w++ = 0x83; *w++ = 0xF8; *w++ = 0x10; // cmp eax, 16
   uint8_t* const jb = w;
   *w++ = 0x72; *w++ = 0x00; // jb load
   *w++ = 0x31; *w++ = 0xC9; // xor ecx, ecx (OOB stage: treat as unbound; out-param already zeroed upstream)
   *w++ = 0xE9;
   *reinterpret_cast<int32_t*>(w) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(back) - reinterpret_cast<uintptr_t>(w + 4));
   w += 4;
   uint8_t* const load = w;
   jb[1] = static_cast<int8_t>(load - (jb + 2));
   *w++ = 0x8B; *w++ = 0x8C; *w++ = 0x86; // mov ecx,[esi+eax*4 - disp] (the displaced original)
   *reinterpret_cast<int32_t*>(w) = disp;
   w += 4;
   *w++ = 0xE9;
   *reinterpret_cast<int32_t*>(w) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(back) - reinterpret_cast<uintptr_t>(w + 4));
   w += 4;

   DWORD old_protection = 0;
   if (!VirtualProtect(found, 7, PAGE_EXECUTE_READWRITE, &old_protection))
   {
      done = true;
      reshade::log::message(reshade::log::level::warning, "[BL] dgVoodoo GetTexture fix: VirtualProtect on the patch site failed, workaround inactive");
      return false;
   }
   found[0] = 0xE9;
   *reinterpret_cast<int32_t*>(found + 1) = static_cast<int32_t>(reinterpret_cast<uintptr_t>(cave) - reinterpret_cast<uintptr_t>(found + 5));
   found[5] = 0x90;
   found[6] = 0x90;
   DWORD ignored = 0;
   VirtualProtect(found, 7, old_protection, &ignored);
   FlushInstructionCache(GetCurrentProcess(), found, 7);
   FlushInstructionCache(GetCurrentProcess(), cave, static_cast<SIZE_T>(w - cave));

   reshade::log::message(reshade::log::level::info, std::format("[BL] dgVoodoo GetTexture fix: bounds-check detour applied at d3d9.dll+0x{:X} (stage table disp 0x{:X}, cave at site{:+d})",
      reinterpret_cast<uintptr_t>(found) - reinterpret_cast<uintptr_t>(module), disp & 0xFFFFFFFFu, static_cast<int>(reinterpret_cast<uintptr_t>(cave) - reinterpret_cast<uintptr_t>(found))).c_str());
   done = true;
   return true;
}

struct BrutalLegendDeviceData final : public GameDeviceData
{
   bool has_drawn_tonemap = false;

   // XeGTAO
   BLXeGTAO::Resources gtao_resources;
   float gtao_tan_x = 0.f; // abs(tanHalfFovX), cached from the shadow resolve draw (used by the next frame's AO)
   float gtao_tan_y = 0.f; // abs(tanHalfFovY)
   bool gtao_tan_valid = false;
   bool gtao_ok_last_frame = false; // set by the AO upsample draw: XeGTAO produced this frame's AO
   ComPtr<ID3D11ShaderResourceView> sky_depth_srv; // scene depth captured from the half-res particles draw, bound at t8 for the tonemap
   ComPtr<ID3D11Buffer> cb_read_staging; // CPU readable copy of a PS constant buffer
   UINT cb_read_staging_size = 0;
   bool fog_cb_copied_this_frame = false; // the game's fog constant buffers were copied this frame (see OnDrawOrDispatch)
   ComPtr<ID3D11Buffer> fog_vs_cb; // GPU copy of the VS constant buffer in slot 4 (fog)
   ComPtr<ID3D11Buffer> fog_ps_cb; // GPU copy of the PS constant buffer in slot 4 (fog)
};

// Copies a whole constant buffer to a GPU-only buffer (no CPU readback). "dst" is (re)created when the size changes.
static void CopyConstantBufferForFog(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, ComPtr<ID3D11Buffer>& dst, ID3D11Buffer* src)
{
   D3D11_BUFFER_DESC src_desc = {};
   src->GetDesc(&src_desc);
   bool recreate = !dst;
   if (dst)
   {
      D3D11_BUFFER_DESC dst_desc = {};
      dst->GetDesc(&dst_desc);
      recreate = dst_desc.ByteWidth != src_desc.ByteWidth;
   }
   if (recreate)
   {
      D3D11_BUFFER_DESC desc = src_desc;
      desc.Usage = D3D11_USAGE_DEFAULT;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      desc.CPUAccessFlags = 0;
      desc.MiscFlags = 0;
      desc.StructureByteStride = 0;
      ComPtr<ID3D11Buffer> buffer;
      if (FAILED(native_device->CreateBuffer(&desc, nullptr, buffer.put())))
      {
         dst = nullptr;
         return;
      }
      dst = buffer;
   }
   native_device_context->CopyResource(dst.get(), src);
}

// Copies one float4 row of the PS constant buffer in "slot" to the CPU (via a staging copy). Returns false if unavailable.
// Stalls the GPU (Map), so keep the number of calls per frame low.
static bool ReadPSConstantBufferRow(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, BrutalLegendDeviceData& game_device_data, uint32_t slot, uint32_t row, float out[4])
{
   ComPtr<ID3D11Buffer> cb;
   native_device_context->PSGetConstantBuffers(slot, 1, cb.put());
   if (!cb)
      return false;
   D3D11_BUFFER_DESC desc = {};
   cb->GetDesc(&desc);
   if (desc.ByteWidth < (row + 1) * 16)
      return false;
   if (!game_device_data.cb_read_staging || game_device_data.cb_read_staging_size != desc.ByteWidth)
   {
      D3D11_BUFFER_DESC staging_desc = desc;
      staging_desc.Usage = D3D11_USAGE_STAGING;
      staging_desc.BindFlags = 0;
      staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      staging_desc.MiscFlags = 0;
      staging_desc.StructureByteStride = 0;
      ComPtr<ID3D11Buffer> staging;
      if (FAILED(native_device->CreateBuffer(&staging_desc, nullptr, staging.put())))
         return false;
      game_device_data.cb_read_staging = staging;
      game_device_data.cb_read_staging_size = desc.ByteWidth;
   }
   native_device_context->CopyResource(game_device_data.cb_read_staging.get(), cb.get());
   D3D11_MAPPED_SUBRESOURCE mapped = {};
   if (FAILED(native_device_context->Map(game_device_data.cb_read_staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
      return false;
   memcpy(out, static_cast<const uint8_t*>(mapped.pData) + row * 16, 16);
   native_device_context->Unmap(game_device_data.cb_read_staging.get(), 0);
   return true;
}

// Replaces the game's FXAA draw (pixel shader B78D5AEC, SRV2 = scene, RTV0 = output) with Luma's SMAA.
// The scene is gamma 2.2 encoded, sign mirrored and HDR: it is decoded to linear (HDR and negatives kept) for the blending,
// and a saturated gamma copy is used for edge detection. SMAA's output is encoded back to gamma 2.2 (see Luma_SMAA_impl.hlsl).
// Returns false, without changing any state, on any failure: the caller then runs the original FXAA draw.
static bool DrawSMAAForFXAA(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, DeviceData& device_data)
{
   ComPtr<ID3D11ShaderResourceView> srv_scene;
   native_device_context->PSGetShaderResources(2, 1, srv_scene.put());
   if (!srv_scene)
      return false;

   ComPtr<ID3D11RenderTargetView> rtv_output;
   native_device_context->OMGetRenderTargets(1, rtv_output.put(), nullptr);
   if (!rtv_output)
      return false;

   auto cs_it = device_data.native_compute_shaders.find(CompileTimeStringHash("BL SMAA Decode CS"));
   if (cs_it == device_data.native_compute_shaders.end() || !cs_it->second.get())
      return false;

   ComPtr<ID3D11Resource> scene_resource;
   srv_scene->GetResource(scene_resource.put());
   ComPtr<ID3D11Texture2D> scene_tex;
   if (!scene_resource || FAILED(scene_resource->QueryInterface(scene_tex.put())))
      return false;
   D3D11_TEXTURE2D_DESC tex_desc = {};
   scene_tex->GetDesc(&tex_desc);
   if (tex_desc.Width == 0 || tex_desc.Height == 0 || tex_desc.SampleDesc.Count != 1 || tex_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
      return false;

   ComPtr<ID3D11Resource> rtv_resource;
   rtv_output->GetResource(rtv_resource.put());
   ComPtr<ID3D11Texture2D> rtv_tex;
   if (!rtv_resource || FAILED(rtv_resource->QueryInterface(rtv_tex.put())))
      return false;
   D3D11_TEXTURE2D_DESC rtv_desc = {};
   rtv_tex->GetDesc(&rtv_desc);
   if (rtv_desc.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || rtv_desc.Width != tex_desc.Width || rtv_desc.Height != tex_desc.Height)
      return false;

   // Create all resources before binding anything, so early failures leave the state untouched.
   tex_desc.Usage = D3D11_USAGE_DEFAULT;
   tex_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
   tex_desc.CPUAccessFlags = 0;
   tex_desc.MiscFlags = 0;
   ComPtr<ID3D11Texture2D> tex_linear;
   ComPtr<ID3D11Texture2D> tex_gamma;
   if (FAILED(native_device->CreateTexture2D(&tex_desc, nullptr, tex_linear.put())) || FAILED(native_device->CreateTexture2D(&tex_desc, nullptr, tex_gamma.put())))
      return false;
   ComPtr<ID3D11UnorderedAccessView> uav_linear;
   ComPtr<ID3D11UnorderedAccessView> uav_gamma;
   ComPtr<ID3D11ShaderResourceView> srv_linear;
   ComPtr<ID3D11ShaderResourceView> srv_gamma;
   if (FAILED(native_device->CreateUnorderedAccessView(tex_linear.get(), nullptr, uav_linear.put()))
      || FAILED(native_device->CreateUnorderedAccessView(tex_gamma.get(), nullptr, uav_gamma.put()))
      || FAILED(native_device->CreateShaderResourceView(tex_linear.get(), nullptr, srv_linear.put()))
      || FAILED(native_device->CreateShaderResourceView(tex_gamma.get(), nullptr, srv_gamma.put())))
      return false;

   // SMAA metrics (1/size, size) for the metrics cbuffer at b1 (see Shaders/Brutal Legend/Luma_SMAA_impl.hlsl).
   const float smaa_metrics[4] = { 1.f / (float)tex_desc.Width, 1.f / (float)tex_desc.Height, (float)tex_desc.Width, (float)tex_desc.Height };
   D3D11_BUFFER_DESC cb_desc = {};
   cb_desc.ByteWidth = sizeof(smaa_metrics);
   cb_desc.Usage = D3D11_USAGE_IMMUTABLE;
   cb_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
   D3D11_SUBRESOURCE_DATA cb_data = {};
   cb_data.pSysMem = smaa_metrics;
   ComPtr<ID3D11Buffer> cb_smaa_metrics;
   if (FAILED(native_device->CreateBuffer(&cb_desc, &cb_data, cb_smaa_metrics.put())))
      return false;

   DrawStateStack<DrawStateStackType::FullGraphics> draw_state_stack;
   draw_state_stack.Cache(native_device_context, device_data.uav_max_count);

   // Decode pass: reads the scene, writes the linear copy (uav0) and the saturated gamma copy (uav1).
   ID3D11ShaderResourceView* srv_null = nullptr;
   native_device_context->PSSetShaderResources(2, 1, &srv_null); // The scene can't stay bound as an SRV while it's the CS input (and DrawSMAA rebinds PS SRVs anyway).
   ID3D11ShaderResourceView* cs_srv = srv_scene.get();
   native_device_context->CSSetShaderResources(0, 1, &cs_srv);
   const std::array<ID3D11UnorderedAccessView*, 2> cs_uavs = { uav_linear.get(), uav_gamma.get() };
   native_device_context->CSSetUnorderedAccessViews(0, cs_uavs.size(), cs_uavs.data(), nullptr);
   native_device_context->CSSetShader(cs_it->second.get(), nullptr, 0);
   native_device_context->Dispatch((tex_desc.Width + 8 - 1) / 8, (tex_desc.Height + 8 - 1) / 8, 1);

   // Unbind UAVs, SRVs and the shader.
   const std::array<ID3D11UnorderedAccessView*, 2> uav_nulls = {};
   native_device_context->CSSetUnorderedAccessViews(0, uav_nulls.size(), uav_nulls.data(), nullptr);
   native_device_context->CSSetShaderResources(0, 1, &srv_null);
   native_device_context->CSSetShader(nullptr, nullptr, 0);

   // SMAA: linear copy as the colour input, gamma copy for edge detection, output into the game's FXAA target.
   DrawSMAA(native_device, native_device_context, device_data, rtv_output.get(), srv_linear.get(), srv_gamma.get(), nullptr, cb_smaa_metrics.get());

   draw_state_stack.Restore(native_device_context);
   return true;
}

// Cursor hiding: the game window's WM_SETCURSOR is intercepted so the cursor is hidden over the client area.
HWND cursor_hook_window = nullptr;
WNDPROC cursor_prev_wndproc = nullptr;
std::atomic<bool> cursor_hide_enabled = true;

// True if the game exe has the LARGE_ADDRESS_AWARE flag set (4GB patched). Read from the in-memory PE header, cached once.
static bool IsGameLargeAddressAware()
{
   static const bool large_address_aware = []()
   {
      const auto* base = (const uint8_t*)GetModuleHandleW(nullptr);
      if (!base)
         return false;
      const auto* dos = (const IMAGE_DOS_HEADER*)base;
      if (dos->e_magic != IMAGE_DOS_SIGNATURE)
         return false;
      const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE)
         return false;
      return (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
   }();
   return large_address_aware;
}

static LRESULT CALLBACK CursorWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
   if (msg == WM_SETCURSOR && cursor_hide_enabled && LOWORD(lp) == HTCLIENT)
   {
      SetCursor(nullptr);
      return TRUE;
   }
   return CallWindowProcW(cursor_prev_wndproc, h, msg, wp, lp);
}

class BrutalLegend final : public Game // Go view "Source/Core/includes/game.h" for all the overridable virtual functions
{
   // Optional helper to hide ugly casts
   static BrutalLegendDeviceData& GetGameDeviceData(DeviceData& device_data)
   {
      return *static_cast<BrutalLegendDeviceData*>(device_data.game);
   }

public:
   // "Replace FXAA with SMAA" option (config key "SMAAEnable").
   bool replace_fxaa_with_smaa = true;
   // "Replace SSAO with XeGTAO" option (config key "XeGTAOEnable").
   bool replace_ssao_with_xegtao = true;
   // "Hide Cursor" option (config key "HideCursor"): the game runs borderless, so Windows no longer hides the cursor by itself.
   bool hide_cursor = true;
   // Private cursor state
   bool last_hide_cursor = true;

   // You can define any data to initialize once here
   int shadow_resolution_multiplier = 1; // Fixed at vanilla atlas resolution (no user option). 2 = 2x atlas (dev only, requires restart)
   bool shadow_upgrades_added = false;

   void OnInit(bool async) override
   {
      // One-shot: fix the dgVoodoo out-of-bounds GetTexture stage read (self-gated, see function comment).
      TryPatchDgVoodooGetTextureBounds();

      InstallSdlRedirect();

      if (IsGameLargeAddressAware())
         reshade::log::message(reshade::log::level::info, "[BL] 4GB patch: applied");
      else
         reshade::log::message(reshade::log::level::warning, "[BL] 4GB patch: NOT applied, out of memory crashes are likely");

      // Snap dgVoodoo's half-pixel shadow atlas viewports before Core's draw-time viewport checks see them.
      reshade::register_event<reshade::addon_event::bind_viewports>(BrutalLegend::OnBindViewports);
#if DEVELOPMENT
      reshade::register_event<reshade::addon_event::init_sampler>(BrutalLegend::OnInitSamplerLog);
#endif
      reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(BrutalLegend::OnBindRenderTargetsAndDepthStencil);

      // You can add shader defines that will end up in the advanced settings for users to modify here.
      // These will be defined in the shaders, so they can be used to do static branches in them.
      // Ideally they should also be defined in the game's Settings.hlsl file.
      std::vector<ShaderDefineData> game_shader_defines_data = {
         {"TONEMAP_TYPE", /*default value*/ '1', true, false, /*tooltip*/ "0 - Vanilla SDR\n1 - Luma HDR (Vanilla+)", /*max value*/ 1},
      };
       shader_defines_data.append_range(game_shader_defines_data);
       assert(shader_defines_data.size() < MAX_SHADER_DEFINES); // Make sure there's room for at least one extra custom define to add for development (this isn't really relevant outside of development)

       // Address-space ceiling, PROBED (as in ME1): the game exe is 32-bit DX9 and the shipped one has no
       // LARGE_ADDRESS_AWARE, so the process is capped at 2 GB. Luma's scratch + dgVoodoo's copies eat into that.
       // The bit cannot be flipped from inside the running process (read at creation; the loader locks the image
       // file without FILE_SHARE_WRITE), so it is a one-time external patch: warn instead, like Mass Effect does.
       static bool address_space_checked = false;
       if (!address_space_checked)
       {
          address_space_checked = true;
          SYSTEM_INFO sys_info = {};
          GetSystemInfo(&sys_info);
          if (reinterpret_cast<uintptr_t>(sys_info.lpMaximumApplicationAddress) < 0x80000000u)
             reshade::log::message(reshade::log::level::warning,
                "[Luma] Brütal Legend: this executable is NOT large-address-aware, so the process is capped at 2 GB of "
                "address space, on top of dgVoodoo's copies: expect crashes with high resolutions, the devkit, or HDR "
                "scratch. Patch the LARGE_ADDRESS_AWARE bit (one-time, external), or lower the resolution.");
       }


      // Define these according to the game's original technical details and the mod's implementation (see their declarations for more).
      GetShaderDefineData(POST_PROCESS_SPACE_TYPE_HASH).SetDefaultValue('0'); // What space are the colors in? Was the swapchain linear (sRGB texture format)? Did we change post processing to store in linear space?
      GetShaderDefineData(EARLY_DISPLAY_ENCODING_HASH).SetDefaultValue('0'); // Whether we do gamma correction and paper white scaling during post processing or we delay them until the final display composition pass
      GetShaderDefineData(VANILLA_ENCODING_TYPE_HASH).SetDefaultValue('0'); // What SDR transfer curve was the game using? Most modern games used sRGB in SDR
      GetShaderDefineData(GAMMA_CORRECTION_TYPE_HASH).SetDefaultValue('1'); // What SDR transfer curve to we want to emulate? This is relevant even if we work in linear space, as there can be a gamma mismatch on it
      GetShaderDefineData(UI_DRAW_TYPE_HASH).SetDefaultValue('0'); // How does the UI draw in?
      GetShaderDefineData(GAMUT_MAPPING_TYPE_HASH).SetDefaultValue('1'); // The tonemapper expands into BT.2020, late gamma correction and FXAA can push colors beyond it

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
      default_luma_global_game_settings.HighlightsStrength = cb_luma_global_settings.GameSettings.HighlightsStrength = 1.f;
      default_luma_global_game_settings.VanillaHueRestore = cb_luma_global_settings.GameSettings.VanillaHueRestore = 1.f;
      default_luma_global_game_settings.BloomIntensity = cb_luma_global_settings.GameSettings.BloomIntensity = 0.5f;
      default_luma_global_game_settings.GamutExpansion = cb_luma_global_settings.GameSettings.GamutExpansion = 0.5f;
      default_luma_global_game_settings.XeGTAOIntensity = cb_luma_global_settings.GameSettings.XeGTAOIntensity = 0.75f;
      default_luma_global_game_settings.XeGTAODarkness = cb_luma_global_settings.GameSettings.XeGTAODarkness = 0.5f;
      default_luma_global_game_settings.XeGTAORadius = cb_luma_global_settings.GameSettings.XeGTAORadius = 10.f; // fixed, no user setting
      default_luma_global_game_settings.ShadowSoftness = cb_luma_global_settings.GameSettings.ShadowSoftness = 1.5f;
      default_luma_global_game_settings.VideoAutoHDREnable = cb_luma_global_settings.GameSettings.VideoAutoHDREnable = 1.f;
      default_luma_global_game_settings.VideoAutoHDR = cb_luma_global_settings.GameSettings.VideoAutoHDR = 0.5f;
      cb_luma_global_settings.GameSettings.VideoDrawActive = 0.f; // Set per draw by the sprite draw, never saved
      cb_luma_global_settings.GameSettings.XeGTAOActive = 0.f; // Set per frame by the AO upsample draw, never saved
      default_luma_global_game_settings.SkyBrightness = cb_luma_global_settings.GameSettings.SkyBrightness = 0.6f;
      default_luma_global_game_settings.BlackFloor = cb_luma_global_settings.GameSettings.BlackFloor = 0.f;
      default_luma_global_game_settings.Tonemapper = cb_luma_global_settings.GameSettings.Tonemapper = 1.f;
      default_luma_global_game_settings.PsychoVShoulder = cb_luma_global_settings.GameSettings.PsychoVShoulder = 0.f;
      default_luma_global_game_settings.PsychoVGamutFit = cb_luma_global_settings.GameSettings.PsychoVGamutFit = 0.f;
      default_luma_global_game_settings.PsychoVHueLock = cb_luma_global_settings.GameSettings.PsychoVHueLock = 0.5f;
      cb_luma_global_settings.GameSettings.SkyDepthActive = 0.f; // Set per tonemap draw, never saved
      cb_luma_global_settings.GameSettings.FogDataActive = 0.f; // Set per tonemap draw, never saved
      default_luma_global_game_settings.FogStart = cb_luma_global_settings.GameSettings.FogStart = 0.f;
      default_luma_global_game_settings.FogDensity = cb_luma_global_settings.GameSettings.FogDensity = 1.f;
      default_luma_global_game_settings.FogColorSaturation = cb_luma_global_settings.GameSettings.FogColorSaturation = 1.f;
      default_luma_global_game_settings.FogColorBrightness = cb_luma_global_settings.GameSettings.FogColorBrightness = 0.76f;
      default_luma_global_game_settings.FogKeepColor = cb_luma_global_settings.GameSettings.FogKeepColor = 0.75f;

      native_shaders_definitions.emplace(CompileTimeStringHash("BL SMAA Decode CS"), ShaderDefinition{ "Luma_SMAA_Decode", reshade::api::pipeline_subobject_type::compute_shader });
      native_shaders_definitions.emplace(CompileTimeStringHash("BL XeGTAO Prefilter CS"), ShaderDefinition{ "Luma_BL_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "prefilter_depths16x16_cs" });
      native_shaders_definitions.emplace(CompileTimeStringHash("BL XeGTAO Main CS"), ShaderDefinition{ "Luma_BL_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "main_pass_cs" });
      native_shaders_definitions.emplace(CompileTimeStringHash("BL XeGTAO Denoise CS"), ShaderDefinition{ "Luma_BL_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "denoise_pass_cs" });
      native_shaders_definitions.emplace(CompileTimeStringHash("BL XeGTAO Denoise Final CS"), ShaderDefinition{ "Luma_BL_XeGTAO", reshade::api::pipeline_subobject_type::compute_shader, nullptr, "denoise_final_cs" });

   }

    // This needs to be overridden with your own "GameDeviceData" sub-class (destruction is automatically handled)
    void OnCreateDevice(ID3D11Device* native_device, DeviceData& device_data) override
    {
       // Defensive retry of the dgVoodoo bounds patch (self-gated; no-op once applied or given up).
       TryPatchDgVoodooGetTextureBounds();

       device_data.game = new BrutalLegendDeviceData;
    }

   DrawOrDispatchOverrideType OnDrawOrDispatch(ID3D11Device* native_device, ID3D11DeviceContext* native_device_context, CommandListData& cmd_list_data, DeviceData& device_data, reshade::api::shader_stage stages, const ShaderHashesList<OneShaderPerPipeline>& original_shader_hashes, bool is_custom_pass, bool& updated_cbuffers, std::function<void()>* original_draw_dispatch_func) override
   {
      auto& game_device_data = GetGameDeviceData(device_data);

      // Fog: copy the game's fog constant buffers (VS and PS slot 4) once per frame, on a draw of the fog vertex shader. Bound to the tonemap draw below.
      static const uint32_t vertex_shader_hash_fog = Shader::Hash_StrToNum("29550E3E");
      if (!game_device_data.fog_cb_copied_this_frame && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE && original_shader_hashes.Contains(vertex_shader_hash_fog, reshade::api::shader_stage::vertex))
      {
         ComPtr<ID3D11Buffer> fog_vs_src;
         ComPtr<ID3D11Buffer> fog_ps_src;
         native_device_context->VSGetConstantBuffers(4, 1, fog_vs_src.put());
         native_device_context->PSGetConstantBuffers(4, 1, fog_ps_src.put());
         if (fog_vs_src && fog_ps_src)
         {
            CopyConstantBufferForFog(native_device, native_device_context, game_device_data.fog_vs_cb, fog_vs_src.get());
            CopyConstantBufferForFog(native_device, native_device_context, game_device_data.fog_ps_cb, fog_ps_src.get());
            game_device_data.fog_cb_copied_this_frame = true;
         }
      }

      // The game's FXAA draw becomes Luma's SMAA (only drawn while FXAA is on in the game's options). On failure the original draw runs.
      static const uint32_t pixel_shader_hash_fxaa = Shader::Hash_StrToNum("B78D5AEC");
      if (replace_fxaa_with_smaa && native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE && original_shader_hashes.Contains(pixel_shader_hash_fxaa, reshade::api::shader_stage::pixel))
      {
         if (DrawSMAAForFXAA(native_device, native_device_context, device_data))
            return DrawOrDispatchOverrideType::Replaced;
      }

      // Video AutoHDR: the generic sprite PS draws the FMV frames (a large RENDER_TARGET texture) and UI sprites. Flag the video draws so the shader only expands those.
      static const uint32_t pixel_shader_hash_sprite = Shader::Hash_StrToNum("D5C6F8E3");
      if (original_shader_hashes.Contains(pixel_shader_hash_sprite, reshade::api::shader_stage::pixel))
      {
         bool video_draw = false;
         ComPtr<ID3D11ShaderResourceView> srv_video;
         native_device_context->PSGetShaderResources(0, 1, srv_video.put());
         if (srv_video)
         {
            ComPtr<ID3D11Resource> video_resource;
            ComPtr<ID3D11Texture2D> video_tex;
            srv_video->GetResource(video_resource.put());
            if (video_resource && SUCCEEDED(video_resource->QueryInterface(video_tex.put())))
            {
               D3D11_TEXTURE2D_DESC video_desc = {};
               video_tex->GetDesc(&video_desc);
               video_draw = (video_desc.BindFlags & D3D11_BIND_RENDER_TARGET) && video_desc.Width >= 640 && video_desc.Height >= 360;
            }
         }
         const float video_draw_active = video_draw ? 1.f : 0.f;
         if (cb_luma_global_settings.GameSettings.VideoDrawActive != video_draw_active)
         {
            cb_luma_global_settings.GameSettings.VideoDrawActive = video_draw_active;
            device_data.cb_luma_global_settings_dirty = true;
            SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
            updated_cbuffers = true;
         }
      }

      // XeGTAO: replaces the game's AO generation (907DFA1C), blur (ABF4B0D7) and upsample (51BA9E06). The shadow resolve (203D9FAF) only caches the projection.
      static const uint32_t pixel_shader_hash_shadow_resolve = Shader::Hash_StrToNum("203D9FAF");
      static const uint32_t pixel_shader_hash_ao_upsample = Shader::Hash_StrToNum("51BA9E06");
      static const uint32_t pixel_shader_hash_ao_generation = Shader::Hash_StrToNum("907DFA1C");
      static const uint32_t pixel_shader_hash_ao_blur = Shader::Hash_StrToNum("ABF4B0D7");
      if (native_device_context->GetType() == D3D11_DEVICE_CONTEXT_IMMEDIATE)
      {
         // Sky depth: the half-res particles draw (runs before the tonemap) reads the scene depth at t2. Capture it for the tonemap's t8 binding.
         static const uint32_t pixel_shader_hash_particles_half = Shader::Hash_StrToNum("772DEB94");
         if (original_shader_hashes.Contains(pixel_shader_hash_particles_half, reshade::api::shader_stage::pixel))
         {
            game_device_data.sky_depth_srv = nullptr;
            ComPtr<ID3D11ShaderResourceView> srv_sky_depth;
            native_device_context->PSGetShaderResources(2, 1, srv_sky_depth.put());
            if (srv_sky_depth)
            {
               ComPtr<ID3D11Resource> sky_depth_resource;
               ComPtr<ID3D11Texture2D> sky_depth_tex;
               srv_sky_depth->GetResource(sky_depth_resource.put());
               if (sky_depth_resource && SUCCEEDED(sky_depth_resource->QueryInterface(sky_depth_tex.put())))
               {
                  D3D11_TEXTURE2D_DESC sky_depth_desc = {};
                  sky_depth_tex->GetDesc(&sky_depth_desc);
                  if (sky_depth_desc.Format == DXGI_FORMAT_R32_FLOAT && sky_depth_desc.SampleDesc.Count == 1)
                     game_device_data.sky_depth_srv = srv_sky_depth;
               }
            }
         }

         if (original_shader_hashes.Contains(pixel_shader_hash_shadow_resolve, reshade::api::shader_stage::pixel))
         {
            float row17[4] = {};
            if (ReadPSConstantBufferRow(native_device, native_device_context, game_device_data, 4, 17, row17))
            {
               game_device_data.gtao_tan_x = std::abs(row17[0]);
               game_device_data.gtao_tan_y = std::abs(row17[1]);
               game_device_data.gtao_tan_valid = true;
            }
         }
         else if (original_shader_hashes.Contains(pixel_shader_hash_ao_upsample, reshade::api::shader_stage::pixel))
         {
            bool gtao_drawn = false;
            ID3D11ShaderResourceView* gtao_ao_srv = nullptr;
            if (replace_ssao_with_xegtao && original_draw_dispatch_func)
            {
               ComPtr<ID3D11ShaderResourceView> srv_depth;
               native_device_context->PSGetShaderResources(2, 1, srv_depth.put());
               ComPtr<ID3D11Resource> depth_resource;
               ComPtr<ID3D11Texture2D> depth_tex;
               D3D11_TEXTURE2D_DESC depth_desc = {};
               if (srv_depth)
                  srv_depth->GetResource(depth_resource.put());
               if (depth_resource && SUCCEEDED(depth_resource->QueryInterface(depth_tex.put())))
                  depth_tex->GetDesc(&depth_desc);
               float row16[4] = {};
               if (srv_depth && depth_desc.Format == DXGI_FORMAT_R32_FLOAT && depth_desc.SampleDesc.Count == 1
                  && ReadPSConstantBufferRow(native_device, native_device_context, game_device_data, 4, 16, row16))
               {
                  BLXeGTAO::Inputs gtao_inputs;
                  gtao_inputs.depth_srv = srv_depth.get();
                  gtao_inputs.depth_lin_z = row16[2];
                  gtao_inputs.depth_lin_w = row16[3];
                  const float fallback_tan_y = 0.630023f;
                  gtao_inputs.tan_half_fov_y = game_device_data.gtao_tan_valid ? game_device_data.gtao_tan_y : fallback_tan_y;
                  gtao_inputs.tan_half_fov_x = game_device_data.gtao_tan_valid ? game_device_data.gtao_tan_x : fallback_tan_y * float(depth_desc.Width) / float(depth_desc.Height);
                  gtao_inputs.radius = cb_luma_global_settings.GameSettings.XeGTAORadius;
                  gtao_inputs.intensity = cb_luma_global_settings.GameSettings.XeGTAOIntensity;
                  gtao_inputs.darkness = cb_luma_global_settings.GameSettings.XeGTAODarkness;
                  gtao_inputs.width = depth_desc.Width;
                  gtao_inputs.height = depth_desc.Height;
                  if (BLXeGTAO::Draw(native_device, native_device_context, device_data, game_device_data.gtao_resources, gtao_inputs, &gtao_ao_srv) && gtao_ao_srv)
                     gtao_drawn = true;
               }
            }
            game_device_data.gtao_ok_last_frame = gtao_drawn;

            // Re-upload the settings cbuffer with the new XeGTAOActive value for this draw.
            cb_luma_global_settings.GameSettings.XeGTAOActive = gtao_drawn ? 1.f : 0.f;
            device_data.cb_luma_global_settings_dirty = true;
            SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
            updated_cbuffers = true;

            if (!gtao_drawn)
               return DrawOrDispatchOverrideType::None; // The game's AO upsample runs untouched

            // Bind the XeGTAO visibility at t5 for the game's upsample draw, then restore slot 5.
            ComPtr<ID3D11ShaderResourceView> saved_t5;
            native_device_context->PSGetShaderResources(5, 1, saved_t5.put());
            ID3D11ShaderResourceView* gtao_srv_ptr = gtao_ao_srv;
            native_device_context->PSSetShaderResources(5, 1, &gtao_srv_ptr);
            (*original_draw_dispatch_func)();
            ID3D11ShaderResourceView* t5_restore = saved_t5.get();
            native_device_context->PSSetShaderResources(5, 1, &t5_restore);
            return DrawOrDispatchOverrideType::Skip;
         }
         else if ((original_shader_hashes.Contains(pixel_shader_hash_ao_generation, reshade::api::shader_stage::pixel) || original_shader_hashes.Contains(pixel_shader_hash_ao_blur, reshade::api::shader_stage::pixel))
            && replace_ssao_with_xegtao && game_device_data.gtao_ok_last_frame)
         {
            return DrawOrDispatchOverrideType::Skip; // XeGTAO replaces the game's AO generation and blur
         }
      }

      // Random pixel shader hash as an example
      static uint32_t pixel_shader_hash_tonemap = Shader::Hash_StrToNum("A2C20279");

      // Here you can track and customize shader passes by hash, you can do whatever you want in it
      if (original_shader_hashes.Contains(pixel_shader_hash_tonemap, reshade::api::shader_stage::pixel))
      {
         if (!game_device_data.has_drawn_tonemap)
         {
            game_device_data.has_drawn_tonemap = true;

            device_data.has_drawn_main_post_processing = true;
         }

         // Sky depth (t8) and fog (PS b6 = fog VS cb, PS b7 = fog PS cb): bound for the tonemap draw only, in one draw call.
         const bool bind_sky_depth = original_draw_dispatch_func && game_device_data.sky_depth_srv;
         const bool bind_fog = original_draw_dispatch_func && game_device_data.fog_cb_copied_this_frame && game_device_data.fog_vs_cb && game_device_data.fog_ps_cb;
         if (bind_sky_depth || bind_fog)
         {
            cb_luma_global_settings.GameSettings.SkyDepthActive = bind_sky_depth ? 1.f : 0.f;
            cb_luma_global_settings.GameSettings.FogDataActive = bind_fog ? 1.f : 0.f;
            device_data.cb_luma_global_settings_dirty = true;
            SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
            updated_cbuffers = true;

            ComPtr<ID3D11ShaderResourceView> saved_t8;
            if (bind_sky_depth)
            {
               native_device_context->PSGetShaderResources(8, 1, saved_t8.put());
               ID3D11ShaderResourceView* sky_depth_srv_ptr = game_device_data.sky_depth_srv.get();
               native_device_context->PSSetShaderResources(8, 1, &sky_depth_srv_ptr);
            }
            ComPtr<ID3D11Buffer> saved_b6;
            ComPtr<ID3D11Buffer> saved_b7;
            if (bind_fog)
            {
               native_device_context->PSGetConstantBuffers(6, 1, saved_b6.put());
               native_device_context->PSGetConstantBuffers(7, 1, saved_b7.put());
               ID3D11Buffer* fog_vs_cb_ptr = game_device_data.fog_vs_cb.get();
               ID3D11Buffer* fog_ps_cb_ptr = game_device_data.fog_ps_cb.get();
               native_device_context->PSSetConstantBuffers(6, 1, &fog_vs_cb_ptr);
               native_device_context->PSSetConstantBuffers(7, 1, &fog_ps_cb_ptr);
            }

            (*original_draw_dispatch_func)();

            if (bind_sky_depth)
            {
               ID3D11ShaderResourceView* t8_restore = saved_t8.get();
               native_device_context->PSSetShaderResources(8, 1, &t8_restore);
            }
            if (bind_fog)
            {
               ID3D11Buffer* b6_restore = saved_b6.get();
               ID3D11Buffer* b7_restore = saved_b7.get();
               native_device_context->PSSetConstantBuffers(6, 1, &b6_restore);
               native_device_context->PSSetConstantBuffers(7, 1, &b7_restore);
            }

            cb_luma_global_settings.GameSettings.SkyDepthActive = 0.f;
            cb_luma_global_settings.GameSettings.FogDataActive = 0.f;
            device_data.cb_luma_global_settings_dirty = true;
            SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
            updated_cbuffers = true;
            return DrawOrDispatchOverrideType::Skip;
         }
         if (cb_luma_global_settings.GameSettings.SkyDepthActive != 0.f || cb_luma_global_settings.GameSettings.FogDataActive != 0.f)
         {
            cb_luma_global_settings.GameSettings.SkyDepthActive = 0.f;
            cb_luma_global_settings.GameSettings.FogDataActive = 0.f;
            device_data.cb_luma_global_settings_dirty = true;
            SetLumaConstantBuffers(native_device_context, cmd_list_data, device_data, stages, LumaConstantBufferType::LumaSettings);
            updated_cbuffers = true;
         }
      }

      return DrawOrDispatchOverrideType::None; // Don't cancel the original draw call
   }
   void OnDestroyDeviceData(DeviceData& device_data) override
   {
      GetGameDeviceData(device_data).gtao_resources.Release();
      GetGameDeviceData(device_data).sky_depth_srv = nullptr;
      Game::OnDestroyDeviceData(device_data); // Deletes device_data.game
   }

   void UpdateCursorState()
   {
      cursor_hide_enabled = hide_cursor;

      HWND w = game_window;
      if (w && w != cursor_hook_window)
      {
         if (cursor_hook_window && GetWindowLongPtrW(cursor_hook_window, GWLP_WNDPROC) == (LONG_PTR)&CursorWndProc)
            SetWindowLongPtrW(cursor_hook_window, GWLP_WNDPROC, (LONG_PTR)cursor_prev_wndproc);
         cursor_prev_wndproc = (WNDPROC)SetWindowLongPtrW(w, GWLP_WNDPROC, (LONG_PTR)&CursorWndProc);
         cursor_hook_window = w;
      }

      // Refresh the cursor immediately when the option was toggled, without moving the mouse
      if (hide_cursor != last_hide_cursor)
      {
         last_hide_cursor = hide_cursor;
         if (cursor_hook_window)
            PostMessageW(cursor_hook_window, WM_SETCURSOR, (WPARAM)cursor_hook_window, MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
      }
   }

   void OnPresent(ID3D11Device* native_device, DeviceData& device_data) override
   {
      UpdateCursorState();

      auto& game_device_data = GetGameDeviceData(device_data);

      game_device_data.has_drawn_tonemap = false;
      game_device_data.fog_cb_copied_this_frame = false;
      game_device_data.sky_depth_srv = nullptr;
      device_data.has_drawn_main_post_processing = true;
   }

   void LoadConfigs() override
   {
      reshade::api::effect_runtime* runtime = nullptr;

      // ReShade.ini is ready here, so load its values.
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "HighlightsStrength", cb_luma_global_settings.GameSettings.HighlightsStrength);
      reshade::get_config_value(runtime, NAME, "VanillaHueRestore", cb_luma_global_settings.GameSettings.VanillaHueRestore);
      reshade::get_config_value(runtime, NAME, "SkyBrightness", cb_luma_global_settings.GameSettings.SkyBrightness);
      reshade::get_config_value(runtime, NAME, "BlackFloor", cb_luma_global_settings.GameSettings.BlackFloor);
#endif
#if DEVELOPMENT // Release always uses PsychoV35 (the default)
      reshade::get_config_value(runtime, NAME, "Tonemapper", cb_luma_global_settings.GameSettings.Tonemapper);
#endif
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "PsychoVShoulder", cb_luma_global_settings.GameSettings.PsychoVShoulder);
#endif
      reshade::get_config_value(runtime, NAME, "PsychoVGamutFit", cb_luma_global_settings.GameSettings.PsychoVGamutFit);
      reshade::get_config_value(runtime, NAME, "PsychoVHueLock", cb_luma_global_settings.GameSettings.PsychoVHueLock);
      reshade::get_config_value(runtime, NAME, "BloomIntensity", cb_luma_global_settings.GameSettings.BloomIntensity);
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "GamutExpansion", cb_luma_global_settings.GameSettings.GamutExpansion);
#endif
      reshade::get_config_value(runtime, NAME, "SMAAEnable", replace_fxaa_with_smaa);
      reshade::get_config_value(runtime, NAME, "HideCursor", hide_cursor);
      reshade::get_config_value(runtime, NAME, "XeGTAOEnable", replace_ssao_with_xegtao);
#if DEVELOPMENT
      reshade::get_config_value(runtime, NAME, "XeGTAOIntensity", cb_luma_global_settings.GameSettings.XeGTAOIntensity);
      reshade::get_config_value(runtime, NAME, "XeGTAODarkness", cb_luma_global_settings.GameSettings.XeGTAODarkness);
#endif
      reshade::get_config_value(runtime, NAME, "ShadowSoftness", cb_luma_global_settings.GameSettings.ShadowSoftness);
      reshade::get_config_value(runtime, NAME, "VideoAutoHDREnable", cb_luma_global_settings.GameSettings.VideoAutoHDREnable);
      reshade::get_config_value(runtime, NAME, "VideoAutoHDR", cb_luma_global_settings.GameSettings.VideoAutoHDR);
      reshade::get_config_value(runtime, NAME, "FogStart", cb_luma_global_settings.GameSettings.FogStart);
      reshade::get_config_value(runtime, NAME, "FogDensity", cb_luma_global_settings.GameSettings.FogDensity);
      reshade::get_config_value(runtime, NAME, "FogColorSaturation", cb_luma_global_settings.GameSettings.FogColorSaturation);
      reshade::get_config_value(runtime, NAME, "FogColorBrightness", cb_luma_global_settings.GameSettings.FogColorBrightness);
      reshade::get_config_value(runtime, NAME, "FogKeepColor", cb_luma_global_settings.GameSettings.FogKeepColor);

      // Read before filling the texture upgrade lists (they are consumed when the game creates its textures)
      if (shadow_resolution_multiplier != 1 && !shadow_upgrades_added)
      {
         shadow_upgrades_added = true;
         const uint32_t m = (uint32_t)shadow_resolution_multiplier;
         // Shadow atlas (1024x4096, 4 cascades stacked vertically): caster colour target and depth buffer
         texture_custom_dimensions_upgrades.insert(texture_custom_dimensions_upgrades.end(), {
            {uint4(1024, 4096, 1, 1), reshade::api::format::r32_float, uint4(1024 * m, 4096 * m, 0, 0)},
            {uint4(1024, 4096, 1, 1), reshade::api::format::r24_g8_typeless, uint4(1024 * m, 4096 * m, 0, 0)},
         });
      }
   }

   void DrawImGuiSettings(DeviceData& device_data) override
   {
      reshade::api::effect_runtime* runtime = nullptr;

      // 4GB (Large Address Aware) patch status.
      if (IsGameLargeAddressAware())
      {
         ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.f), "4GB patch applied");
      }
      else
      {
         ImGui::TextColored(ImVec4(1.f, 0.75f, 0.2f, 1.f), "Warning: the game is not 4GB patched");
         ImGui::TextWrapped("BrutalLegend.exe is not Large Address Aware. Apply a 4GB patch to it yourself (e.g. with a 4GB patch tool), otherwise out of memory crashes are likely, especially with HDR and the mod's features enabled.");
      }
      ImGui::Separator();

      // Draw whatever ImGui to let users modify settings.
#if DEVELOPMENT
      if (ImGui::SliderFloat("Highlights Strength", &cb_luma_global_settings.GameSettings.HighlightsStrength, 0.f, 2.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "HighlightsStrength", cb_luma_global_settings.GameSettings.HighlightsStrength);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Intensity of the overbright range the game clips in SDR. 0 is vanilla.");
      DrawResetButton(cb_luma_global_settings.GameSettings.HighlightsStrength, default_luma_global_game_settings.HighlightsStrength, "HighlightsStrength", runtime);

      if (ImGui::SliderFloat("Sky Brightness", &cb_luma_global_settings.GameSettings.SkyBrightness, 0.f, 3.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "SkyBrightness", cb_luma_global_settings.GameSettings.SkyBrightness);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How far the sky can go above SDR white, in paper white units. 0 = vanilla clip.");
      DrawResetButton(cb_luma_global_settings.GameSettings.SkyBrightness, default_luma_global_game_settings.SkyBrightness, "SkyBrightness", runtime);

      if (ImGui::SliderFloat("Black Floor", &cb_luma_global_settings.GameSettings.BlackFloor, 0.f, 1.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "BlackFloor", cb_luma_global_settings.GameSettings.BlackFloor);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Lowers the black level the game's distance fog lifts. 0 is vanilla.");
      DrawResetButton(cb_luma_global_settings.GameSettings.BlackFloor, default_luma_global_game_settings.BlackFloor, "BlackFloor", runtime);
#endif

#if DEVELOPMENT
      {
         static const char* tonemapper_items[] = { "DICE", "PsychoV35" };
         int tonemapper_index = cb_luma_global_settings.GameSettings.Tonemapper >= 0.5f ? 1 : 0;
         if (ImGui::Combo("Tonemapper", &tonemapper_index, tonemapper_items, IM_ARRAYSIZE(tonemapper_items)))
         {
            cb_luma_global_settings.GameSettings.Tonemapper = (float)tonemapper_index;
            reshade::set_config_value(runtime, NAME, "Tonemapper", cb_luma_global_settings.GameSettings.Tonemapper);
         }
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("HDR display mapping. PsychoV35: RenoDX cone-response tonemapper.");
         DrawResetButton(cb_luma_global_settings.GameSettings.Tonemapper, default_luma_global_game_settings.Tonemapper, "Tonemapper", runtime);
      }
#endif

      if (cb_luma_global_settings.GameSettings.Tonemapper >= 0.5f)
      {
#if DEVELOPMENT
         if (ImGui::SliderFloat("PsychoV Shoulder", &cb_luma_global_settings.GameSettings.PsychoVShoulder, 0.f, 3.f, "%.2f"))
            reshade::set_config_value(runtime, NAME, "PsychoVShoulder", cb_luma_global_settings.GameSettings.PsychoVShoulder);
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("0 = auto (recommended). Higher keeps mid-tones brighter but compresses highlights harder.");
         DrawResetButton(cb_luma_global_settings.GameSettings.PsychoVShoulder, default_luma_global_game_settings.PsychoVShoulder, "PsychoVShoulder", runtime);
#endif

         {
            static const char* gamut_fit_items[] = { "Exact", "Smooth" };
            int gamut_fit_index = cb_luma_global_settings.GameSettings.PsychoVGamutFit >= 0.5f ? 1 : 0;
            if (ImGui::Combo("PsychoV Gamut Fit", &gamut_fit_index, gamut_fit_items, IM_ARRAYSIZE(gamut_fit_items)))
            {
               cb_luma_global_settings.GameSettings.PsychoVGamutFit = (float)gamut_fit_index;
               reshade::set_config_value(runtime, NAME, "PsychoVGamutFit", cb_luma_global_settings.GameSettings.PsychoVGamutFit);
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
               ImGui::SetTooltip("How saturated colors are fitted into the display gamut near peak.");
            DrawResetButton(cb_luma_global_settings.GameSettings.PsychoVGamutFit, default_luma_global_game_settings.PsychoVGamutFit, "PsychoVGamutFit", runtime);
         }

         if (ImGui::SliderFloat("PsychoV Hue Lock", &cb_luma_global_settings.GameSettings.PsychoVHueLock, 0.f, 1.f, "%.2f"))
            reshade::set_config_value(runtime, NAME, "PsychoVHueLock", cb_luma_global_settings.GameSettings.PsychoVHueLock);
         if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Restores the hue PsychoV shifts in bright saturated colors (MacLeod-Boynton, hue only).");
         DrawResetButton(cb_luma_global_settings.GameSettings.PsychoVHueLock, default_luma_global_game_settings.PsychoVHueLock, "PsychoVHueLock", runtime);
      }

      ImGui::Separator();
      ImGui::Text("Fog");
      if (ImGui::SliderFloat("Fog Start", &cb_luma_global_settings.GameSettings.FogStart, 0.f, 300.f, "%.0f"))
         reshade::set_config_value(runtime, NAME, "FogStart", cb_luma_global_settings.GameSettings.FogStart);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Pushes the game's fog away from the camera (world units). 0 = vanilla.");
      DrawResetButton(cb_luma_global_settings.GameSettings.FogStart, default_luma_global_game_settings.FogStart, "FogStart", runtime);

      if (ImGui::SliderFloat("Fog Density", &cb_luma_global_settings.GameSettings.FogDensity, 0.f, 1.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "FogDensity", cb_luma_global_settings.GameSettings.FogDensity);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Multiplier on the fog density. Fog is only ever reduced.");
      DrawResetButton(cb_luma_global_settings.GameSettings.FogDensity, default_luma_global_game_settings.FogDensity, "FogDensity", runtime);

      if (ImGui::SliderFloat("Fog Colour Saturation", &cb_luma_global_settings.GameSettings.FogColorSaturation, 0.f, 2.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "FogColorSaturation", cb_luma_global_settings.GameSettings.FogColorSaturation);
      DrawResetButton(cb_luma_global_settings.GameSettings.FogColorSaturation, default_luma_global_game_settings.FogColorSaturation, "FogColorSaturation", runtime);

      if (ImGui::SliderFloat("Fog Colour Brightness", &cb_luma_global_settings.GameSettings.FogColorBrightness, 0.f, 1.5f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "FogColorBrightness", cb_luma_global_settings.GameSettings.FogColorBrightness);
      DrawResetButton(cb_luma_global_settings.GameSettings.FogColorBrightness, default_luma_global_game_settings.FogColorBrightness, "FogColorBrightness", runtime);

      if (ImGui::SliderFloat("Fog Keeps Surface Colour", &cb_luma_global_settings.GameSettings.FogKeepColor, 0.f, 1.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "FogKeepColor", cb_luma_global_settings.GameSettings.FogKeepColor);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Fogged surfaces keep part of their own saturation.");
      DrawResetButton(cb_luma_global_settings.GameSettings.FogKeepColor, default_luma_global_game_settings.FogKeepColor, "FogKeepColor", runtime);

#if DEVELOPMENT
      if (ImGui::SliderFloat("Vanilla Hue Restore", &cb_luma_global_settings.GameSettings.VanillaHueRestore, 0.f, 1.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "VanillaHueRestore", cb_luma_global_settings.GameSettings.VanillaHueRestore);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Restores the hue and saturation of the SDR image on highlights.");
      DrawResetButton(cb_luma_global_settings.GameSettings.VanillaHueRestore, default_luma_global_game_settings.VanillaHueRestore, "VanillaHueRestore", runtime);
#endif

      if (ImGui::SliderFloat("Bloom Intensity", &cb_luma_global_settings.GameSettings.BloomIntensity, 0.f, 2.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "BloomIntensity", cb_luma_global_settings.GameSettings.BloomIntensity);
      DrawResetButton(cb_luma_global_settings.GameSettings.BloomIntensity, default_luma_global_game_settings.BloomIntensity, "BloomIntensity", runtime);

#if DEVELOPMENT
      if (ImGui::SliderFloat("Gamut Expansion", &cb_luma_global_settings.GameSettings.GamutExpansion, 0.f, 1.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "GamutExpansion", cb_luma_global_settings.GameSettings.GamutExpansion);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Expands already saturated colors beyond BT.709 into wide gamut, keeping hue and brightness. Neutrals and skin tones are protected. 0 = vanilla");
      DrawResetButton(cb_luma_global_settings.GameSettings.GamutExpansion, default_luma_global_game_settings.GamutExpansion, "GamutExpansion", runtime);
#endif

      if (ImGui::Checkbox("SMAA", &replace_fxaa_with_smaa))
         reshade::set_config_value(runtime, NAME, "SMAAEnable", replace_fxaa_with_smaa);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Uses Luma's SMAA instead of the game's FXAA. Only applies while FXAA is enabled in the game's options.");

      if (ImGui::Checkbox("Hide Cursor", &hide_cursor))
         reshade::set_config_value(runtime, NAME, "HideCursor", hide_cursor);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Hides the Windows mouse cursor over the game window (the game runs borderless, so Windows no longer hides it).");

      if (ImGui::Checkbox("XeGTAO", &replace_ssao_with_xegtao))
         reshade::set_config_value(runtime, NAME, "XeGTAOEnable", replace_ssao_with_xegtao);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Uses XeGTAO instead of the game's ambient occlusion. Only applies while ambient occlusion is enabled in the game's options.");

#if DEVELOPMENT
      if (ImGui::SliderFloat("XeGTAO Intensity", &cb_luma_global_settings.GameSettings.XeGTAOIntensity, 0.5f, 3.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "XeGTAOIntensity", cb_luma_global_settings.GameSettings.XeGTAOIntensity);
      DrawResetButton(cb_luma_global_settings.GameSettings.XeGTAOIntensity, default_luma_global_game_settings.XeGTAOIntensity, "XeGTAOIntensity", runtime);

      if (ImGui::SliderFloat("XeGTAO Darkness", &cb_luma_global_settings.GameSettings.XeGTAODarkness, 0.f, 2.f, "%.2f"))
         reshade::set_config_value(runtime, NAME, "XeGTAODarkness", cb_luma_global_settings.GameSettings.XeGTAODarkness);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How much ambient occlusion darkens. 1 is the default, 0 disables it, 2 is twice as dark.");
      DrawResetButton(cb_luma_global_settings.GameSettings.XeGTAODarkness, default_luma_global_game_settings.XeGTAODarkness, "XeGTAODarkness", runtime);
#endif

      if (ImGui::SliderFloat("Shadow Softness", &cb_luma_global_settings.GameSettings.ShadowSoftness, 0.5f, 2.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "ShadowSoftness", cb_luma_global_settings.GameSettings.ShadowSoftness);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Width of the shadow edge filter. 1 is the vanilla width.");
      DrawResetButton(cb_luma_global_settings.GameSettings.ShadowSoftness, default_luma_global_game_settings.ShadowSoftness, "ShadowSoftness", runtime);

      // Stored as float in the cbuffer, saved to ReShade.ini as a bool-like value
      bool video_auto_hdr = cb_luma_global_settings.GameSettings.VideoAutoHDREnable > 0.f;
      if (ImGui::Checkbox("Video AutoHDR", &video_auto_hdr))
      {
         cb_luma_global_settings.GameSettings.VideoAutoHDREnable = video_auto_hdr ? 1.f : 0.f;
         reshade::set_config_value(runtime, NAME, "VideoAutoHDREnable", cb_luma_global_settings.GameSettings.VideoAutoHDREnable);
      }
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("Expands the highlights of the FMV videos into HDR. Only the videos are affected; UI and the rest of the game are untouched.");

      ImGui::BeginDisabled(cb_luma_global_settings.GameSettings.VideoAutoHDREnable <= 0.f);
      if (ImGui::SliderFloat("Video AutoHDR Strength", &cb_luma_global_settings.GameSettings.VideoAutoHDR, 0.f, 1.f, "%.3f"))
         reshade::set_config_value(runtime, NAME, "VideoAutoHDR", cb_luma_global_settings.GameSettings.VideoAutoHDR);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
         ImGui::SetTooltip("How far the video highlights reach toward the display peak brightness. 0 = paper white (no expansion).");
      DrawResetButton(cb_luma_global_settings.GameSettings.VideoAutoHDR, default_luma_global_game_settings.VideoAutoHDR, "VideoAutoHDR", runtime);
      ImGui::EndDisabled();
   }

   // dgVoodoo binds half-pixel viewports (x.5) on shadow atlas draws. Core asserts whole-pixel viewports when it rescales
   // the resized atlas mirror, so snap them to whole pixels. ReShade fires bind_* after the native call, so this runs after the game's bind.
   static void SnapShadowAtlasViewports(reshade::api::command_list* cmd_list)
   {
      BrutalLegend* bl = static_cast<BrutalLegend*>(game);
      if (!bl || bl->shadow_resolution_multiplier <= 1)
         return;
      const uint32_t mult = (uint32_t)bl->shadow_resolution_multiplier;

      ComPtr<ID3D11DeviceContext> native_device_context;
      ID3D11DeviceChild* device_child = (ID3D11DeviceChild*)(cmd_list->get_native());
      device_child->QueryInterface(native_device_context.put());
      if (!native_device_context)
         return;

      ComPtr<ID3D11RenderTargetView> rtv;
      ComPtr<ID3D11DepthStencilView> dsv;
      native_device_context->OMGetRenderTargets(1, rtv.put(), dsv.put());
      ComPtr<ID3D11Resource> resource;
      if (rtv)
         rtv->GetResource(resource.put());
      else if (dsv)
         dsv->GetResource(resource.put());
      ComPtr<ID3D11Texture2D> texture;
      if (!resource || FAILED(resource->QueryInterface(texture.put())))
         return;
      D3D11_TEXTURE2D_DESC desc = {};
      texture->GetDesc(&desc);
      const bool is_atlas = (desc.Width == 1024 && desc.Height == 4096) || (desc.Width == 1024 * mult && desc.Height == 4096 * mult);
      if (!is_atlas)
         return;

      D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
      UINT viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
      native_device_context->RSGetViewports(&viewport_count, viewports);
      bool changed = false;
      for (UINT i = 0; i < viewport_count; i++)
      {
         D3D11_VIEWPORT& vp = viewports[i];
         if (!(vp.Width > 0.f && vp.Height > 0.f))
            continue;
         const bool whole_size = (vp.Width == std::floor(vp.Width)) && (vp.Height == std::floor(vp.Height));
         const bool fractional_origin = (vp.TopLeftX != std::floor(vp.TopLeftX)) || (vp.TopLeftY != std::floor(vp.TopLeftY));
         if (!whole_size || !fractional_origin)
            continue;
         vp.TopLeftX = std::floor(vp.TopLeftX);
         vp.TopLeftY = std::floor(vp.TopLeftY);
         changed = true;
      }
      if (changed)
         native_device_context->RSSetViewports(viewport_count, viewports);
   }

#if DEVELOPMENT
   static void OnInitSamplerLog(reshade::api::device* device, const reshade::api::sampler_desc& desc, reshade::api::sampler sampler)
   {
      static std::mutex sampler_log_mutex;
      static std::set<std::tuple<uint32_t, float, float, float, float, uint32_t, uint32_t, uint32_t, uint32_t>> seen_samplers;
      const auto key = std::make_tuple(
         static_cast<uint32_t>(desc.filter), desc.max_anisotropy, desc.mip_lod_bias, desc.min_lod, desc.max_lod,
         static_cast<uint32_t>(desc.address_u), static_cast<uint32_t>(desc.address_v), static_cast<uint32_t>(desc.address_w),
         static_cast<uint32_t>(desc.compare_op));
      std::lock_guard<std::mutex> lock(sampler_log_mutex);
      if (seen_samplers.insert(key).second)
      {
         reshade::log::message(reshade::log::level::info, std::format("[BL] sampler #{}: filter=0x{:X} aniso={} bias={} lod=[{},{}] addr={}/{}/{} cmp={}",
            seen_samplers.size(), std::get<0>(key), std::get<1>(key), std::get<2>(key), std::get<3>(key), std::get<4>(key),
            std::get<5>(key), std::get<6>(key), std::get<7>(key), std::get<8>(key)).c_str());
      }
   }
#endif

   static void OnBindViewports(reshade::api::command_list* cmd_list, uint32_t first, uint32_t count, const reshade::api::viewport* viewports)
   {
      SnapShadowAtlasViewports(cmd_list);
   }

   static void OnBindRenderTargetsAndDepthStencil(reshade::api::command_list* cmd_list, uint32_t count, const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv)
   {
      SnapShadowAtlasViewports(cmd_list);
   }

   void PrintImGuiAbout() override
   {
      // Remember to credit Luma developers, the game mod creators, and all third party code that is used (plus, optionally, testers too)
      ImGui::Text("Brutal Legend Luma mod - about and credits section", "");
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
       Globals::SetGlobals(cleared_project_name, "Brutal Legend Luma mod", "" /*E.g. Nexus link*/, mod_version);

       // scRGB fp16 swapchain (the game's backbuffer is 8-bit).
       swapchain_format_upgrade_type = TextureFormatUpgradesType::AllowedEnabled;
       swapchain_upgrade_type = SwapchainUpgradeType::scRGB;

       // Exclusive fullscreen -> borderless, as in the other dgVoodoo mods (MoHA/ME1): "force_borderless" covers
       // LEAVING fullscreen too, so the window cannot come back with a title bar after alt-tab (TW2).
       prevent_fullscreen_state = true;
       force_borderless = true;

       // dgVoodoo scheme (MoHA main.cpp): changing a dgVoodoo resource's creation format at creation breaks the
       // translator's bookkeeping - black screen even in menus - so only the two clip-capable families are upgraded,
       // INDIRECTLY (creation-time mirrors substituted at bind), keyed by FORMAT. The broad template list must not be
       // used under dgVoodoo: it is what caused the black screen here.
       texture_format_upgrades_type = TextureFormatUpgradesType::AllowedEnabled;
       enable_indirect_texture_format_upgrades = true; // creation-time mirrors, substituted at bind (BL2/TW2 scheme)
       enable_chain_indirect_texture_format_upgrades = ChainTextureFormatUpgradesType::DirectDependencies;
       texture_upgrade_formats = {
          reshade::api::format::r8g8b8a8_typeless,     // dgVoodoo's D3D9 backbuffer surface (the LDR canvas)
          reshade::api::format::r16g16b16a16_typeless, // fp16 post/bloom targets the game VIEWS as unorm
       };
       // "No1Px" is mandatory under dgVoodoo (BL2): the wrapper binds 1x1 placeholders in every unused sampler slot
       // and a 1x1 trivially passes the aspect filter, so core would mirror those too (and assert in DEVELOPMENT).
       texture_format_upgrades_2d_size_filters = (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainResolution | (uint32_t)TextureFormatUpgrades2DSizeFilters::SwapchainAspectRatio | (uint32_t)TextureFormatUpgrades2DSizeFilters::No1Px;

       // Update samples to override the bias, based on the rendering resolution etc
       // Forces every game ANISOTROPIC sampler (in-game AF toggle) to 16x. Bias offset stays 0 (no SR/TAA here).
       enable_samplers_upgrade = true;
       force_upgrade_linear_samplers = true; // Also trilinear (MIN_MAG_MIP_LINEAR) samplers: terrain/detail layers never get the game's AF

#if DEVELOPMENT // If you want to track any shader names over time, you can hardcode them here by hash (they can be a useful reference in the pipeline)
      forced_shader_names.emplace(std::stoul("FD2925B4", nullptr, 16), "Tracked Shader Name");
#endif

#if !DEVELOPMENT // Put shaders that a previous version of the mod used but has ever since been deleted here, so that users updating the mod from an older version won't accidentally load them
      old_shader_file_names.emplace("Bloom_0xDC9373A8.ps_5_0.hlsl");
#endif

      // Create your game sub-class instance (it will be automatically destroyed on exit).
      // You do not need to do this if you have no custom data to store.
      game = new BrutalLegend();
   }
   else if (ul_reason_for_call == DLL_PROCESS_DETACH)
   {
      reshade::unregister_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(BrutalLegend::OnBindRenderTargetsAndDepthStencil);
      reshade::unregister_event<reshade::addon_event::bind_viewports>(BrutalLegend::OnBindViewports);
#if DEVELOPMENT
      reshade::unregister_event<reshade::addon_event::init_sampler>(BrutalLegend::OnInitSamplerLog);
#endif
   }

   CoreMain(hModule, ul_reason_for_call, lpReserved);

   return TRUE;
}
