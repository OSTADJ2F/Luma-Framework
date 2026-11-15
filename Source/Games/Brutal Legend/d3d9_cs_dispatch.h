#pragma once

// Brutal Legend: CsDispatch (direct mode only) and the device vtable install glue for the D3D9 command stream.
// Thunks (d3d9_cs_hook.h) call CsDispatch(slot, args). Mode 0 calls the original entry, counts the call,
// and adopts returned interface pointers. Modes 1/2 run calls on the CsQueue worker (d3d9_cs_queue.h).

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>

#include "d3d9_cs_tables.h"
#include "d3d9_cs_hook.h"
#include "d3d9_cs_stats.h"
#include "d3d9_cs_queue.h"
#include "d3d9_cs_staging.h"
#include "d3d9_cs_shadow.h"
#include "d3d9_cs_watchdog.h"

// Thread that made the last device Present. 0 until the first Present (no owner yet).
inline std::atomic<DWORD> g_cs_owner_thread{0};

// Address range of the d3d9.dll image the device vtable was found in (set by InstallD3D9Cs). A thunk's caller
// inside this range is dgVoodoo calling itself; anything else is the game.
inline uintptr_t g_cs_d3d9_base = 0;
inline uintptr_t g_cs_d3d9_end = 0;

// Maps the out interface. CsI_Count (base texture) is resolved with IDirect3DResource9::GetType (vtable slot 10).
inline CsIface CsResolveOutIface(void* obj, CsIface declared)
{
   if (declared != CsI_Count)
      return declared;

   using GetTypeFn = DWORD(__stdcall*)(void*);
   void** vt = *reinterpret_cast<void***>(obj);
   if (vt == nullptr || vt[10] == nullptr)
      return CsI_Count;

   switch (reinterpret_cast<GetTypeFn>(vt[10])(obj))
   {
   case D3DRTYPE_SURFACE:       return CsI_Surface;
   case D3DRTYPE_VOLUME:        return CsI_Volume;
   case D3DRTYPE_TEXTURE:       return CsI_Texture;
   case D3DRTYPE_VOLUMETEXTURE: return CsI_VolumeTexture;
   case D3DRTYPE_CUBETEXTURE:   return CsI_CubeTexture;
   case D3DRTYPE_VERTEXBUFFER:  return CsI_VertexBuffer;
   case D3DRTYPE_INDEXBUFFER:   return CsI_IndexBuffer;
   default:                     return CsI_Count;
   }
}

// Threaded modes. Owner = thread of the last Present. Worker = the CsQueue thread.
inline std::atomic<int> g_cs_mode_request{0}, g_cs_max_frames{1};
inline std::atomic<int> g_cs_async_refs{1};          // dev d3d9.cs_async_refs: 1 = owner-thread Surface AddRef/Release async (return 1)
inline std::atomic<int> g_cs_coalesce{0};            // dev d3d9.cs_coalesce: 1 = merge SetVertex/PixelShaderConstantF into the unpublished tail command
inline std::atomic<int> g_cs_mode_active{0};         // applied mode: 0 direct, 1 threaded, 2 drain after every async call
inline void (*g_cs_owner_present_cb)() = nullptr;    // owner thread, inside Present, before enqueue, only when the active mode != 0
inline bool g_cs_device_hooked = false;              // set by InstallD3D9Cs: CsPresent counts every frame, so the stats wrapper must not

namespace cs_dispatch_detail {
inline constexpr uint32_t kCsRingBytes = 16u << 20;
inline constexpr uint32_t kCsMaxPayload = 256u << 10;  // larger async calls block instead (no heap payload yet)
inline constexpr uint32_t kCsMaxArgs = 12;             // CsInvoke supports 1..12 arguments
inline constexpr uint32_t kCsPublishBatch = 16;        // owner publishes after this many async commands (or at any blocking point)
inline volatile LONG g_frames_queued = 0;              // Present commands queued and not yet executed
inline volatile LONG g_device_lost = 0;                // sticky D3DERR_DEVICELOST; cleared by a successful Reset
inline volatile LONG g_mail_pending = 0;
inline SRWLOCK g_mail_lock = SRWLOCK_INIT;
struct CsMailItem { CsSlot* slot; DWORD* args; CsDone done; CsMailItem* next; DWORD (*fn)(void*); void* ctx; };  // fn set: run fn(ctx) instead of slot
inline CsMailItem* g_mail_head = nullptr;
inline CsMailItem* g_mail_tail = nullptr;
inline uint32_t g_frame_cmds = 0;                      // owner only: commands and bytes enqueued since the last Present
inline uint32_t g_frame_bytes = 0;
}

inline bool CsRunning() { return cs_queue_detail::g_cs_queue.running; }

inline uint64_t CsQpc()
{
   LARGE_INTEGER t;
   QueryPerformanceCounter(&t);
   return static_cast<uint64_t>(t.QuadPart);
}

inline uint32_t CsAlign4(uint32_t v) { return (v + 3u) & ~3u; }

