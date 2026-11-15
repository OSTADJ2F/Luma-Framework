#pragma once

// Brütal Legend: filters the redundant D3D9 state calls the game makes into dgVoodoo's d3d9.dll (all builds), and
// counts calls, redundant calls, filtered calls and register traffic (DEVELOPMENT only, logged every 300 frames).
// The device vtable is found through the patched dgVoodoo GetTexture site (main.cpp: TryPatchDgVoodooGetTextureBounds).

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "d3d9_cs_dispatch.h"

#if DEVELOPMENT
#define BLD3D_DEV(...) __VA_ARGS__
#else
#define BLD3D_DEV(...)
#endif

// Set by TryPatchDgVoodooGetTextureBounds() on success: the patched GetTexture site inside dgVoodoo's d3d9.dll.
static uintptr_t g_dgv_gettexture_site = 0;

// Drop calls that would set the value the device already has (shadow valid and equal). DEVELOPMENT toggle "d3d9.filter".
static bool g_d3d9_filter = true;

enum D3DStatMethod : int
{
   D3DStat_Present,
   D3DStat_Reset,
   D3DStat_SetRenderTarget,
   D3DStat_SetDepthStencilSurface,
   D3DStat_Clear,
   D3DStat_SetViewport,
   D3DStat_SetRenderState,
   D3DStat_BeginStateBlock,
   D3DStat_EndStateBlock,
   D3DStat_SetTexture,
   D3DStat_SetTextureStageState,
   D3DStat_SetSamplerState,
   D3DStat_SetScissorRect,
   D3DStat_DrawPrimitive,
   D3DStat_DrawIndexedPrimitive,
   D3DStat_DrawPrimitiveUP,
   D3DStat_DrawIndexedPrimitiveUP,
   D3DStat_SetVertexDeclaration,
   D3DStat_SetFVF,
   D3DStat_SetVertexShader,
   D3DStat_SetVertexShaderConstantF,
   D3DStat_SetVertexShaderConstantI,
   D3DStat_SetVertexShaderConstantB,
   D3DStat_SetStreamSource,
   D3DStat_SetIndices,
   D3DStat_SetPixelShader,
   D3DStat_SetPixelShaderConstantF,
   D3DStat_Count
};

#if DEVELOPMENT
static const char* const g_d3d_stat_names[D3DStat_Count] = {
   "Present", "Reset", "SetRenderTarget", "SetDepthStencilSurface", "Clear", "SetViewport", "SetRenderState",
   "BeginStateBlock", "EndStateBlock", "SetTexture", "SetTextureStageState", "SetSamplerState", "SetScissorRect",
   "DrawPrimitive", "DrawIndexedPrimitive", "DrawPrimitiveUP", "DrawIndexedPrimitiveUP", "SetVertexDeclaration",
   "SetFVF", "SetVertexShader", "SetVertexShaderConstantF", "SetVertexShaderConstantI", "SetVertexShaderConstantB",
   "SetStreamSource", "SetIndices", "SetPixelShader", "SetPixelShaderConstantF"
};

// Methods that have a redundancy shadow.
static const bool g_d3d_stat_tracked[D3DStat_Count] = {
   false, false, false, false, false, false, true,  // Present .. SetRenderState
   false, false, true, true, true, false,           // BeginStateBlock .. SetScissorRect (SetTexture, TSS, Sampler tracked)
   false, false, false, false,                      // Draw*
   true, true, true, true, false, false, true, true, true, // SetVertexDeclaration .. SetPixelShader (VS F tracked)
   true                                             // SetPixelShaderConstantF
};
#endif

// Shadow of the last value passed per state. valid = false means unknown (never set since the last invalidation).
struct D3DStatConsts
{
   float v[256][4];
   bool valid[256];
};

