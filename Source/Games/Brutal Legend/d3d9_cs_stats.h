#pragma once

// Brutal Legend: counters and [BL CS] log lines for the D3D9 command stream (DEVELOPMENT only; every body is empty otherwise).
// Core file: no game addresses. The interface and slot tables come from d3d9_cs_tables.h.

#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdint>
#include <format>
#include <new>
#include <string>
#include <vector>

#include "d3d9_cs_tables.h"
#include "d3d9_cs_queue.h"

// Interface name for log lines (matches the CsI_ enum without the prefix).
inline const char* CsIfaceName(CsIface iface)
{
   static const char* const kNames[CsI_Count] = {
      "Device", "VertexBuffer", "IndexBuffer", "Texture", "CubeTexture", "VolumeTexture",
      "Surface", "Volume", "SwapChain", "VertexShader", "PixelShader", "VertexDecl", "StateBlock", "Query",
   };
   return static_cast<size_t>(iface) < CsI_Count ? kNames[iface] : "?";
}

// Per-slot counters. Allocated lazily by CsStatGet (DEVELOPMENT only; the struct itself is always defined).
struct CsSlotStat
{
   const CsSlot* slot;
   volatile LONG64 calls;
   volatile LONG64 failures;                  // hr < 0, or hr != D3D_OK for CsM_Async slots
   volatile LONG64 foreign;                   // calls made from a non-owner thread
   volatile LONG64 internal;                  // calls whose return address is inside the d3d9.dll image (made by dgVoodoo itself)
   volatile LONG64 lock_buckets[2][4];        // game calls only: [0 = owner, 1 = foreign][0 DISCARD, 1 NOOVERWRITE, 2 READONLY, 3 other]
   volatile LONG64 waits;                     // blocking waits on this slot
   volatile LONG64 wait_ticks;                // QPC ticks spent waiting
   volatile LONG64 worker_calls;              // items this slot ran on the worker (dev)
   volatile LONG64 worker_ticks;              // QPC ticks the worker spent executing them (dev)
};

#if DEVELOPMENT

static const uint32_t kCsStatMax = 512;

// Registry of every allocated CsSlotStat, so the log window can walk them. inline = one definition across TUs.
inline CsSlotStat* volatile g_cs_stat_list[kCsStatMax];
inline volatile LONG g_cs_stat_count = 0;

// Frame accumulators since the last log window.
inline volatile LONG64 g_cs_frames = 0;
inline volatile LONG64 g_cs_cmds = 0;
inline volatile LONG64 g_cs_bytes = 0;
inline volatile LONG64 g_cs_last_qpc = 0;      // QPC at the previous CsStatFrame call (0 = none yet)
inline volatile LONG64 g_cs_frame_ticks = 0;   // sum of QPC deltas between CsStatFrame calls
inline volatile LONG64 g_cs_frame_deltas = 0;  // number of those deltas
inline volatile LONG64 g_cs_ring_high = 0;     // max bytes in use in the CS ring since the last log window
inline volatile LONG64 g_cs_ring_nearfull = 0; // enqueues that found the ring more than 75% full

inline void CsStatLog(const std::string& msg)
{
   if (g_cs_log)
      g_cs_log(0, msg.c_str()); // level 0 = info
}

// Returns the slot's counters, allocating them on first use. Safe from any thread.
inline CsSlotStat* CsStatGet(const CsSlot* s)
{
   if (CsSlotStat* st = s->stat)
      return st;

   CsSlotStat* fresh = new (std::nothrow) CsSlotStat();
   if (!fresh)
      return nullptr;
   fresh->slot = s;

   CsSlotStat* prev = static_cast<CsSlotStat*>(InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(&s->stat), fresh, nullptr));
   if (prev)
   {
      delete fresh; // another thread won the race
      return prev;
   }

   const LONG idx = InterlockedIncrement(&g_cs_stat_count) - 1;
   if (static_cast<uint32_t>(idx) < kCsStatMax)
      InterlockedExchangePointer(reinterpret_cast<void**>(const_cast<CsSlotStat**>(&g_cs_stat_list[idx])), fresh);
   return fresh;
}