inline bool CsIsName(const CsMethod* m, const char* name) { return std::strcmp(m->name, name) == 0; }

inline bool CsIsCreate(const CsMethod* m) { return std::strncmp(m->name, "Create", 6) == 0; }

// Vertex count for a D3DPRIMITIVETYPE and primitive count.
inline uint32_t CsPrimVertexCount(DWORD type, DWORD count)
{
   switch (type)
   {
   case D3DPT_POINTLIST:     return count;
   case D3DPT_LINELIST:      return count * 2u;
   case D3DPT_LINESTRIP:     return count + 1u;
   case D3DPT_TRIANGLELIST:  return count * 3u;
   case D3DPT_TRIANGLESTRIP: return count + 2u;
   case D3DPT_TRIANGLEFAN:   return count + 2u;
   default:                  return 0;
   }
}

// Bytes copied for one pointer argument of an async row. args are the caller's values.
inline uint32_t CsPtrBytes(const CsPtrArg& p, const DWORD* args)
{
   switch (p.kind)
   {
   case CsP_Fixed:     return p.fixed_bytes;
   case CsP_Vec4Count: return args[p.count_arg] * 16u;
   case CsP_BoolCount: return args[p.count_arg] * 4u;
   case CsP_RectCount: return args[p.count_arg] * 16u;
   case CsP_UPVerts:
   {
      const DWORD stride = args[p.arg + 1];
      if (p.count_arg == 0)
         return CsPrimVertexCount(args[1], args[2]) * stride;                // DrawPrimitiveUP
      return (args[p.count_arg - 1] + args[p.count_arg]) * stride;           // DrawIndexedPrimitiveUP: MinVertexIndex + NumVertices
   }
   case CsP_UPIndices:
      return CsPrimVertexCount(args[1], args[p.count_arg]) * (args[p.arg + 1] == D3DFMT_INDEX16 ? 2u : 4u);
   default:
      return 0;
   }
}

// Owner thread, mode != 0. Copies the call and its async pointer data into the ring.
// Layout: +8 CsSlot*, +12 argc, +16 args[argc] (copied pointers hold their offset from the command start, 0 = null), then the data.
// publish = commit now (Present). Otherwise the command is published with its batch.
// False = not queued (the caller blocks instead).
inline bool CsEnqueueCall(CsSlot* slot, DWORD* args, bool publish = false)
{
   using namespace cs_dispatch_detail;
   const CsMethod* m = slot->method;
   if (m->argc == 0 || m->argc > kCsMaxArgs)
      return false;

   // Set{Vertex,Pixel}ShaderConstantF(StartRegister, pData, Vector4fCount). Same-range calls overwrite the unpublished
   // tail's data; contiguous ranges append to it. Never across another command, never into a published one.
   const bool const_f = m->iface == CsI_Device && m->argc == 4 && (m->slot == 94 || m->slot == 109);
   if (const_f && g_cs_coalesce.load(std::memory_order_relaxed) != 0 && args[2] != 0 && args[3] != 0 &&
       args[3] <= kCsMaxPayload / 16u)
   {
      const uint32_t nbytes = args[3] * 16u;
      CsCmd* prev = CsQueueUnpublishedTail();
      if (prev != nullptr && prev->op == CsOp_Call)
      {
         uint8_t* pp = reinterpret_cast<uint8_t*>(prev);
         CsSlot* pslot = nullptr;
         DWORD pa[4] = {};
         std::memcpy(&pslot, pp + 8, sizeof(pslot));
         std::memcpy(pa, pp + 16, sizeof(pa));          // pa[1] = StartRegister, pa[2] = data offset, pa[3] = count
         const uint8_t* src = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(args[2]));
         if (pslot == slot && pa[2] != 0)
         {
            if (pa[1] == args[1] && pa[3] == args[3])
            {
               std::memcpy(pp + pa[2], src, nbytes);
               if (publish)
                  CsQueueCommit();
               return true;
            }
            if (pa[1] + pa[3] == args[1])
            {
               const uint32_t old_bytes = prev->bytes;
               if (CsQueueTryExtend(nbytes) == prev)
               {
                  std::memcpy(pp + old_bytes, src, nbytes);      // the data is the command's last bytes
                  const DWORD count = pa[3] + args[3];
                  std::memcpy(pp + 16 + 4 * 3, &count, sizeof(count));
                  if (publish)
                     CsQueueCommit();
                  return true;
               }
            }
         }
      }
   }

   uint32_t sizes[4] = {};
   uint32_t total = 16u + 4u * m->argc;
   for (int i = 0; i < 4; ++i)
   {
      const CsPtrArg& pa = m->ptr[i];
      if (pa.kind == CsP_None || args[pa.arg] == 0)
         continue;
      sizes[i] = CsAlign4(CsPtrBytes(pa, args));
      total += sizes[i];
   }
   if (total > kCsMaxPayload)
      return false;

   CsCmd* c = CsQueueReserve(total);
   if (!c)
      return false;

   uint8_t* p = reinterpret_cast<uint8_t*>(c);
   c->op = CsOp_Call;

   DWORD tmp[kCsMaxArgs] = {};
   for (uint32_t j = 0; j < m->argc; ++j)
      tmp[j] = args[j];

   uint32_t cursor = 16u + 4u * m->argc;
   for (int i = 0; i < 4; ++i)
   {
      const CsPtrArg& pa = m->ptr[i];
      if (pa.kind == CsP_None || args[pa.arg] == 0)
         continue;
      std::memcpy(p + cursor, reinterpret_cast<const void*>(static_cast<uintptr_t>(args[pa.arg])), CsPtrBytes(pa, args));
      tmp[pa.arg] = cursor;
      cursor += sizes[i];
   }

   std::memcpy(p + 8, &slot, sizeof(slot));
   const uint32_t argc = m->argc;
   std::memcpy(p + 12, &argc, sizeof(argc));
   std::memcpy(p + 16, tmp, 4u * m->argc);

   BLD3D_DEV(
      {
         const auto& q = cs_queue_detail::g_cs_queue;
         const LONG64 used = static_cast<LONG64>(q.reserved - q.tail.v.load(std::memory_order_relaxed));
         if (used > g_cs_ring_high)
            g_cs_ring_high = used;
         if (used * 4 > static_cast<LONG64>(q.ring_bytes) * 3)
            InterlockedIncrement64(&g_cs_ring_nearfull);
      }
   )

   g_frame_cmds += 1;
   g_frame_bytes += total;
   CsQueueAfterWrite(c, const_f, kCsPublishBatch);
   if (publish)
      CsQueueCommit();
   return true;
}