struct D3DStatShadow
{
   uintptr_t rs[256];
   bool rs_valid[256];
   uintptr_t samp[20][14];
   bool samp_valid[20][14];
   uintptr_t tex[260];
   bool tex_valid[260];
   uintptr_t tss[8][33];
   bool tss_valid[8][33];
   D3DStatConsts vs;
   D3DStatConsts ps;
   uintptr_t stream_vb[16];
   uintptr_t stream_off[16];
   uintptr_t stream_stride[16];
   bool stream_valid[16];
   uintptr_t indices;
   bool indices_valid;
   uintptr_t vs_shader;
   bool vs_valid;
   uintptr_t ps_shader;
   bool ps_valid;
   uintptr_t decl;
   bool decl_valid;
   uintptr_t fvf;
   bool fvf_valid;
};

struct D3DStatCounter
{
   uint32_t calls;
   uint32_t sb_calls;       // recorded between BeginStateBlock and EndStateBlock (never filtered, no shadow update)
   uint32_t redundant;      // value equal to the shadow
   uint32_t filtered;       // dropped without calling the original
   uint32_t regs;           // constant F: registers in the call
   uint32_t regs_sent;      // constant F: registers passed to the original after trimming
   uint32_t regs_unchanged; // constant F: registers equal to the shadow
};

enum class D3DCheck { Send, Drop, Foreign };

static D3DStatShadow g_d3d_stat_shadow = {};
static D3DStatCounter g_d3d_stat[D3DStat_Count] = {};
static uint32_t g_d3d_stat_frames = 0;
static uint32_t g_d3d_stat_foreign = 0;
static bool g_d3d_stat_in_block = false;
static DWORD g_d3d_owner_thread = 0; // thread of the last draw call; state calls from other threads pass through
static bool g_d3d_toggle_pending = false; // filter toggled on: invalidate at the next draw
static bool g_d3d_sb_unhooked = false; // a second state block vtable appeared: filtering stays off
static void* g_d3d_stat_orig[128] = {};
static bool g_d3d_stat_installed = false;
static bool g_d3d_stat_attempted = false;
static void* g_d3d_sb_vtable = nullptr;
static void* g_d3d_sb_apply_orig = nullptr;

static void D3DInvalidateAll()
{
   g_d3d_stat_shadow = D3DStatShadow{};
}

static bool D3DForeignThread()
{
   return g_d3d_owner_thread != 0 && GetCurrentThreadId() != g_d3d_owner_thread;
}

static bool D3DFilterAllowed()
{
   return g_d3d9_filter && !g_d3d_sb_unhooked && !g_d3d_stat_in_block && g_d3d_owner_thread != 0 && GetCurrentThreadId() == g_d3d_owner_thread;
}

// Draw calls define the owner (render) thread. A draw from another thread takes ownership and invalidates everything.
static void D3DClaimDrawThread()
{
   const DWORD current = GetCurrentThreadId();
   if (g_d3d_owner_thread != current)
   {
      D3DInvalidateAll();
      g_d3d_owner_thread = current;
   }
   if (g_d3d_toggle_pending)
   {
      D3DInvalidateAll();
      g_d3d_toggle_pending = false;
   }
}

// Counts one call. Returns true when it is recorded into a state block (the caller passes it through untouched).
static bool D3DNote(int m)
{
   const bool recording = g_d3d_stat_in_block && m != D3DStat_EndStateBlock;
   BLD3D_DEV(if (recording) g_d3d_stat[m].sb_calls++; else g_d3d_stat[m].calls++;)
   return recording;
}

// Decides for one scalar-style state. same = shadow valid and equal to the new value.
// Foreign-thread calls only invalidate their shadow entry.
static D3DCheck D3DCheckShadow(int m, bool& valid, bool same)
{
   if (D3DForeignThread())
   {
      valid = false;
      BLD3D_DEV(g_d3d_stat_foreign++;)
      return D3DCheck::Foreign;
   }
   BLD3D_DEV(if (same) g_d3d_stat[m].redundant++;)
   if (same && D3DFilterAllowed())
   {
      BLD3D_DEV(g_d3d_stat[m].filtered++;)
      return D3DCheck::Drop;
   }
   return D3DCheck::Send;
}