#endif

// Counts one call on a slot. hr is the raw result; foreign is true when the calling thread is not the owner;
// internal is true when the caller's return address is inside d3d9.dll (dgVoodoo calling itself, not the game).
// Lock buckets count game calls only.
inline void CsStatCall(const CsSlot* s, const DWORD* args, DWORD hr, bool foreign, bool internal)
{
#if DEVELOPMENT
   CsSlotStat* st = CsStatGet(s);
   if (!st)
      return;

   const CsMethod* m = s->method;
   InterlockedIncrement64(&st->calls);

   const bool failed = (static_cast<LONG>(hr) < 0) || (m->mode == CsM_Async && hr != D3D_OK);
   if (failed)
      InterlockedIncrement64(&st->failures);
   if (foreign)
      InterlockedIncrement64(&st->foreign);
   if (internal)
      InterlockedIncrement64(&st->internal);

   if (m->lock_flags_arg && args && !internal)
   {
      const DWORD flags = args[m->lock_flags_arg];
      const int bucket = (flags & D3DLOCK_DISCARD) ? 0 : (flags & D3DLOCK_NOOVERWRITE) ? 1 : (flags & D3DLOCK_READONLY) ? 2 : 3;
      InterlockedIncrement64(&st->lock_buckets[foreign ? 1 : 0][bucket]);
   }
#else
   (void)s;
   (void)args;
   (void)hr;
   (void)foreign;
   (void)internal;
#endif
}

// Records one blocking wait on a slot and the QPC ticks it took.
inline void CsStatWait(const CsSlot* s, uint64_t qpc_ticks)
{
#if DEVELOPMENT
   CsSlotStat* st = CsStatGet(s);
   if (!st)
      return;

   InterlockedIncrement64(&st->waits);
   InterlockedExchangeAdd64(&st->wait_ticks, static_cast<LONG64>(qpc_ticks));
#else
   (void)s;
   (void)qpc_ticks;
#endif
}

// Records one item executed on the worker for a slot (any worker thread).
inline void CsStatWorker(const CsSlot* s, uint64_t qpc_ticks)
{
#if DEVELOPMENT
   CsSlotStat* st = CsStatGet(s);
   if (!st)
      return;

   InterlockedIncrement64(&st->worker_calls);
   InterlockedExchangeAdd64(&st->worker_ticks, static_cast<LONG64>(qpc_ticks));
#else
   (void)s;
   (void)qpc_ticks;
#endif
}

// Called once per Present. Frame time is the QPC delta between consecutive calls.
inline void CsStatFrame(uint32_t cmds, uint32_t bytes)
{
#if DEVELOPMENT
   LARGE_INTEGER now;
   QueryPerformanceCounter(&now);
   const LONG64 prev = InterlockedExchange64(&g_cs_last_qpc, static_cast<LONG64>(now.QuadPart));
   if (prev)
   {
      const LONG64 delta = static_cast<LONG64>(now.QuadPart) - prev;
      InterlockedExchangeAdd64(&g_cs_frame_ticks, delta);
      InterlockedIncrement64(&g_cs_frame_deltas);

      LARGE_INTEGER freq;
      QueryPerformanceFrequency(&freq);
      if (delta > freq.QuadPart / 4) // 250 ms
         CsStatLog(std::format("[BL CS] frame gap {:.1f} ms", static_cast<double>(delta) * 1000.0 / static_cast<double>(freq.QuadPart)));
   }
   InterlockedIncrement64(&g_cs_frames);
   InterlockedExchangeAdd64(&g_cs_cmds, static_cast<LONG64>(cmds));
   InterlockedExchangeAdd64(&g_cs_bytes, static_cast<LONG64>(bytes));
#else
   (void)cmds;
   (void)bytes;
#endif
}