inline void* CsOutObject(const CsMethod* m, const DWORD* args)
{
   if (m->out_arg == 0)
      return nullptr;
   void** out = reinterpret_cast<void**>(static_cast<uintptr_t>(args[m->out_arg]));
   return out ? *out : nullptr;
}

// Stats, adoption of returned interfaces, and mode-dependent hooks after a call completed.
// owner = the call ran on the owner thread (surface fills are owner-only). Create/Release hooks run on any thread.
inline void CsFinishCall(CsSlot* slot, DWORD* args, DWORD hr, bool foreign, bool internal, bool owner)
{
   using namespace cs_dispatch_detail;
   const CsMethod* m = slot->method;
   CsStatCall(slot, args, hr, foreign, internal);
   const bool ok = static_cast<LONG>(hr) >= 0;

   void* obj = ok ? CsOutObject(m, args) : nullptr;
   if (obj != nullptr)
   {
      const CsIface iface = CsResolveOutIface(obj, m->out_iface);
      if (iface != CsI_Count)
         CsAdopt(obj, iface);
      CsShadowForgetDesc(obj);                                              // a returned interface may reuse a freed surface address
   }

   CsShadowFinish(slot, hr);
   if (g_cs_mode_active.load(std::memory_order_relaxed) == 0)
      return;

   if (ok && m->iface == CsI_Device && m->slot == 16)
      InterlockedExchange(&g_device_lost, 0);                               // successful Reset

   if (obj != nullptr && CsIsCreate(m))
   {
      CsShadowOnCreate(obj);
      if (m->iface == CsI_Device && (CsIsName(m, "CreateVertexBuffer") || CsIsName(m, "CreateIndexBuffer")))
         CsStagingOnCreate(obj, static_cast<UINT>(args[1]), static_cast<DWORD>(args[2]), static_cast<D3DPOOL>(args[4]));
   }

   if ((m->iface == CsI_VertexBuffer || m->iface == CsI_IndexBuffer) && CsIsName(m, "Release"))
      CsStagingOnRelease(reinterpret_cast<void*>(static_cast<uintptr_t>(args[0])), static_cast<ULONG>(hr));
   else if ((m->iface == CsI_Texture || m->iface == CsI_CubeTexture || m->iface == CsI_VolumeTexture) &&
            CsIsName(m, "Release") && hr == 0)
      CsShadowOnRelease(reinterpret_cast<void*>(static_cast<uintptr_t>(args[0])));

   if (owner && ok && (CsIsName(m, "GetSurfaceLevel") || CsIsName(m, "GetCubeMapSurface")))
      CsShadowFill(slot, args);
   if (owner)
      CsShadowFillDevice(slot, args, hr);
}

// Direct call on the calling thread (mode 0, worker thread).
inline DWORD CsDirectCall(CsSlot* slot, DWORD* args, bool foreign, bool internal)
{
   const DWORD hr = CsInvoke(slot->orig, args, slot->method->argc);
   CsFinishCall(slot, args, hr, foreign, internal, false);
   return hr;
}