// Shared logic for SetVertexShaderConstantF / SetPixelShaderConstantF. call(start, data, count4) forwards to the original.
template <typename Call>
static HRESULT D3DConstF(int m, D3DStatConsts& c, UINT start, const float* data, UINT count4, Call&& call)
{
   if (D3DNote(m))
      return call(start, data, count4);
   BLD3D_DEV(g_d3d_stat[m].regs += count4;)

   const bool in_range = data != nullptr && count4 > 0 && count4 <= 256 && start <= 256 - count4;
   if (!in_range || D3DForeignThread())
   {
      BLD3D_DEV(if (D3DForeignThread()) g_d3d_stat_foreign++;)
      for (UINT i = 0; i < count4 && start + i < 256; ++i)
         c.valid[start + i] = false;
      return call(start, data, count4);
   }

   UINT first = 0;
   UINT last = 0;
   UINT unchanged = 0;
   bool any = false;
   for (UINT i = 0; i < count4; ++i)
   {
      if (c.valid[start + i] && std::memcmp(c.v[start + i], data + 4 * i, sizeof(float) * 4) == 0)
      {
         ++unchanged;
         continue;
      }
      if (!any)
      {
         first = i;
         any = true;
      }
      last = i;
   }
   BLD3D_DEV(g_d3d_stat[m].regs_unchanged += unchanged; if (!any) g_d3d_stat[m].redundant++;)

   const bool filter = D3DFilterAllowed();
   if (!any && filter)
   {
      BLD3D_DEV(g_d3d_stat[m].filtered++;)
      return S_OK;
   }

   UINT send_start = start;
   UINT send_count = count4;
   const float* send_data = data;
   if (any && filter)
   {
      send_start = start + first;
      send_count = last - first + 1;
      send_data = data + 4 * first;
   }
   BLD3D_DEV(g_d3d_stat[m].regs_sent += send_count;)

   const HRESULT hr = call(send_start, send_data, send_count);
   if (SUCCEEDED(hr))
   {
      for (UINT i = 0; i < count4; ++i)
      {
         std::memcpy(c.v[start + i], data + 4 * i, sizeof(float) * 4);
         c.valid[start + i] = true;
      }
   }
   return hr;
}

#if DEVELOPMENT
static void D3DStatLogWindow()
{
   const double frames = static_cast<double>(g_d3d_stat_frames);

   uint64_t draws = 0;
   for (int m = D3DStat_DrawPrimitive; m <= D3DStat_DrawIndexedPrimitiveUP; ++m)
      draws += g_d3d_stat[m].calls;
   reshade::log::message(reshade::log::level::info, std::format("[BL D3D9] over {} frames: draws/frame {:.1f}, foreign-thread calls {} (filter {})", g_d3d_stat_frames, static_cast<double>(draws) / frames, g_d3d_stat_foreign, g_d3d9_filter ? "on" : "off").c_str());

   for (int m = 0; m < D3DStat_Count; ++m)
   {
      const D3DStatCounter& c = g_d3d_stat[m];
      if (c.calls == 0 && c.sb_calls == 0)
         continue;

      std::string line = std::format("[BL D3D9] {}: {:.1f}/frame", g_d3d_stat_names[m], static_cast<double>(c.calls) / frames);
      if (g_d3d_stat_tracked[m])
         line += std::format(", {:.1f}% redundant, filtered {:.1f}/frame", c.calls ? 100.0 * c.redundant / c.calls : 0.0, static_cast<double>(c.filtered) / frames);
      if (m == D3DStat_SetVertexShaderConstantF || m == D3DStat_SetPixelShaderConstantF)
         line += std::format(", regs {:.1f}/frame, sent {:.1f}/frame, {:.1f}% unchanged", static_cast<double>(c.regs) / frames, static_cast<double>(c.regs_sent) / frames, c.regs ? 100.0 * c.regs_unchanged / c.regs : 0.0);
      if (c.sb_calls)
         line += std::format(", {:.1f}/frame recorded into state blocks", static_cast<double>(c.sb_calls) / frames);
      reshade::log::message(reshade::log::level::info, line.c_str());
   }

   std::memset(g_d3d_stat, 0, sizeof(g_d3d_stat));
   g_d3d_stat_frames = 0;
   g_d3d_stat_foreign = 0;
}
#endif

