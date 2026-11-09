#pragma once

// Anamorphic streak (horizontal lens streak), ported from KinoStreak by Keijiro Takahashi (MIT License, Copyright (c) 2017).
// Full license text in Shaders/Red Faction Guerrilla Remastered/LICENSE_KinoStreak.txt.
// Header-only. Included by main.cpp after core.hpp. Shaders: Shaders/Red Faction Guerrilla Remastered/Luma_RFG_Streak_*.hlsl (cbuffer in Includes/RFGStreak.hlsl).
// Pass sequence (same as Kino's Streak.cs): prefilter (half height) -> downsample chain (halve width while > 16) -> upsample with stretch -> composite (full res).

struct RFGStreakTarget
{
   ComPtr<ID3D11Texture2D> tex;
   ComPtr<ID3D11ShaderResourceView> srv;
   ComPtr<ID3D11RenderTargetView> rtv;
   UINT w = 0, h = 0;
};

static void RFGStreak_ResetTarget(RFGStreakTarget& t)
{
   t.tex.reset();
   t.srv.reset();
   t.rtv.reset();
   t.w = 0;
   t.h = 0;
}

struct RFGStreakResources
{
   RFGStreakTarget prefilter;         // W x H/2
   std::vector<RFGStreakTarget> down; // down[i]: (W >> (i+1)) x H/2, Kino's MIP pyramid. The last one is the smallest level.
   std::vector<RFGStreakTarget> up;   // up[i]: same size as down[i], for i < down.size() - 1
   RFGStreakTarget streak;            // W x H, composite output. Bound at t3 for the tonemap draw.
   ComPtr<ID3D11Buffer> cb;
   ComPtr<ID3D11SamplerState> smp_clamp;

   void Release()
   {
      RFGStreak_ResetTarget(prefilter);
      for (auto& t : down)
         RFGStreak_ResetTarget(t);
      for (auto& t : up)
         RFGStreak_ResetTarget(t);
      down.clear();
      up.clear();
      RFGStreak_ResetTarget(streak);
      cb.reset();
      smp_clamp.reset();
   }
};

// Mirrors the "RFGStreakCB" cbuffer in Shaders/Red Faction Guerrilla Remastered/Includes/RFGStreak.hlsl (3 float4 = 48 bytes).
struct RFGStreakCBData
{
   float InSize[4];  // xy = input size (px), zw = 1/xy
   float OutSize[4]; // xy = output size (px), zw = 1/xy
   float Params[4];  // x = threshold (paper white units), y = stretch
};
static_assert(sizeof(RFGStreakCBData) == 48, "RFGStreakCBData must match RFGStreakCB in RFGStreak.hlsl");

// Registers the streak shaders (called from OnInit). The vertex shader is the core "Copy VS".
static void RFG_RegisterStreakShaders()
{
   native_shaders_definitions.emplace(CompileTimeStringHash("RFG Streak Prefilter"), ShaderDefinition{ "Luma_RFG_Streak_Prefilter", reshade::api::pipeline_subobject_type::pixel_shader });
   native_shaders_definitions.emplace(CompileTimeStringHash("RFG Streak Down"), ShaderDefinition{ "Luma_RFG_Streak_Down", reshade::api::pipeline_subobject_type::pixel_shader });
   native_shaders_definitions.emplace(CompileTimeStringHash("RFG Streak Up"), ShaderDefinition{ "Luma_RFG_Streak_Up", reshade::api::pipeline_subobject_type::pixel_shader });
   native_shaders_definitions.emplace(CompileTimeStringHash("RFG Streak Composite"), ShaderDefinition{ "Luma_RFG_Streak_Composite", reshade::api::pipeline_subobject_type::pixel_shader });
}

static ID3D11PixelShader* RFGStreak_FindPS(DeviceData& device_data, uint32_t hash)
{
   auto it = device_data.native_pixel_shaders.find(hash);
   return (it != device_data.native_pixel_shaders.end()) ? it->second.get() : nullptr;
}

static ID3D11VertexShader* RFGStreak_FindVS(DeviceData& device_data, uint32_t hash)
{
   auto it = device_data.native_vertex_shaders.find(hash);
   return (it != device_data.native_vertex_shaders.end()) ? it->second.get() : nullptr;
}

// Reads the size of a non-MSAA 2D texture behind an SRV.
static bool RFGStreak_GetSRVSize(ID3D11ShaderResourceView* srv, UINT& w, UINT& h)
{
   ComPtr<ID3D11Resource> resource;
   srv->GetResource(resource.put());
   if (!resource)
      return false;
   ComPtr<ID3D11Texture2D> tex;
   if (FAILED(resource->QueryInterface(tex.put())))
      return false;
   D3D11_TEXTURE2D_DESC desc = {};
   tex->GetDesc(&desc);
   if (desc.Width == 0 || desc.Height == 0 || desc.SampleDesc.Count != 1)
      return false;
   w = desc.Width;
   h = desc.Height;
   return true;
}