// Owner thread, blocking row: the worker runs the call; the caller waits. Falls back to a direct call if the queue is not running.
inline DWORD CsBlockingCall(CsSlot* slot, DWORD* args)
{
   if (!CsRunning())
      return CsInvoke(slot->orig, args, slot->method->argc);

   CsDone done{};
   CsCmd* c = CsQueueReserve(24);
   if (!c)
      return CsInvoke(slot->orig, args, slot->method->argc);

   uint8_t* p = reinterpret_cast<uint8_t*>(c);
   CsDone* dp = &done;
   c->op = CsOp_CallBlocking;
   std::memcpy(p + 8, &slot, sizeof(slot));
   std::memcpy(p + 12, &dp, sizeof(dp));
   std::memcpy(p + 16, &args, sizeof(args));
   CsQueueCommit();

   const uint64_t t0 = CsQpc();
   CsWait(&done.done);
   CsStatWait(slot, CsQpc() - t0);
#if DEVELOPMENT
   CsNoteLongWait("call", nullptr, slot, t0);
#endif
   return done.result;
}

// Foreign thread (or no owner yet): blocking through the mailbox; the worker runs it between commands.
inline DWORD CsMailboxCall(CsSlot* slot, DWORD* args)
{
   using namespace cs_dispatch_detail;
   if (!CsRunning())
      return CsInvoke(slot->orig, args, slot->method->argc);

   CsMailItem it{};
   it.slot = slot;
   it.args = args;

   AcquireSRWLockExclusive(&g_mail_lock);
   if (g_mail_tail)
      g_mail_tail->next = &it;
   else
      g_mail_head = &it;
   g_mail_tail = &it;
   InterlockedIncrement(&g_mail_pending);
   ReleaseSRWLockExclusive(&g_mail_lock);
   CsQueueWake();                                                        // the worker polls the mailbox when it wakes

   const uint64_t t0 = CsQpc();
   CsWait(&it.done.done);
   CsStatWait(slot, CsQpc() - t0);
#if DEVELOPMENT
   CsNoteLongWait("mailbox", nullptr, slot, t0);
#endif
   return it.done.result;
}

// Foreign thread: runs fn(ctx) on the worker through the mailbox and waits. Direct call when the queue is not running.
inline DWORD CsMailboxRun(DWORD (*fn)(void*), void* ctx)
{
   using namespace cs_dispatch_detail;
   if (!CsRunning())
      return fn(ctx);

   CsMailItem it{};
   it.fn = fn;
   it.ctx = ctx;

   AcquireSRWLockExclusive(&g_mail_lock);
   if (g_mail_tail)
      g_mail_tail->next = &it;
   else
      g_mail_head = &it;
   g_mail_tail = &it;
   InterlockedIncrement(&g_mail_pending);
   ReleaseSRWLockExclusive(&g_mail_lock);
   CsQueueWake();

   CsWait(&it.done.done);
   return it.done.result;
}

// Worker: runs queued foreign calls. Called between commands and when idle.
inline void CsPollMailbox()
{
   using namespace cs_dispatch_detail;
   if (InterlockedCompareExchange(&g_mail_pending, 0, 0) == 0)
      return;

   AcquireSRWLockExclusive(&g_mail_lock);
   CsMailItem* it = g_mail_head;
   g_mail_head = nullptr;
   g_mail_tail = nullptr;
   ReleaseSRWLockExclusive(&g_mail_lock);

   while (it != nullptr)
   {
      CsMailItem* next = it->next;
#if DEVELOPMENT
      const uint64_t mstart = CsQpc();
      g_cs_worker_op = CsOp_Call;
      g_cs_worker_slot = it->slot;
      InterlockedExchange64(&g_cs_worker_cmd_start, static_cast<LONG64>(mstart));
#endif
      const DWORD hr = it->fn ? it->fn(it->ctx) : CsInvoke(it->slot->orig, it->args, it->slot->method->argc);
#if DEVELOPMENT
      const uint64_t mdur = CsQpc() - mstart;
      InterlockedExchange64(&g_cs_worker_cmd_start, 0);
      InterlockedExchangeAdd64(&g_cs_worker_busy_ticks, static_cast<LONG64>(mdur));
      if (it->slot)
         CsStatWorker(it->slot, mdur);
      if (g_cs_diag_cb && mdur > cs_queue_detail::CsTicksPerSec() / 10)
         g_cs_diag_cb(1, nullptr, nullptr, it->slot, mdur); // it->slot is read before CsSignalDone frees it
#endif
      InterlockedDecrement(&g_mail_pending);
      CsSignalDone(&it->done, hr);                                          // the caller may free `it` from here on
      it = next;
   }
}