// ---- __stdcall wrappers: count, filter, update shadows after success, call the original vtable entry ----

static HRESULT __stdcall D3DWrap_Present(void* self, const RECT* src, const RECT* dst, HWND hwnd, const void* dirty)
{
   static bool last_filter = true;
   if (g_d3d9_filter && !last_filter)
      g_d3d_toggle_pending = true; // filter turned on: invalidated at the next draw on the owner thread
   last_filter = g_d3d9_filter;

   D3DNote(D3DStat_Present);
   if (!g_cs_device_hooked)
      CsStatFrame(0, 0); // with the CS device hook, CsPresent counts the frame (exactly one CsStatFrame per Present)
   BLD3D_DEV(if (++g_d3d_stat_frames >= 300) { D3DStatLogWindow(); CsStatLogWindow(); CsCountOnlyLogWindow(300); })
   return reinterpret_cast<HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*)>(g_d3d_stat_orig[17])(self, src, dst, hwnd, dirty);
}

static HRESULT __stdcall D3DWrap_Reset(void* self, void* params)
{
   D3DNote(D3DStat_Reset);
   D3DInvalidateAll();
   const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_d3d_stat_orig[16])(self, params);
   D3DInvalidateAll();
   return hr;
}

static HRESULT __stdcall D3DWrap_SetRenderTarget(void* self, DWORD index, void* surface)
{
   D3DNote(D3DStat_SetRenderTarget);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(g_d3d_stat_orig[37])(self, index, surface);
}

static HRESULT __stdcall D3DWrap_SetDepthStencilSurface(void* self, void* surface)
{
   D3DNote(D3DStat_SetDepthStencilSurface);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_d3d_stat_orig[39])(self, surface);
}

static HRESULT __stdcall D3DWrap_Clear(void* self, DWORD count, const void* rects, DWORD flags, DWORD color, float z, DWORD stencil)
{
   D3DNote(D3DStat_Clear);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, const void*, DWORD, DWORD, float, DWORD)>(g_d3d_stat_orig[43])(self, count, rects, flags, color, z, stencil);
}

static HRESULT __stdcall D3DWrap_SetViewport(void* self, const void* viewport)
{
   D3DNote(D3DStat_SetViewport);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, const void*)>(g_d3d_stat_orig[47])(self, viewport);
}

static HRESULT __stdcall D3DWrap_SetRenderState(void* self, DWORD state, DWORD value)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD)>(g_d3d_stat_orig[57]);
   if (D3DNote(D3DStat_SetRenderState) || state >= 256)
      return fn(self, state, value);
   auto& s = g_d3d_stat_shadow;
   const D3DCheck check = D3DCheckShadow(D3DStat_SetRenderState, s.rs_valid[state], s.rs_valid[state] && s.rs[state] == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, state, value);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.rs[state] = value;
      s.rs_valid[state] = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_BeginStateBlock(void* self)
{
   D3DNote(D3DStat_BeginStateBlock);
   g_d3d_stat_in_block = true;
   return reinterpret_cast<HRESULT(__stdcall*)(void*)>(g_d3d_stat_orig[60])(self);
}

static void D3DHookStateBlock(void* block);