static UINT RFGStreak_Div(UINT v, UINT d)
{
   UINT r = v / d;
   return r > 0 ? r : 1;
}

// (Re)creates an R16G16B16A16_FLOAT RT+SRV of the given size. Keeps the old one if it already matches.
static bool RFGStreak_EnsureTarget(ID3D11Device* device, RFGStreakTarget& t, UINT w, UINT h)
{
   if (t.tex && t.srv && t.rtv && t.w == w && t.h == h)
      return true;
   RFGStreak_ResetTarget(t);

   D3D11_TEXTURE2D_DESC desc = {};
   desc.Width = w;
   desc.Height = h;
   desc.MipLevels = 1;
   desc.ArraySize = 1;
   desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
   desc.SampleDesc.Count = 1;
   desc.Usage = D3D11_USAGE_DEFAULT;
   desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

   ComPtr<ID3D11Texture2D> tex;
   ComPtr<ID3D11ShaderResourceView> srv;
   ComPtr<ID3D11RenderTargetView> rtv;
   if (FAILED(device->CreateTexture2D(&desc, nullptr, tex.put()))
      || FAILED(device->CreateShaderResourceView(tex.get(), nullptr, srv.put()))
      || FAILED(device->CreateRenderTargetView(tex.get(), nullptr, rtv.put())))
      return false;

   t.tex = tex;
   t.srv = srv;
   t.rtv = rtv;
   t.w = w;
   t.h = h;
   return true;
}

// Creates the scene-size independent objects once: constant buffer and a linear clamp sampler at s0.
static bool RFGStreak_EnsureStatics(ID3D11Device* device, RFGStreakResources& res)
{
   if (!res.cb)
   {
      D3D11_BUFFER_DESC desc = {};
      desc.ByteWidth = sizeof(RFGStreakCBData); // 48, multiple of 16
      desc.Usage = D3D11_USAGE_DYNAMIC;
      desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      ComPtr<ID3D11Buffer> cb;
      if (FAILED(device->CreateBuffer(&desc, nullptr, cb.put())))
         return false;
      res.cb = cb;
   }

   if (!res.smp_clamp)
   {
      D3D11_SAMPLER_DESC desc = {};
      desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
      desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
      desc.MaxAnisotropy = 1;
      desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
      desc.MaxLOD = D3D11_FLOAT32_MAX;
      ComPtr<ID3D11SamplerState> smp;
      if (FAILED(device->CreateSamplerState(&desc, smp.put())))
         return false;
      res.smp_clamp = smp;
   }

   return true;
}

static void RFGStreak_Upload(ID3D11DeviceContext* ctx, ID3D11Buffer* cb, const RFGStreakCBData& data)
{
   D3D11_MAPPED_SUBRESOURCE mapped = {};
   if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
   {
      std::memcpy(mapped.pData, &data, sizeof(data));
      ctx->Unmap(cb, 0);
   }
}