// Worker: executes one command from the ring (CsQueue hands Call, CallBlocking and BufferUpload here).
inline void CsExecute(const CsCmd* c)
{
   using namespace cs_dispatch_detail;
   const uint8_t* p = reinterpret_cast<const uint8_t*>(c);
   switch (c->op)
   {
   case CsOp_Call:
   {
      CsSlot* slot = nullptr;
      uint32_t argc = 0;
      std::memcpy(&slot, p + 8, sizeof(slot));
      std::memcpy(&argc, p + 12, sizeof(argc));
      DWORD a[kCsMaxArgs] = {};
      std::memcpy(a, p + 16, 4u * argc);

      const CsMethod* m = slot->method;
      for (int i = 0; i < 4; ++i)
      {
         const CsPtrArg& pa = m->ptr[i];
         if (pa.kind != CsP_None && a[pa.arg] != 0)
            a[pa.arg] = static_cast<DWORD>(reinterpret_cast<uintptr_t>(p + a[pa.arg]));
      }

      BLD3D_DEV(g_cs_worker_slot = slot;)
      BLD3D_DEV(const uint64_t wt0 = CsQpc();)
      const DWORD hr = CsInvoke(slot->orig, a, m->argc);
      BLD3D_DEV(CsStatWorker(slot, CsQpc() - wt0);)
      CsStatCall(slot, a, hr, false, false);

      if (m->iface == CsI_Device && m->slot == 17)
      {
         if (hr == static_cast<DWORD>(D3DERR_DEVICELOST))
            InterlockedExchange(&g_device_lost, 1);
         InterlockedDecrement(&g_frames_queued);
         WakeByAddressAll((void*)&g_frames_queued);
      }
      break;
   }
   case CsOp_CallBlocking:
   {
      CsSlot* slot = nullptr;
      CsDone* done = nullptr;
      DWORD* args = nullptr;
      std::memcpy(&slot, p + 8, sizeof(slot));
      std::memcpy(&done, p + 12, sizeof(done));
      std::memcpy(&args, p + 16, sizeof(args));
      BLD3D_DEV(g_cs_worker_slot = slot;)
      BLD3D_DEV(const uint64_t wt0 = CsQpc();)
      const DWORD hr = CsInvoke(slot->orig, args, slot->method->argc);
      BLD3D_DEV(CsStatWorker(slot, CsQpc() - wt0);)
      CsSignalDone(done, hr);                                               // the caller's stack args are read until here
      break;
   }
   case CsOp_BufferUpload:
      CsStagingExecute(c);
      break;
   default:
      break;
   }
}

// Owner thread: waits while more Presents are queued than g_cs_max_frames (decision 8).
inline void CsWaitFramesQueued(CsSlot* present)
{
   using namespace cs_dispatch_detail;
   const LONG limit = std::max<LONG>(1, static_cast<LONG>(g_cs_max_frames.load(std::memory_order_relaxed)));
   LONG cur = InterlockedCompareExchange(&g_frames_queued, 0, 0);
   if (cur <= limit)
      return;

   const uint64_t t0 = CsQpc();
   const bool gui = IsGUIThread(FALSE) != FALSE;
   while (cur > limit)
   {
      LONG expect = cur;
      WaitOnAddress((void*)&g_frames_queued, &expect, sizeof(LONG), 1);
      if (gui)
      {
         MSG m;
         PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
      }
      cur = InterlockedCompareExchange(&g_frames_queued, 0, 0);
   }
   CsStatWait(present, CsQpc() - t0);
#if DEVELOPMENT
   CsNoteLongWait("wait", "present throttle", nullptr, t0);
#endif
}

#if DEVELOPMENT
// Method name for a call item, or "op N" for other worker items.
inline std::string CsDiagMethodName(const void* slot, LONG op)
{
   if (slot)
   {
      const CsSlot* s = static_cast<const CsSlot*>(slot);
      return std::format("{}::{}", CsIfaceName(s->method->iface), s->method->name);
   }
   return std::format("op {}", static_cast<int>(op));
}

// Sink for g_cs_diag_cb (queue dev diagnostics). Logs the [BL CS] long-wait and slow-command lines.
inline void CsDiagLog(int event, const char* kind, const char* what, const void* slot, uint64_t ticks)
{
   using namespace cs_dispatch_detail;
   using namespace cs_queue_detail;
   const double ms = static_cast<double>(ticks) * 1000.0 / static_cast<double>(CsTicksPerSec());

   if (event == 1)
   {
      CsStatLog(std::format("[BL CS] slow worker cmd {} {:.1f} ms", CsDiagMethodName(slot, g_cs_worker_op), ms));
      return;
   }

   const DWORD tid = GetCurrentThreadId();
   const DWORD owner = g_cs_owner_thread.load(std::memory_order_relaxed);
   const std::string who = CsOnWorkerThread() ? std::string("worker")
      : (owner == tid ? std::string("owner") : std::format("foreign tid {}", tid));

   std::string what_name = slot ? CsDiagMethodName(slot, 0) : std::string(what ? what : "?");

   std::string exec = "idle";
   double exec_ms = 0.0;
   const LONG64 start = InterlockedCompareExchange64(&g_cs_worker_cmd_start, 0, 0);
   if (start)
   {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      exec = CsDiagMethodName(g_cs_worker_slot, g_cs_worker_op);
      exec_ms = static_cast<double>(static_cast<LONG64>(now.QuadPart) - start) * 1000.0 / static_cast<double>(CsTicksPerSec());
   }

   CsStatLog(std::format("[BL CS] long wait {:.1f} ms: {} {} on {}; worker executing {} for {:.1f} ms; mailbox pending {}; IsGUIThread {}",
      ms, kind ? kind : "wait", what_name, who, exec, exec_ms,
      InterlockedCompareExchange(&g_mail_pending, 0, 0), IsGUIThread(FALSE) ? 1 : 0));
}
#endif