static HRESULT __stdcall D3DWrap_EndStateBlock(void* self, void** block)
{
   D3DNote(D3DStat_EndStateBlock);
   g_d3d_stat_in_block = false;
   D3DInvalidateAll();
   const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void**)>(g_d3d_stat_orig[61])(self, block);
   if (SUCCEEDED(hr) && block != nullptr && *block != nullptr)
      D3DHookStateBlock(*block);
   return hr;
}

static HRESULT __stdcall D3DWrap_CreateStateBlock(void* self, UINT type, void** block)
{
   const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, void**)>(g_d3d_stat_orig[59])(self, type, block);
   if (SUCCEEDED(hr) && block != nullptr && *block != nullptr && !CsSbIsProxy(*block))
      D3DHookStateBlock(*block);
   return hr;
}

// IDirect3DStateBlock9 slot 5 is Apply: applying a block changes device state behind the shadows.
static HRESULT __stdcall D3DWrap_SBApply(void* self)
{
   const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*)>(g_d3d_sb_apply_orig)(self);
   D3DInvalidateAll();
   return hr;
}

// Patches the Apply slot of the first state block vtable seen (idempotent). A different vtable disables filtering.
static void D3DHookStateBlock(void* block)
{
   void** vt = *reinterpret_cast<void***>(block);
   if (vt == g_d3d_sb_vtable)
      return;
   if (g_d3d_sb_vtable != nullptr)
   {
      if (!g_d3d_sb_unhooked)
         reshade::log::message(reshade::log::level::warning, "[BL D3D9] filter: second state block vtable, Apply not hooked, filtering disabled");
      g_d3d_sb_unhooked = true;
      return;
   }
   DWORD old_protection = 0;
   if (!VirtualProtect(&vt[5], sizeof(void*), PAGE_READWRITE, &old_protection))
   {
      g_d3d_sb_unhooked = true;
      return;
   }
   g_d3d_sb_apply_orig = vt[5];
   vt[5] = reinterpret_cast<void*>(&D3DWrap_SBApply);
   DWORD ignored = 0;
   VirtualProtect(&vt[5], sizeof(void*), old_protection, &ignored);
   g_d3d_sb_vtable = vt;
}

static HRESULT __stdcall D3DWrap_SetTexture(void* self, DWORD stage, void* texture)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, void*)>(g_d3d_stat_orig[65]);
   if (D3DNote(D3DStat_SetTexture) || stage >= 260)
      return fn(self, stage, texture);
   auto& s = g_d3d_stat_shadow;
   const uintptr_t value = reinterpret_cast<uintptr_t>(texture);
   const D3DCheck check = D3DCheckShadow(D3DStat_SetTexture, s.tex_valid[stage], s.tex_valid[stage] && s.tex[stage] == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, stage, texture);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.tex[stage] = value;
      s.tex_valid[stage] = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetTextureStageState(void* self, DWORD stage, DWORD type, DWORD value)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD)>(g_d3d_stat_orig[67]);
   if (D3DNote(D3DStat_SetTextureStageState) || stage >= 8 || type >= 33)
      return fn(self, stage, type, value);
   auto& s = g_d3d_stat_shadow;
   const D3DCheck check = D3DCheckShadow(D3DStat_SetTextureStageState, s.tss_valid[stage][type], s.tss_valid[stage][type] && s.tss[stage][type] == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, stage, type, value);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.tss[stage][type] = value;
      s.tss_valid[stage][type] = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetSamplerState(void* self, DWORD sampler, DWORD type, DWORD value)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD)>(g_d3d_stat_orig[69]);
   if (D3DNote(D3DStat_SetSamplerState) || sampler >= 20 || type >= 14)
      return fn(self, sampler, type, value);
   auto& s = g_d3d_stat_shadow;
   const D3DCheck check = D3DCheckShadow(D3DStat_SetSamplerState, s.samp_valid[sampler][type], s.samp_valid[sampler][type] && s.samp[sampler][type] == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, sampler, type, value);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.samp[sampler][type] = value;
      s.samp_valid[sampler][type] = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetScissorRect(void* self, const RECT* rect)
{
   D3DNote(D3DStat_SetScissorRect);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, const RECT*)>(g_d3d_stat_orig[75])(self, rect);
}