// Runs the streak passes on the tonemap's HDR scene. scene_srv = tonemap PS t0 (stored /8), exposure_srv = tonemap PS t1 (.y = k).
// Returns true and sets out_srv (non-owning, = res.streak.srv) on success. Returns false if any shader or input is missing.
static bool RFGStreak_Draw(ID3D11Device* device, ID3D11DeviceContext* ctx, DeviceData& device_data, RFGStreakResources& res,
   float threshold, float stretch, ID3D11ShaderResourceView* scene_srv, ID3D11ShaderResourceView* exposure_srv, ID3D11ShaderResourceView** out_srv)
{
   if (out_srv == nullptr)
      return false;
   *out_srv = nullptr;
   if (device == nullptr || ctx == nullptr || scene_srv == nullptr || exposure_srv == nullptr)
      return false;

   ID3D11PixelShader* ps_prefilter = RFGStreak_FindPS(device_data, CompileTimeStringHash("RFG Streak Prefilter"));
   ID3D11PixelShader* ps_down = RFGStreak_FindPS(device_data, CompileTimeStringHash("RFG Streak Down"));
   ID3D11PixelShader* ps_up = RFGStreak_FindPS(device_data, CompileTimeStringHash("RFG Streak Up"));
   ID3D11PixelShader* ps_composite = RFGStreak_FindPS(device_data, CompileTimeStringHash("RFG Streak Composite"));
   ID3D11VertexShader* vs_copy = RFGStreak_FindVS(device_data, CompileTimeStringHash("Copy VS"));
   if (!ps_prefilter || !ps_down || !ps_up || !ps_composite || !vs_copy)
      return false;

   UINT scene_w = 0, scene_h = 0;
   if (!RFGStreak_GetSRVSize(scene_srv, scene_w, scene_h))
      return false;

   // Kino: the pyramid halves the width while it is above 16 (minimum width 8). Level count = number of halvings.
   size_t levels = 0;
   for (UINT w = scene_w; w > 16; )
   {
      w /= 2;
      levels++;
   }
   if (levels == 0)
      return false;

   const UINT half_h = RFGStreak_Div(scene_h, 2);
   if (!RFGStreak_EnsureStatics(device, res))
      return false;
   if (!RFGStreak_EnsureTarget(device, res.prefilter, scene_w, half_h))
      return false;
   res.down.resize(levels);
   res.up.resize(levels - 1);
   UINT level_w = scene_w;
   for (size_t i = 0; i < levels; i++)
   {
      level_w /= 2;
      if (!RFGStreak_EnsureTarget(device, res.down[i], level_w, half_h))
         return false;
   }
   for (size_t i = 0; i + 1 < levels; i++)
   {
      if (!RFGStreak_EnsureTarget(device, res.up[i], res.down[i].w, res.down[i].h))
         return false;
   }
   if (!RFGStreak_EnsureTarget(device, res.streak, scene_w, scene_h))
      return false;

   DrawStateStack<DrawStateStackType::FullGraphics> draw_state_stack;
   draw_state_stack.Cache(ctx, device_data.uav_max_count);

   ID3D11SamplerState* smp = res.smp_clamp.get();
   ctx->PSSetSamplers(0, 1, &smp);
   ID3D11Buffer* cb = res.cb.get();
   ctx->PSSetConstantBuffers(0, 1, &cb);

   // Fullscreen pass helper. src is bound at t0, optional t1 is bound after the inputs are cleared.
   auto draw = [&](ID3D11PixelShader* ps, ID3D11ShaderResourceView* src, UINT src_w, UINT src_h, RFGStreakTarget& dst, const char* name, ID3D11ShaderResourceView* t1)
   {
      ctx->OMSetRenderTargets(0, nullptr, nullptr); // The previous pass's output is still bound as RTV
      ID3D11ShaderResourceView* ps_nulls[4] = {};
      ctx->PSSetShaderResources(0, 4, ps_nulls);

      RFGStreakCBData cbd = {};
      cbd.InSize[0] = (float)src_w;
      cbd.InSize[1] = (float)src_h;
      cbd.InSize[2] = 1.f / (float)src_w;
      cbd.InSize[3] = 1.f / (float)src_h;
      cbd.OutSize[0] = (float)dst.w;
      cbd.OutSize[1] = (float)dst.h;
      cbd.OutSize[2] = 1.f / (float)dst.w;
      cbd.OutSize[3] = 1.f / (float)dst.h;
      cbd.Params[0] = threshold;
      cbd.Params[1] = stretch;
      RFGStreak_Upload(ctx, cb, cbd);

      if (t1)
         ctx->PSSetShaderResources(1, 1, &t1);
      DrawCustomPixelShader(ctx, device_data.default_depth_stencil_state.get(), device_data.default_blend_state.get(), nullptr,
         vs_copy, ps, src, dst.rtv.get(), dst.w, dst.h, true, name);
   };

   // 1. Prefilter: scene (t0) + exposure (t1) -> prefilter (W x H/2), threshold applied.
   draw(ps_prefilter, scene_srv, scene_w, scene_h, res.prefilter, "RFG Streak Prefilter", exposure_srv);

   // 2. Downsample chain: prefilter -> down[0] -> ... -> down[levels-1].
   ID3D11ShaderResourceView* prev_srv = res.prefilter.srv.get();
   UINT prev_w = res.prefilter.w, prev_h = res.prefilter.h;
   for (size_t i = 0; i < levels; i++)
   {
      draw(ps_down, prev_srv, prev_w, prev_h, res.down[i], "RFG Streak Down", nullptr);
      prev_srv = res.down[i].srv.get();
      prev_w = res.down[i].w;
      prev_h = res.down[i].h;
   }

   // 3. Upsample: from the smallest level back up, up[i] = lerp(down[i], last, stretch). The smallest level is the start point (Kino cuts it from the stack).
   ID3D11ShaderResourceView* cur_srv = res.down[levels - 1].srv.get();
   UINT cur_w = res.down[levels - 1].w, cur_h = res.down[levels - 1].h;
   for (size_t i = levels - 1; i-- > 0; )
   {
      draw(ps_up, cur_srv, cur_w, cur_h, res.up[i], "RFG Streak Up", res.down[i].srv.get());
      cur_srv = res.up[i].srv.get();
      cur_w = res.up[i].w;
      cur_h = res.up[i].h;
   }

   // 4. Composite: the last upsample level -> streak (W x H).
   draw(ps_composite, cur_srv, cur_w, cur_h, res.streak, "RFG Streak Composite", nullptr);

   draw_state_stack.Restore(ctx);

   *out_srv = res.streak.srv.get();
   return true;
}