inline void CsAfterAsync()
{
   if (g_cs_mode_active.load(std::memory_order_relaxed) == 2)
      CsQueueDrain();
}

// Proxy state blocks (d3d9_cs_stateblock.h). Included here: it uses CsAfterAsync, CsMailboxRun and the owner/mode globals.
#include "d3d9_cs_stateblock.h"

// Owner thread, mode != 0: hooks that may answer a call without the worker. True = answered with hr.
inline bool CsOwnerPreHook(CsSlot* slot, DWORD* args, DWORD& hr)
{
   const CsMethod* m = slot->method;
   if (m->iface == CsI_Device && CsIsName(m, "CreateStateBlock"))
      return CsStateBlockCreate(slot, args, hr);
   if (m->iface == CsI_VertexBuffer || m->iface == CsI_IndexBuffer)
   {
      if (CsIsName(m, "Lock"))
         return CsStagingLock(slot, args, hr);
      if (CsIsName(m, "Unlock"))
         return CsStagingUnlock(slot, args, hr);
   }
   if (CsIsName(m, "GetSurfaceLevel") || CsIsName(m, "GetCubeMapSurface"))
      return CsShadowGetSurface(slot, args, hr);
   return CsShadowGetDevice(slot, args, hr);
}

// Owner thread, inside Present (decision 9): drain, drop staging and shadows, start the queue on the first switch to non-zero.
inline void CsApplyModeRequest()
{
   using namespace cs_dispatch_detail;
   int req = g_cs_mode_request.load(std::memory_order_relaxed);
   if (req < 0 || req > 2)
      req = 0;
   const int active = g_cs_mode_active.load(std::memory_order_relaxed);
   if (req == active)
      return;

   if (CsRunning())
      CsQueueDrain();
   CsStagingDropAll();
   CsShadowDropAll();
   if (req != 0 && !CsRunning() && !CsQueueStart(kCsRingBytes, CsExecute, CsPollMailbox))
   {
      CsHookLog(2, "[BL CS] worker thread failed to start; staying in direct mode");
      req = 0;
   }
   if (req != 0 && CsRunning())
      CsWatchdogStart(&g_cs_owner_thread, cs_queue_detail::g_cs_queue.tid);
   g_cs_mode_active.store(req, std::memory_order_relaxed);
   CsHookLog(0, req ? "[BL CS] threaded mode on" : "[BL CS] threaded mode off");
}

// Device Present (decision 8). Also defines the owner thread and applies the mode request.
inline DWORD CsPresent(CsSlot* slot, DWORD* args, DWORD tid, bool foreign, bool internal)
{
   using namespace cs_dispatch_detail;
   if (foreign && CsRunning())
      CsQueueDrain();                                                       // a Present from another thread takes ownership
   foreign = false;
   g_cs_owner_thread.store(tid, std::memory_order_seq_cst);
   CsWatchdogNotePresent();
   CsApplyModeRequest();
   CsSbFlushPending();                                                   // deferred proxy releases, before the Present is queued

   if (g_cs_mode_active.load(std::memory_order_relaxed) == 0)
   {
      const DWORD hr = CsDirectCall(slot, args, false, internal);
      CsStatFrame(0, 0);
      return hr;
   }

   CsShadowFrame();
   if (InterlockedCompareExchange(&g_device_lost, 0, 0) != 0)
      return static_cast<DWORD>(D3DERR_DEVICELOST);

   if (g_cs_owner_present_cb)
      g_cs_owner_present_cb();

   // args[4] = pDirtyRegion: non-null blocks.
   if (args[4] == 0)
   {
      InterlockedIncrement(&g_frames_queued);
      if (CsEnqueueCall(slot, args, true))
      {
         CsAfterAsync();
         CsStatFrame(g_frame_cmds, g_frame_bytes);
         g_frame_cmds = 0;
         g_frame_bytes = 0;
         CsWaitFramesQueued(slot);
         return D3D_OK;
      }
      InterlockedDecrement(&g_frames_queued);
   }

   const DWORD hr = CsBlockingCall(slot, args);
   if (hr == static_cast<DWORD>(D3DERR_DEVICELOST))
      InterlockedExchange(&g_device_lost, 1);
   CsFinishCall(slot, args, hr, false, internal, true);
   CsStatFrame(g_frame_cmds, g_frame_bytes);
   g_frame_cmds = 0;
   g_frame_bytes = 0;
   return hr;
}