static HRESULT __stdcall D3DWrap_DrawPrimitive(void* self, UINT type, UINT start_vertex, UINT primitive_count)
{
   D3DClaimDrawThread();
   D3DNote(D3DStat_DrawPrimitive);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, UINT)>(g_d3d_stat_orig[81])(self, type, start_vertex, primitive_count);
}

static HRESULT __stdcall D3DWrap_DrawIndexedPrimitive(void* self, UINT type, INT base_vertex, UINT min_index, UINT num_vertices, UINT start_index, UINT primitive_count)
{
   D3DClaimDrawThread();
   D3DNote(D3DStat_DrawIndexedPrimitive);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, INT, UINT, UINT, UINT, UINT)>(g_d3d_stat_orig[82])(self, type, base_vertex, min_index, num_vertices, start_index, primitive_count);
}

static HRESULT __stdcall D3DWrap_DrawPrimitiveUP(void* self, UINT type, UINT primitive_count, const void* data, UINT stride)
{
   D3DClaimDrawThread();
   D3DNote(D3DStat_DrawPrimitiveUP);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, const void*, UINT)>(g_d3d_stat_orig[83])(self, type, primitive_count, data, stride);
}

static HRESULT __stdcall D3DWrap_DrawIndexedPrimitiveUP(void* self, UINT type, UINT min_vertex, UINT num_vertices, UINT primitive_count, const void* index_data, UINT index_format, const void* vertex_data, UINT stride)
{
   D3DClaimDrawThread();
   D3DNote(D3DStat_DrawIndexedPrimitiveUP);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, UINT, UINT, UINT, const void*, UINT, const void*, UINT)>(g_d3d_stat_orig[84])(self, type, min_vertex, num_vertices, primitive_count, index_data, index_format, vertex_data, stride);
}

static HRESULT __stdcall D3DWrap_SetVertexDeclaration(void* self, void* decl)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_d3d_stat_orig[87]);
   if (D3DNote(D3DStat_SetVertexDeclaration))
      return fn(self, decl);
   auto& s = g_d3d_stat_shadow;
   const uintptr_t value = reinterpret_cast<uintptr_t>(decl);
   const D3DCheck check = D3DCheckShadow(D3DStat_SetVertexDeclaration, s.decl_valid, s.decl_valid && s.decl == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, decl);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.decl = value;
      s.decl_valid = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetFVF(void* self, DWORD fvf)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, DWORD)>(g_d3d_stat_orig[89]);
   if (D3DNote(D3DStat_SetFVF))
      return fn(self, fvf);
   auto& s = g_d3d_stat_shadow;
   const D3DCheck check = D3DCheckShadow(D3DStat_SetFVF, s.fvf_valid, s.fvf_valid && s.fvf == fvf);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, fvf);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.fvf = fvf;
      s.fvf_valid = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetVertexShader(void* self, void* shader)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_d3d_stat_orig[92]);
   if (D3DNote(D3DStat_SetVertexShader))
      return fn(self, shader);
   auto& s = g_d3d_stat_shadow;
   const uintptr_t value = reinterpret_cast<uintptr_t>(shader);
   const D3DCheck check = D3DCheckShadow(D3DStat_SetVertexShader, s.vs_valid, s.vs_valid && s.vs_shader == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, shader);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.vs_shader = value;
      s.vs_valid = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetVertexShaderConstantF(void* self, UINT start, const float* data, UINT count4)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, const float*, UINT)>(g_d3d_stat_orig[94]);
   return D3DConstF(D3DStat_SetVertexShaderConstantF, g_d3d_stat_shadow.vs, start, data, count4,
      [&](UINT st, const float* d, UINT n) { return fn(self, st, d, n); });
}