// Logs the window totals and per-slot lines (slots with calls or waits, sorted by calls per frame), then resets everything.
inline void CsStatLogWindow()
{
#if DEVELOPMENT
   const LONG64 frames_raw = InterlockedExchange64(&g_cs_frames, 0);
   const LONG64 cmds = InterlockedExchange64(&g_cs_cmds, 0);
   const LONG64 bytes = InterlockedExchange64(&g_cs_bytes, 0);
   const LONG64 frame_ticks = InterlockedExchange64(&g_cs_frame_ticks, 0);
   const LONG64 frame_deltas = InterlockedExchange64(&g_cs_frame_deltas, 0);
   const LONG64 busy_ticks = InterlockedExchange64(&g_cs_worker_busy_ticks, 0);
   const LONG64 spin_ticks = InterlockedExchange64(&g_cs_worker_spin_ticks, 0);
   const LONG64 sleep_ticks = InterlockedExchange64(&g_cs_worker_sleep_ticks, 0);
   InterlockedExchange64(&g_cs_last_qpc, 0);

   LARGE_INTEGER freq;
   QueryPerformanceFrequency(&freq);
   const double ticks_to_ms = 1000.0 / static_cast<double>(freq.QuadPart);
   const double frames = static_cast<double>(std::max<LONG64>(frames_raw, 1));

   struct Row
   {
      const CsSlot* slot;
      LONG64 calls, failures, foreign, internal, waits, wait_ticks;
      LONG64 worker_calls, worker_ticks;
      LONG64 lock[2][4];
   };

   std::vector<Row> rows;
   const LONG allocated = g_cs_stat_count;
   const LONG count = std::min<LONG>(allocated, static_cast<LONG>(kCsStatMax));
   rows.reserve(static_cast<size_t>(count));
   LONG64 total_wait_ticks = 0;
   LONG64 total_waits = 0;

   for (LONG i = 0; i < count; ++i)
   {
      CsSlotStat* st = g_cs_stat_list[i];
      if (!st)
         continue;

      Row r{};
      r.slot = st->slot;
      r.calls = InterlockedExchange64(&st->calls, 0);
      r.failures = InterlockedExchange64(&st->failures, 0);
      r.foreign = InterlockedExchange64(&st->foreign, 0);
      r.internal = InterlockedExchange64(&st->internal, 0);
      r.waits = InterlockedExchange64(&st->waits, 0);
      r.wait_ticks = InterlockedExchange64(&st->wait_ticks, 0);
      r.worker_calls = InterlockedExchange64(&st->worker_calls, 0);
      r.worker_ticks = InterlockedExchange64(&st->worker_ticks, 0);
      for (int f = 0; f < 2; ++f)
         for (int b = 0; b < 4; ++b)
            r.lock[f][b] = InterlockedExchange64(&st->lock_buckets[f][b], 0);

      total_waits += r.waits;
      total_wait_ticks += r.wait_ticks;
      if (r.calls > 0 || r.waits > 0 || r.worker_calls > 0)
         rows.push_back(r);
   }

   // Sort by game calls (total minus dgVoodoo-internal calls).
   std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return (a.calls - a.internal) > (b.calls - b.internal); });

   const double avg_frame_ms = frame_deltas ? static_cast<double>(frame_ticks) * ticks_to_ms / static_cast<double>(frame_deltas) : 0.0;
   const double fps = avg_frame_ms > 0.0 ? 1000.0 / avg_frame_ms : 0.0;
   const double busy_pct = frame_ticks > 0 ? 100.0 * static_cast<double>(busy_ticks) / static_cast<double>(frame_ticks) : 0.0;
   CsStatLog(std::format("[BL CS] over {} frames: frame {:.2f} ms ({:.1f} fps), cmds/frame {:.1f}, bytes/frame {:.0f}, waits/frame {:.2f}, blocked {:.3f} ms/frame, worker busy {:.1f}%",
      frames_raw, avg_frame_ms, fps, static_cast<double>(cmds) / frames, static_cast<double>(bytes) / frames,
      static_cast<double>(total_waits) / frames, static_cast<double>(total_wait_ticks) * ticks_to_ms / frames, busy_pct));

   // Worker time per frame, split into items (commands), spinning and sleeping. The rest of the window is the worker's
   // remaining time (dispatch overhead, and any time the worker was not in one of the three states).
   {
      const double busy_ms = static_cast<double>(busy_ticks) * ticks_to_ms / frames;
      const double spin_ms = static_cast<double>(spin_ticks) * ticks_to_ms / frames;
      const double sleep_ms = static_cast<double>(sleep_ticks) * ticks_to_ms / frames;
      const double window_ms = static_cast<double>(frame_ticks) * ticks_to_ms / frames;
      const double other_ms = (window_ms - busy_ms - spin_ms - sleep_ms) > 0.0 ? (window_ms - busy_ms - spin_ms - sleep_ms) : 0.0;
      CsStatLog(std::format("[BL CS] worker ms/frame: in commands {:.3f}, spinning {:.3f} (incl. mailbox), asleep {:.3f}, other {:.3f} of {:.3f} window",
         busy_ms, spin_ms, sleep_ms, other_ms, window_ms));
   }

   // Top 10 slots by worker execution time.
   {
      std::vector<const Row*> by_worker;
      for (const Row& r : rows)
         if (r.worker_ticks > 0)
            by_worker.push_back(&r);
      std::sort(by_worker.begin(), by_worker.end(), [](const Row* a, const Row* b) { return a->worker_ticks > b->worker_ticks; });
      const size_t top = std::min<size_t>(by_worker.size(), 10);
      for (size_t i = 0; i < top; ++i)
      {
         const Row& r = *by_worker[i];
         CsStatLog(std::format("[BL CS] worker {}::{} {:.3f}/frame ms ({:.2f}/frame)",
            CsIfaceName(r.slot->method->iface), r.slot->method->name,
            static_cast<double>(r.worker_ticks) * ticks_to_ms / frames, static_cast<double>(r.worker_calls) / frames));
      }
   }

   {
      const LONG64 ring_high = InterlockedExchange64(&g_cs_ring_high, 0);
      const LONG64 nearfull = InterlockedExchange64(&g_cs_ring_nearfull, 0);
      CsStatLog(std::format("[BL CS] ring high-water {:.0f} KB, near-full enqueues {:.2f}/frame, blocked {:.1f}% of frame time",
         static_cast<double>(ring_high) / 1024.0, static_cast<double>(nearfull) / frames,
         avg_frame_ms > 0.0 ? 100.0 * static_cast<double>(total_wait_ticks) * ticks_to_ms / frames / avg_frame_ms : 0.0));
   }

   for (const Row& r : rows)
   {
      const LONG64 game_calls = r.calls - r.internal;
      std::string line = std::format("[BL CS] {}::{}: {:.1f}/frame game, {:.2f}/frame internal, foreign {:.2f}/frame",
         CsIfaceName(r.slot->method->iface), r.slot->method->name,
         static_cast<double>(game_calls) / frames, static_cast<double>(r.internal) / frames,
         static_cast<double>(r.foreign) / frames);

      if (r.failures)
         line += std::format(", failed {:.2f}/frame", static_cast<double>(r.failures) / frames);

      // Lock buckets hold game calls only (see CsStatCall).
      const LONG64 lock_total = r.lock[0][0] + r.lock[0][1] + r.lock[0][2] + r.lock[0][3] + r.lock[1][0] + r.lock[1][1] + r.lock[1][2] + r.lock[1][3];
      if (lock_total)
      {
         line += std::format(", lock D/N/R/- owner {:.1f}/{:.1f}/{:.1f}/{:.1f} foreign {:.1f}/{:.1f}/{:.1f}/{:.1f} per frame",
            r.lock[0][0] / frames, r.lock[0][1] / frames, r.lock[0][2] / frames, r.lock[0][3] / frames,
            r.lock[1][0] / frames, r.lock[1][1] / frames, r.lock[1][2] / frames, r.lock[1][3] / frames);
      }
      if (r.waits)
      {
         line += std::format(", waits {:.2f}/frame, {:.3f} ms/frame blocked", static_cast<double>(r.waits) / frames,
            static_cast<double>(r.wait_ticks) * ticks_to_ms / frames);
      }
      CsStatLog(line);
   }
#else
   // Empty outside DEVELOPMENT.
#endif
}