// Stops the worker (owner thread, never from DllMain). Drops staging and shadows first.
inline void CsShutdown()
{
   using namespace cs_dispatch_detail;
   CsWatchdogStop();
   if (CsRunning())
      CsQueueDrain();
   CsStagingDropAll();
   CsShadowDropAll();
   CsQueueStop();
   g_cs_mode_active.store(0, std::memory_order_relaxed);
   g_cs_mode_request.store(0, std::memory_order_relaxed);
}

inline bool CsThreadingActive() { return g_cs_mode_active.load(std::memory_order_relaxed) != 0; }

// Target of every hooked device/interface thunk. args[0] is `this`; args[i] is the i-th stack dword.
extern "C" DWORD __cdecl CsDispatch(CsSlot* slot, DWORD* args)
{
   using namespace cs_dispatch_detail;
   const CsMethod* m = slot->method;
   const DWORD tid = GetCurrentThreadId();
   const DWORD owner = g_cs_owner_thread.load(std::memory_order_relaxed);
   const bool foreign = owner != 0 && owner != tid;

   // Owner: a mailbox post from another thread cannot commit the owner's pending batch, so the owner does it on its next call.
   if (!foreign && owner == tid && g_mail_pending != 0 && CsRunning())
      CsQueueCommit();

   // args[-1] is the caller's return address: the thunk entered with esp = args - 4 (see CsEmitThunk).
   const uintptr_t caller = static_cast<uintptr_t>(args[-1]);
   const bool internal = caller >= g_cs_d3d9_base && caller < g_cs_d3d9_end;
   const bool present = m->iface == CsI_Device && m->slot == 17;

   // Worker thread (dgVoodoo calling its own vtable) goes straight to the original (decision 4).
   if (CsOnWorkerThread())
      return CsDirectCall(slot, args, foreign, internal);
   if (present)
      return CsPresent(slot, args, tid, foreign, internal);
   CsShadowPreCall(slot, args, owner == tid, g_cs_mode_active.load(std::memory_order_relaxed) != 0);
   if (g_cs_mode_active.load(std::memory_order_relaxed) == 0)
      return CsDirectCall(slot, args, foreign, internal);

   // Foreign thread, or no owner yet: blocking through the mailbox (decision 5).
   if (owner == 0 || foreign)
   {
      const DWORD hr = CsMailboxCall(slot, args);
      CsFinishCall(slot, args, hr, foreign, internal, false);
      return hr;
   }

   // Owner thread, mode != 0.
   DWORD pre_hr = 0;
   if (CsOwnerPreHook(slot, args, pre_hr))
      return pre_hr;

   const bool surface_ref = m->iface == CsI_Surface && (CsIsName(m, "AddRef") || CsIsName(m, "Release"));
   if (surface_ref && g_cs_async_refs.load(std::memory_order_relaxed) != 0 && CsEnqueueCall(slot, args))
   {
      CsAfterAsync();
      return 1;
   }

   if (m->mode == CsM_Async && CsEnqueueCall(slot, args))
   {
      CsAfterAsync();
      return D3D_OK;
   }

   if (m->iface == CsI_Device && CsIsName(m, "Reset"))
   {
      CsQueueDrain();
      CsStagingDropAll();
      CsShadowDropAll();
   }

   const DWORD hr = CsBlockingCall(slot, args);
   CsFinishCall(slot, args, hr, false, internal, true);
   return hr;
}

#if DEVELOPMENT
// Count-only thunks on device vtable slots 119..144: count, record last caller/thread, then `jmp orig`. Dev measurement only.
inline constexpr size_t kCsCountOnlyFirst = 119;
inline constexpr size_t kCsCountOnlyCount = 26;
inline constexpr size_t kCsCountOnlyStride = 64;
inline volatile LONG g_cs_count_only[kCsCountOnlyCount] = {};
inline volatile DWORD g_cs_last_ret[kCsCountOnlyCount] = {};
inline volatile DWORD g_cs_last_tid[kCsCountOnlyCount] = {};