static HRESULT __stdcall D3DWrap_SetVertexShaderConstantI(void* self, UINT start, const int* data, UINT count4)
{
   D3DNote(D3DStat_SetVertexShaderConstantI);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, const int*, UINT)>(g_d3d_stat_orig[96])(self, start, data, count4);
}

static HRESULT __stdcall D3DWrap_SetVertexShaderConstantB(void* self, UINT start, const BOOL* data, UINT count)
{
   D3DNote(D3DStat_SetVertexShaderConstantB);
   return reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, const BOOL*, UINT)>(g_d3d_stat_orig[98])(self, start, data, count);
}

static HRESULT __stdcall D3DWrap_SetStreamSource(void* self, UINT stream, void* vb, UINT offset, UINT stride)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, void*, UINT, UINT)>(g_d3d_stat_orig[100]);
   if (D3DNote(D3DStat_SetStreamSource) || stream >= 16)
      return fn(self, stream, vb, offset, stride);
   auto& s = g_d3d_stat_shadow;
   const uintptr_t value = reinterpret_cast<uintptr_t>(vb);
   const bool same = s.stream_valid[stream] && s.stream_vb[stream] == value && s.stream_off[stream] == offset && s.stream_stride[stream] == stride;
   const D3DCheck check = D3DCheckShadow(D3DStat_SetStreamSource, s.stream_valid[stream], same);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, stream, vb, offset, stride);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.stream_vb[stream] = value;
      s.stream_off[stream] = offset;
      s.stream_stride[stream] = stride;
      s.stream_valid[stream] = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetIndices(void* self, void* ib)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_d3d_stat_orig[104]);
   if (D3DNote(D3DStat_SetIndices))
      return fn(self, ib);
   auto& s = g_d3d_stat_shadow;
   const uintptr_t value = reinterpret_cast<uintptr_t>(ib);
   const D3DCheck check = D3DCheckShadow(D3DStat_SetIndices, s.indices_valid, s.indices_valid && s.indices == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, ib);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.indices = value;
      s.indices_valid = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetPixelShader(void* self, void* shader)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(g_d3d_stat_orig[107]);
   if (D3DNote(D3DStat_SetPixelShader))
      return fn(self, shader);
   auto& s = g_d3d_stat_shadow;
   const uintptr_t value = reinterpret_cast<uintptr_t>(shader);
   const D3DCheck check = D3DCheckShadow(D3DStat_SetPixelShader, s.ps_valid, s.ps_valid && s.ps_shader == value);
   if (check == D3DCheck::Drop)
      return S_OK;
   const HRESULT hr = fn(self, shader);
   if (check == D3DCheck::Send && SUCCEEDED(hr))
   {
      s.ps_shader = value;
      s.ps_valid = true;
   }
   return hr;
}

static HRESULT __stdcall D3DWrap_SetPixelShaderConstantF(void* self, UINT start, const float* data, UINT count4)
{
   auto fn = reinterpret_cast<HRESULT(__stdcall*)(void*, UINT, const float*, UINT)>(g_d3d_stat_orig[109]);
   return D3DConstF(D3DStat_SetPixelShaderConstantF, g_d3d_stat_shadow.ps, start, data, count4,
      [&](UINT st, const float* d, UINT n) { return fn(self, st, d, n); });
}

// Accepts the device vtable whose slot 64 (GetTexture) points at the dgVoodoo site patched by TryPatchDgVoodooGetTextureBounds().
static bool BLDgvGetTextureSlotAccept(void** vtable)
{
   const uintptr_t target = reinterpret_cast<uintptr_t>(vtable[64]);
   return target <= g_dgv_gettexture_site && g_dgv_gettexture_site < target + 0x400;
}

// Installs the filter and stats wrappers on top of the device vtable `vtable` (the command-stream thunks, see InstallD3D9Cs).
// Idempotent. A null vtable (locator failed) patches nothing.
static void InstallD3D9CallStats(void** vtable)
{
   if (g_d3d_stat_installed || g_d3d_stat_attempted)
      return;
   g_d3d_stat_attempted = true;
   if (vtable == nullptr)
      return;
   g_cs_sb_apply_cb = &D3DInvalidateAll;   // proxy state-block Apply invalidates like the hooked real-block Apply

   struct D3DStatHook
   {
      int slot;
      void* fn;
   };
   const D3DStatHook hooks[] = {
      { 16, reinterpret_cast<void*>(&D3DWrap_Reset) },
      { 17, reinterpret_cast<void*>(&D3DWrap_Present) },
      { 37, reinterpret_cast<void*>(&D3DWrap_SetRenderTarget) },
      { 39, reinterpret_cast<void*>(&D3DWrap_SetDepthStencilSurface) },
      { 43, reinterpret_cast<void*>(&D3DWrap_Clear) },
      { 47, reinterpret_cast<void*>(&D3DWrap_SetViewport) },
      { 57, reinterpret_cast<void*>(&D3DWrap_SetRenderState) },
      { 59, reinterpret_cast<void*>(&D3DWrap_CreateStateBlock) },
      { 60, reinterpret_cast<void*>(&D3DWrap_BeginStateBlock) },
      { 61, reinterpret_cast<void*>(&D3DWrap_EndStateBlock) },
      { 65, reinterpret_cast<void*>(&D3DWrap_SetTexture) },
      { 67, reinterpret_cast<void*>(&D3DWrap_SetTextureStageState) },
      { 69, reinterpret_cast<void*>(&D3DWrap_SetSamplerState) },
      { 75, reinterpret_cast<void*>(&D3DWrap_SetScissorRect) },
      { 81, reinterpret_cast<void*>(&D3DWrap_DrawPrimitive) },
      { 82, reinterpret_cast<void*>(&D3DWrap_DrawIndexedPrimitive) },
      { 83, reinterpret_cast<void*>(&D3DWrap_DrawPrimitiveUP) },
      { 84, reinterpret_cast<void*>(&D3DWrap_DrawIndexedPrimitiveUP) },
      { 87, reinterpret_cast<void*>(&D3DWrap_SetVertexDeclaration) },
      { 89, reinterpret_cast<void*>(&D3DWrap_SetFVF) },
      { 92, reinterpret_cast<void*>(&D3DWrap_SetVertexShader) },
      { 94, reinterpret_cast<void*>(&D3DWrap_SetVertexShaderConstantF) },
      { 96, reinterpret_cast<void*>(&D3DWrap_SetVertexShaderConstantI) },
      { 98, reinterpret_cast<void*>(&D3DWrap_SetVertexShaderConstantB) },
      { 100, reinterpret_cast<void*>(&D3DWrap_SetStreamSource) },
      { 104, reinterpret_cast<void*>(&D3DWrap_SetIndices) },
      { 107, reinterpret_cast<void*>(&D3DWrap_SetPixelShader) },
      { 109, reinterpret_cast<void*>(&D3DWrap_SetPixelShaderConstantF) },
   };

   int patched = 0;
   for (const D3DStatHook& h : hooks)
   {
      DWORD old_protection = 0;
      if (!VirtualProtect(&vtable[h.slot], sizeof(void*), PAGE_READWRITE, &old_protection))
         continue;
      g_d3d_stat_orig[h.slot] = vtable[h.slot];
      vtable[h.slot] = h.fn;
      DWORD ignored = 0;
      VirtualProtect(&vtable[h.slot], sizeof(void*), old_protection, &ignored);
      ++patched;
   }

   g_d3d_stat_installed = true;
   BLD3D_DEV(reshade::log::message(reshade::log::level::info, std::format("[BL D3D9] {} of {} vtable slots hooked (filter {})", patched, static_cast<int>(std::size(hooks)), g_d3d9_filter ? "on" : "off").c_str());)
}