// Installs count-only thunks on the device vtable entries that point into d3d9.dll, stopping at the first one that does not.
inline void CsInstallCountOnly(HMODULE d3d9, void** vtable)
{
   const uintptr_t base = reinterpret_cast<uintptr_t>(d3d9);
   const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(d3d9);
   const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
   const uintptr_t image_end = base + nt->OptionalHeader.SizeOfImage;

   uint8_t* page = nullptr;
   size_t installed = 0;
   for (size_t i = 0; i < kCsCountOnlyCount; ++i)
   {
      const size_t slot = kCsCountOnlyFirst + i;
      const uintptr_t orig = reinterpret_cast<uintptr_t>(vtable[slot]);
      if (orig < base || orig >= image_end)
         break;

      if (page == nullptr)
      {
         page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
         if (page == nullptr)
            return;
      }

      // Stub layout (64-byte stride, 33 bytes used):
      //   50                      push eax
      //   8B 44 24 04             mov eax,[esp+4]        (return address, after the push)
      //   A3 <&last_ret>          mov [last_ret],eax
      //   64 A1 24 00 00 00       mov eax,fs:[0x24]      (thread id)
      //   A3 <&last_tid>          mov [last_tid],eax
      //   58                      pop eax
      //   FF 05 <&counter>        inc dword [counter]
      //   E9 <rel32 orig>
      uint8_t* stub = page + i * kCsCountOnlyStride;
      uint8_t* p = stub;
      const uint32_t counter = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_cs_count_only[i]));
      const uint32_t ret_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_cs_last_ret[i]));
      const uint32_t tid_addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_cs_last_tid[i]));
      const uint32_t fs_tid_off = 0x24;
      const uint8_t head[] = { 0x50, 0x8B, 0x44, 0x24, 0x04, 0xA3 };
      std::memcpy(p, head, sizeof(head)); p += sizeof(head);
      std::memcpy(p, &ret_addr, 4); p += 4;
      *p++ = 0x64; *p++ = 0xA1;
      std::memcpy(p, &fs_tid_off, 4); p += 4;
      *p++ = 0xA3;
      std::memcpy(p, &tid_addr, 4); p += 4;
      *p++ = 0x58;
      *p++ = 0xFF; *p++ = 0x05;
      std::memcpy(p, &counter, 4); p += 4;
      *p++ = 0xE9;
      const uint32_t rel = static_cast<uint32_t>(orig - (reinterpret_cast<uintptr_t>(p) + 4));
      std::memcpy(p, &rel, 4); p += 4;
      static_assert(33 <= kCsCountOnlyStride, "count-only thunk must fit its stride");

      if (CsWriteVtableSlot(&vtable[slot], stub))
         ++installed;
   }

   CsStatLog(std::format("[BL CS] count-only thunks on device slots {}..{}: {} installed", kCsCountOnlyFirst, kCsCountOnlyFirst + kCsCountOnlyCount - 1, installed));
}

// "module+0xOFFSET (file name)" for a return address, or "unknown".
inline std::string CsDescribeCaller(uintptr_t ret)
{
   HMODULE mod = nullptr;
   if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(ret), &mod) || mod == nullptr)
      return "unknown";

   wchar_t path[MAX_PATH] = {};
   GetModuleFileNameW(mod, path, MAX_PATH);
   const wchar_t* name = wcsrchr(path, L'\\');
   name = name ? name + 1 : path;

   std::string out;
   for (const wchar_t* c = name; *c != L'\0'; ++c)
      out.push_back(static_cast<char>(*c));
   return std::format("{}+0x{:X}", out, static_cast<uintptr_t>(ret - reinterpret_cast<uintptr_t>(mod)));
}

// Logs nonzero count-only counters for one window of `frames` Presents, then resets them.
inline void CsCountOnlyLogWindow(uint32_t frames)
{
   const DWORD owner = g_cs_owner_thread.load(std::memory_order_relaxed);
   for (size_t i = 0; i < kCsCountOnlyCount; ++i)
   {
      const LONG n = InterlockedExchange(&g_cs_count_only[i], 0);
      if (n != 0)
      {
         const DWORD ret = g_cs_last_ret[i];
         const DWORD tid = g_cs_last_tid[i];
         CsStatLog(std::format("[BL CS] count-only slot {}: {:.2f}/frame, last caller {} tid {} ({} thread)",
            kCsCountOnlyFirst + i, static_cast<double>(n) / frames, CsDescribeCaller(ret), tid,
            tid == owner ? "render" : "other"));
      }
   }
}
#else
inline void CsInstallCountOnly(HMODULE, void**) {}
inline void CsCountOnlyLogWindow(uint32_t) {}
#endif

// Locates the device vtable with `accept` (see CsFindDeviceVtable: one match or nothing), hooks it, and returns it.
// Returns null when the locator fails; nothing is patched in that case (the locator logs the warning).
inline void** InstallD3D9Cs(HMODULE d3d9, bool (*accept)(void**))
{
   if (d3d9 == nullptr)
      return nullptr;
   {
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(d3d9);
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<uintptr_t>(d3d9) + dos->e_lfanew);
      g_cs_d3d9_base = reinterpret_cast<uintptr_t>(d3d9);
      g_cs_d3d9_end = g_cs_d3d9_base + nt->OptionalHeader.SizeOfImage;
   }

   void** vtable = CsFindDeviceVtable(d3d9, accept);
   if (vtable == nullptr)
      return nullptr;

   g_cs_device_hooked = CsHookVtable(vtable, CsI_Device);
   if (!g_cs_device_hooked)
      CsHookLog(2, "[BL CS] device vtable hook failed; the stats filter still installs on the unhooked vtable");
   BLD3D_DEV(g_cs_diag_cb = CsDiagLog;)

   BLD3D_DEV(CsInstallCountOnly(d3d9, vtable);)
   return vtable;
}
