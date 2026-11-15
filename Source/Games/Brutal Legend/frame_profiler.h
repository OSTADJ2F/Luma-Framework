#pragma once

// Dev-only per-frame profiler for Brutal Legend. Everything is compiled only in DEVELOPMENT builds.
// IAT-hooks the wait/sleep/IO imports of BrutalLegend.exe's KERNEL32 import table (no other module is touched) and
// accumulates blocked time per thread. Thread roles: "main" = the thread calling the sim AdvanceAccum hook (sim_tick.h),
// "render" = the thread calling OnPresent.
// Toggle: dev bool "frame.profile" (default off). CSV: Luma_BL_Frame.csv next to the exe while on, one row per present,
// flushed every 60 rows. Log prefix: "[BL Frame]".
// Extended (same toggle): event counts (draws, dispatches, binds, descriptor binds, RT binds, clears), maps split by
// buffer kind and texture, map bytes, render/main/process CPU ms, sim ticks and dt, frame-pacing deltas. Logs:
// "[BL Prof] session" on enable, "[BL Spike]" when present dt > 1.6x the 240-frame median, "[BL Summary]" every 1200
// presents, "[BL Prof] slow create" for creates over 2 ms.

#include <windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <type_traits>
#include <vector>
#include <algorithm>
#include <intrin.h>
#include <mutex>
#include <string>

#if DEVELOPMENT

static bool g_frame_profile = false; // dev toggle "frame.profile"

namespace BLFrameProfiler
{
   enum Group : int
   {
      G_Sleep = 0,
      G_Wait = 1,
      G_IO = 2,
      G_Count = 3
   };

   enum Api : int
   {
      A_Sleep = 0,
      A_SleepEx,
      A_SwitchToThread,
      A_WaitSingle,
      A_WaitSingleEx,
      A_WaitMulti,
      A_WaitMultiEx,
      A_ReadFile,
      A_ReadFileEx,
      A_GetOverlappedResult,
      A_SetEvent,
      A_ReleaseSemaphore,
      A_Count
   };

   static constexpr const char* kApiNames[A_Count] = {
      "Sleep", "SleepEx", "SwitchToThread", "WaitForSingleObject", "WaitForSingleObjectEx",
      "WaitForMultipleObjects", "WaitForMultipleObjectsEx", "ReadFile", "ReadFileEx", "GetOverlappedResult",
      "SetEvent", "ReleaseSemaphore"
   };

   static constexpr uint32_t kMaxThreads = 64;
   static constexpr uint32_t kRowsPerFlush = 60;
   static constexpr uint32_t kAttrEntries = 32; // max (rva, handle) wait sites per report window
   static constexpr uint32_t kSigEntries = 128; // max distinct handles with a recorded signaller
   static constexpr uint32_t kReportEvery = 600; // presents between "[BL Prof]" reports
   static constexpr uint32_t kDescMax = 64;

   using PFN_Sleep = VOID(WINAPI*)(DWORD);
   using PFN_SleepEx = DWORD(WINAPI*)(DWORD, BOOL);
   using PFN_SwitchToThread = BOOL(WINAPI*)();
   using PFN_WaitSingle = DWORD(WINAPI*)(HANDLE, DWORD);
   using PFN_WaitSingleEx = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
   using PFN_WaitMulti = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
   using PFN_WaitMultiEx = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
   using PFN_ReadFile = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
   using PFN_ReadFileEx = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPOVERLAPPED, LPOVERLAPPED_COMPLETION_ROUTINE);
   using PFN_GetOverlappedResult = BOOL(WINAPI*)(HANDLE, LPOVERLAPPED, LPDWORD, BOOL);
   using PFN_SetEvent = BOOL(WINAPI*)(HANDLE);
   using PFN_ReleaseSemaphore = BOOL(WINAPI*)(HANDLE, LONG, LPLONG);
   using PFN_GetThreadDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);

   struct ThreadSlot
   {
      std::atomic<DWORD> tid{ 0 }; // 0 = free. Claimed once per thread with CAS, never released.
      char name[64] = {};
      std::atomic<int64_t> ticks[G_Count] = {};
      std::atomic<uint32_t> calls[G_Count] = {};
      std::atomic<uint64_t> bytes{ 0 }; // ReadFile/ReadFileEx bytes requested
   };

   struct Row
   {
      double time_ms;
      double present_dt_ms;
      double main_ms[3]; // wait, sleep, io
      double render_ms[3]; // wait, sleep, io
      uint64_t other_io_bytes;
      double other_io_ms;
      DWORD busiest_tid;
      double busiest_ms;
      double present_call_ms; // present event -> finish_present, render thread
      DWORD present_tid;
      uint32_t rt_create_n;
      double rt_create_ms;
      uint32_t rt_upload_n;
      double rt_upload_ms;
      uint64_t rt_upload_bytes;
      uint32_t oth_create_n;
      double oth_create_ms;
      uint32_t oth_upload_n;
      // Extended per-frame columns (frame.profile only).
      uint32_t draws, dispatches, binds, desc_binds, rt_binds, clears;
      uint32_t map_cb_n, map_vb_n, map_ib_n, map_other_n, map_tex_n;
      uint64_t map_bytes;
      double rt_cpu_ms, main_cpu_ms, proc_cpu_ms;
      uint32_t sim_ticks;
      double sim_dt_ms;
      uint32_t fp_hold, fp_under, fp_passthrough;
   };

   static ThreadSlot g_slots[kMaxThreads];
   static std::atomic<uint32_t> g_overflow_calls{ 0 };
   static std::atomic<DWORD> g_main_tid{ 0 };
   static std::atomic<DWORD> g_render_tid{ 0 };

   static void* g_orig[A_Count] = {};
   static PFN_GetThreadDescription g_get_thread_description = nullptr;
   static bool g_installed = false;

   // Present-side state (only touched from OnPresent).
   static LARGE_INTEGER g_qpf = {};
   static int64_t g_start_qpc = 0;
   static int64_t g_last_present_qpc = 0;
   static std::vector<Row> g_rows;
   static uint64_t g_window_calls[G_Count] = {};
   static bool g_csv_started = false;
   static bool g_csv_error_logged = false;
   static bool g_names_logged = false;

   // Wait/signal attribution. Waits are recorded only on the main thread; g_attr_mutex guards the tables
   // (the render thread reads and resets them every kReportEvery presents).
   struct WaitEntry { uint32_t rva; uintptr_t handle; uint32_t count; double total_ms; double max_ms; };
   struct SigEntry { uintptr_t handle; DWORD tid; uint32_t rva; };
   static uintptr_t g_exe_base = 0;
   static uint32_t g_exe_size = 0;
   static std::mutex g_attr_mutex;
   static WaitEntry g_wait[kAttrEntries] = {};
   static uint32_t g_wait_n = 0;
   static uint64_t g_wait_overflow = 0;
   static SigEntry g_sig[kSigEntries] = {};
   static uint32_t g_sig_n = 0;
   static uint64_t g_sig_overflow = 0;
   // Present timing: set in FrameProfiler_OnPresent, consumed in FrameProfiler_OnFinishPresent.
   static int64_t g_pending_present_qpc = 0;
   static uint32_t g_presents_since_report = 0;
   static DWORD g_desc_logged[kDescMax] = {};
   static uint32_t g_desc_logged_n = 0;

   // Render-thread wait/sleep table, keyed by caller RVA only. Guarded by g_attr_mutex.
   struct RWaitEntry { uint32_t rva; uint32_t count; double total_ms; double max_ms; };
   static RWaitEntry g_rwait[kAttrEntries] = {};
   static uint32_t g_rwait_n = 0;

   // Resource create/upload accumulators. Index 0 = render thread, 1 = other threads. Written from ReShade events.
   struct ResAcc
   {
      std::atomic<uint32_t> create_n{ 0 };
      std::atomic<int64_t> create_ticks{ 0 };
      std::atomic<uint32_t> upload_n{ 0 };
      std::atomic<int64_t> upload_ticks{ 0 };
      std::atomic<uint64_t> upload_bytes{ 0 };
   };
   static ResAcc g_res[2];

   // Map/unmap pairs in flight. Guarded by g_map_mutex.
   struct PendingMap { uint64_t handle; uint32_t sub; DWORD tid; int64_t t0; uint64_t bytes; bool active; };
   static constexpr uint32_t kPendingMaps = 64;
   static PendingMap g_maps[kPendingMaps] = {};
   static std::mutex g_map_mutex;

   static void Log(reshade::log::level lv, const char* fmt, ...)
   {
      char buf[512];
      va_list args;
      va_start(args, fmt);
      vsnprintf(buf, sizeof(buf), fmt, args);
      va_end(args);
      reshade::log::message(lv, buf);
   }

   static int64_t QpcNow()
   {
      LARGE_INTEGER li;
      QueryPerformanceCounter(&li);
      return li.QuadPart;
   }

   static double TicksToMs(int64_t t)
   {
      return static_cast<double>(t) * 1000.0 / static_cast<double>(g_qpf.QuadPart);
   }

   static void FillName(ThreadSlot& s, DWORD tid)
   {
      s.name[0] = 0;
      if (g_get_thread_description)
      {
         PWSTR desc = nullptr;
         if (SUCCEEDED(g_get_thread_description(GetCurrentThread(), &desc)) && desc)
         {
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, s.name, static_cast<int>(sizeof(s.name) - 1), nullptr, nullptr);
            LocalFree(desc);
         }
      }
      if (!s.name[0])
         snprintf(s.name, sizeof(s.name), "thread %lu", static_cast<unsigned long>(tid));
   }

   // Lock-free slot lookup. Claims a free slot with CAS. Returns nullptr if all slots are taken.
   static ThreadSlot* GetSlot()
   {
      const DWORD tid = GetCurrentThreadId();
      for (uint32_t i = 0; i < kMaxThreads; ++i)
      {
         ThreadSlot& s = g_slots[i];
         const DWORD cur = s.tid.load(std::memory_order_acquire);
         if (cur == tid)
            return &s;
         if (cur == 0)
         {
            DWORD expected = 0;
            if (s.tid.compare_exchange_strong(expected, tid, std::memory_order_acq_rel))
            {
               FillName(s, tid);
               return &s;
            }
            if (expected == tid)
               return &s;
         }
      }
      return nullptr;
   }

   static void Account(Group g, int64_t ticks, uint64_t bytes)
   {
      ThreadSlot* s = GetSlot();
      if (!s)
      {
         g_overflow_calls.fetch_add(1, std::memory_order_relaxed);
         return;
      }
      s->ticks[g].fetch_add(ticks, std::memory_order_relaxed);
      s->calls[g].fetch_add(1, std::memory_order_relaxed);
      if (bytes)
         s->bytes.fetch_add(bytes, std::memory_order_relaxed);
   }

   // Runs the original call, timing it when the profiler is on. Hooks are pass-through when off.
   template <typename Fn>
   static auto Timed(Group g, uint64_t bytes, Fn call) -> decltype(call())
   {
      if (!g_frame_profile)
         return call();
      const int64_t t0 = QpcNow();
      if constexpr (std::is_void_v<decltype(call())>)
      {
         call();
         Account(g, QpcNow() - t0, bytes);
      }
      else
      {
         auto r = call();
         Account(g, QpcNow() - t0, bytes);
         return r;
      }
   }

   static uint32_t RvaOf(const void* ret)
   {
      const uintptr_t a = reinterpret_cast<uintptr_t>(ret);
      if (!g_exe_base || a < g_exe_base || a >= g_exe_base + g_exe_size)
         return 0;
      return static_cast<uint32_t>(a - g_exe_base);
   }

   // Render thread only. Adds one blocked interval to the caller-RVA table.
   static void RecordRenderWait(uint32_t rva, double ms)
   {
      std::lock_guard<std::mutex> lock(g_attr_mutex);
      for (uint32_t i = 0; i < g_rwait_n; ++i)
      {
         RWaitEntry& e = g_rwait[i];
         if (e.rva == rva)
         {
            e.count++;
            e.total_ms += ms;
            if (ms > e.max_ms)
               e.max_ms = ms;
            return;
         }
      }
      if (g_rwait_n >= kAttrEntries)
      {
         ++g_wait_overflow;
         return;
      }
      g_rwait[g_rwait_n++] = { rva, 1, ms, ms };
   }

   // Render thread: caller-RVA table. Main thread: (rva, handle) table. Other threads: ignored.
   static void RecordWait(const void* ret, HANDLE h, int64_t ticks)
   {
      const DWORD tid = GetCurrentThreadId();
      if (tid == g_render_tid.load(std::memory_order_relaxed))
      {
         RecordRenderWait(RvaOf(ret), TicksToMs(ticks));
         return;
      }
      if (tid != g_main_tid.load(std::memory_order_relaxed))
         return;
      const uint32_t rva = RvaOf(ret);
      const uintptr_t hv = reinterpret_cast<uintptr_t>(h);
      const double ms = TicksToMs(ticks);
      std::lock_guard<std::mutex> lock(g_attr_mutex);
      for (uint32_t i = 0; i < g_wait_n; ++i)
      {
         WaitEntry& e = g_wait[i];
         if (e.rva == rva && e.handle == hv)
         {
            e.count++;
            e.total_ms += ms;
            if (ms > e.max_ms)
               e.max_ms = ms;
            return;
         }
      }
      if (g_wait_n >= kAttrEntries)
      {
         ++g_wait_overflow;
         return;
      }
      g_wait[g_wait_n++] = { rva, hv, 1, ms, ms };
   }

   // Any thread. Keeps the last signaller (tid + caller RVA) of each handle.
   static void RecordSignal(const void* ret, HANDLE h)
   {
      const uintptr_t hv = reinterpret_cast<uintptr_t>(h);
      const DWORD tid = GetCurrentThreadId();
      const uint32_t rva = RvaOf(ret);
      std::lock_guard<std::mutex> lock(g_attr_mutex);
      for (uint32_t i = 0; i < g_sig_n; ++i)
      {
         if (g_sig[i].handle == hv)
         {
            g_sig[i].tid = tid;
            g_sig[i].rva = rva;
            return;
         }
      }
      if (g_sig_n >= kSigEntries)
      {
         ++g_sig_overflow;
         return;
      }
      g_sig[g_sig_n++] = { hv, tid, rva };
   }

   // Like Timed, and also attributes the blocked interval to the caller (ret) and the handle (main thread only).
   template <typename Fn>
   static auto TimedWait(Group g, const void* ret, HANDLE h, Fn call) -> decltype(call())
   {
      if (!g_frame_profile)
         return call();
      const int64_t t0 = QpcNow();
      if constexpr (std::is_void_v<decltype(call())>)
      {
         call();
         const int64_t dt = QpcNow() - t0;
         Account(g, dt, 0);
         RecordWait(ret, h, dt);
      }
      else
      {
         auto r = call();
         const int64_t dt = QpcNow() - t0;
         Account(g, dt, 0);
         RecordWait(ret, h, dt);
         return r;
      }
   }

   // ---- Long waits (DEVELOPMENT, always on; not gated by frame.profile) ----
   // A wait that has been blocked longer than kLongWaitTrackMs is listed here while it is still blocked, so a release
   // from another thread can be attributed to its caller. Lock-free: slots are claimed with CAS, freed by their waiter.
   static constexpr uint32_t kLongWaitSlots = 64;
   static constexpr double kLongWaitTrackMs = 50.0;
   static constexpr double kLongWaitLogMs = 200.0;
   static constexpr DWORD kLongWaitSliceMs = 50; // WaitForSingleObject is issued in slices so a long wait can be tracked

   struct LongWaitSlot
   {
      std::atomic<uintptr_t> handle{ 0 }; // 0 = free
      std::atomic<int64_t> t0{ 0 };       // QPC when the wait started
      std::atomic<DWORD> tid{ 0 };        // waiting thread
   };
   static LongWaitSlot g_long_waits[kLongWaitSlots];

   static const char* RoleOf(DWORD tid)
   {
      if (tid == g_render_tid.load(std::memory_order_relaxed))
         return "render";
      if (tid == g_main_tid.load(std::memory_order_relaxed))
         return "main";
      return "other";
   }

   // "<module file name>+0x<offset>" of a return address, or "unknown".
   static std::string CallerText(const void* ret)
   {
      HMODULE mod = nullptr;
      if (!ret || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                      reinterpret_cast<LPCWSTR>(ret), &mod) || !mod)
         return "unknown";
      wchar_t path[MAX_PATH] = {};
      GetModuleFileNameW(mod, path, MAX_PATH);
      const wchar_t* name = path;
      for (const wchar_t* p = path; *p; ++p)
         if (*p == L'\\' || *p == L'/')
            name = p + 1;
      char narrow[128] = {};
      WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, static_cast<int>(sizeof(narrow) - 1), nullptr, nullptr);
      char out[192] = {};
      snprintf(out, sizeof(out), "%s+0x%llX", narrow,
               static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ret) - reinterpret_cast<uintptr_t>(mod)));
      return out;
   }

   // Claims a free slot for a wait on h. Returns the slot index, or -1 when the table is full.
   static int LongWaitClaim(uintptr_t h, int64_t t0, DWORD tid)
   {
      if (!h)
         return -1;
      for (uint32_t i = 0; i < kLongWaitSlots; ++i)
      {
         LongWaitSlot& s = g_long_waits[i];
         uintptr_t expected = 0;
         if (s.handle.load(std::memory_order_relaxed) == 0 && s.handle.compare_exchange_strong(expected, h, std::memory_order_acq_rel))
         {
            s.t0.store(t0, std::memory_order_relaxed);
            s.tid.store(tid, std::memory_order_relaxed);
            return static_cast<int>(i);
         }
      }
      return -1;
   }

   // Any thread, before the native release call: logs a release of a handle that is currently being waited on (tracked).
   static void LongWaitNoteRelease(const void* ret, HANDLE h)
   {
      const uintptr_t hv = reinterpret_cast<uintptr_t>(h);
      if (!hv)
         return;
      for (LongWaitSlot& s : g_long_waits)
      {
         if (s.handle.load(std::memory_order_acquire) != hv)
            continue;
         const DWORD self = GetCurrentThreadId();
         const double ms = TicksToMs(QpcNow() - s.t0.load(std::memory_order_relaxed));
         Log(reshade::log::level::info, "[BL Wait] released by tid %lu (%s) caller %s after %.1f ms",
             static_cast<unsigned long>(self), RoleOf(self), CallerText(ret).c_str(), ms);
         return;
      }
   }

   // ---- Hooks (signatures match the originals; WINAPI = stdcall on x86) ----

   static VOID WINAPI Hook_Sleep(DWORD ms)
   {
      void* const ret = _ReturnAddress();
      TimedWait(G_Sleep, ret, nullptr, [=] { reinterpret_cast<PFN_Sleep>(g_orig[A_Sleep])(ms); });
   }

   static DWORD WINAPI Hook_SleepEx(DWORD ms, BOOL alertable)
   {
      void* const ret = _ReturnAddress();
      return TimedWait(G_Sleep, ret, nullptr, [=] { return reinterpret_cast<PFN_SleepEx>(g_orig[A_SleepEx])(ms, alertable); });
   }

   static BOOL WINAPI Hook_SwitchToThread()
   {
      return Timed(G_Sleep, 0, [] { return reinterpret_cast<PFN_SwitchToThread>(g_orig[A_SwitchToThread])(); });
   }

   static DWORD WINAPI Hook_WaitForSingleObject(HANDLE h, DWORD ms)
   {
      void* const ret = _ReturnAddress();
      const auto orig = reinterpret_cast<PFN_WaitSingle>(g_orig[A_WaitSingle]);
      const DWORD tid = GetCurrentThreadId();
      const int64_t t_start = QpcNow();
      const uintptr_t hv = reinterpret_cast<uintptr_t>(h);
      const bool infinite = ms == INFINITE;
      DWORD remaining = ms;
      int slot = -1;
      DWORD result = WAIT_FAILED;
      // Issued in slices: same total timeout and result, but a wait that lasts long enough gets listed while it is blocked.
      for (;;)
      {
         const DWORD slice = (infinite || remaining > kLongWaitSliceMs) ? kLongWaitSliceMs : remaining;
         result = orig(h, slice);
         if (result != WAIT_TIMEOUT)
            break;
         if (!infinite)
         {
            if (remaining <= slice)
               break;
            remaining -= slice;
         }
         if (slot < 0 && TicksToMs(QpcNow() - t_start) > kLongWaitTrackMs)
            slot = LongWaitClaim(hv, t_start, tid);
      }
      const int64_t dt = QpcNow() - t_start;
      if (slot >= 0)
         g_long_waits[slot].handle.store(0, std::memory_order_release);
      if (g_frame_profile)
      {
         Account(G_Wait, dt, 0);
         RecordWait(ret, h, dt);
      }
      const double total_ms = TicksToMs(dt);
      if (total_ms > kLongWaitLogMs)
      {
         Log(reshade::log::level::info, "[BL Wait] long WFSO %.1f ms tid %lu (%s) handle 0x%p timeout %lu caller %s result 0x%lX",
             total_ms, static_cast<unsigned long>(tid), RoleOf(tid), reinterpret_cast<void*>(hv), static_cast<unsigned long>(ms),
             CallerText(ret).c_str(), static_cast<unsigned long>(result));
      }
      return result;
   }

   static DWORD WINAPI Hook_WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL alertable)
   {
      void* const ret = _ReturnAddress();
      return TimedWait(G_Wait, ret, h, [=] { return reinterpret_cast<PFN_WaitSingleEx>(g_orig[A_WaitSingleEx])(h, ms, alertable); });
   }

   static DWORD WINAPI Hook_WaitForMultipleObjects(DWORD count, const HANDLE* handles, BOOL all, DWORD ms)
   {
      void* const ret = _ReturnAddress();
      const HANDLE first = (count && handles) ? handles[0] : nullptr;
      return TimedWait(G_Wait, ret, first, [=] { return reinterpret_cast<PFN_WaitMulti>(g_orig[A_WaitMulti])(count, handles, all, ms); });
   }

   static DWORD WINAPI Hook_WaitForMultipleObjectsEx(DWORD count, const HANDLE* handles, BOOL all, DWORD ms, BOOL alertable)
   {
      void* const ret = _ReturnAddress();
      const HANDLE first = (count && handles) ? handles[0] : nullptr;
      return TimedWait(G_Wait, ret, first, [=] { return reinterpret_cast<PFN_WaitMultiEx>(g_orig[A_WaitMultiEx])(count, handles, all, ms, alertable); });
   }

   static BOOL WINAPI Hook_SetEvent(HANDLE h)
   {
      void* const ret = _ReturnAddress();
      LongWaitNoteRelease(ret, h);
      const BOOL r = reinterpret_cast<PFN_SetEvent>(g_orig[A_SetEvent])(h);
      if (g_frame_profile)
         RecordSignal(ret, h);
      return r;
   }

   static BOOL WINAPI Hook_ReleaseSemaphore(HANDLE h, LONG count, LPLONG previous)
   {
      void* const ret = _ReturnAddress();
      LongWaitNoteRelease(ret, h);
      const BOOL r = reinterpret_cast<PFN_ReleaseSemaphore>(g_orig[A_ReleaseSemaphore])(h, count, previous);
      if (g_frame_profile)
         RecordSignal(ret, h);
      return r;
   }

   static BOOL WINAPI Hook_ReadFile(HANDLE h, LPVOID buf, DWORD n, LPDWORD read, LPOVERLAPPED ov)
   {
      return Timed(G_IO, n, [=] { return reinterpret_cast<PFN_ReadFile>(g_orig[A_ReadFile])(h, buf, n, read, ov); });
   }

   static BOOL WINAPI Hook_ReadFileEx(HANDLE h, LPVOID buf, DWORD n, LPOVERLAPPED ov, LPOVERLAPPED_COMPLETION_ROUTINE cb)
   {
      return Timed(G_IO, n, [=] { return reinterpret_cast<PFN_ReadFileEx>(g_orig[A_ReadFileEx])(h, buf, n, ov, cb); });
   }

   static BOOL WINAPI Hook_GetOverlappedResult(HANDLE h, LPOVERLAPPED ov, LPDWORD transferred, BOOL wait)
   {
      return Timed(G_IO, 0, [=] { return reinterpret_cast<PFN_GetOverlappedResult>(g_orig[A_GetOverlappedResult])(h, ov, transferred, wait); });
   }

   // Walks BrutalLegend.exe's import directory, KERNEL32 only, and swaps the IAT entries of the listed APIs.
   static int InstallIatHooks()
   {
      const auto* base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
      if (!base)
         return -1;
      const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
      if (dos->e_magic != IMAGE_DOS_SIGNATURE)
         return -1;
      const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
      if (nt->Signature != IMAGE_NT_SIGNATURE)
         return -1;
      const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
      if (!dir.VirtualAddress)
         return -1;

      const void* hooks[A_Count] = {
         reinterpret_cast<const void*>(&Hook_Sleep), reinterpret_cast<const void*>(&Hook_SleepEx),
         reinterpret_cast<const void*>(&Hook_SwitchToThread), reinterpret_cast<const void*>(&Hook_WaitForSingleObject),
         reinterpret_cast<const void*>(&Hook_WaitForSingleObjectEx), reinterpret_cast<const void*>(&Hook_WaitForMultipleObjects),
         reinterpret_cast<const void*>(&Hook_WaitForMultipleObjectsEx), reinterpret_cast<const void*>(&Hook_ReadFile),
         reinterpret_cast<const void*>(&Hook_ReadFileEx), reinterpret_cast<const void*>(&Hook_GetOverlappedResult),
         reinterpret_cast<const void*>(&Hook_SetEvent), reinterpret_cast<const void*>(&Hook_ReleaseSemaphore)
      };

      int hooked = 0;
      const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
      for (; desc->Name; ++desc)
      {
         const char* dll = reinterpret_cast<const char*>(base + desc->Name);
         if (_stricmp(dll, "kernel32.dll") != 0)
            continue;

         auto* iat = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<uint8_t*>(base) + desc->FirstThunk);
         const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA*>(base + (desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk));
         for (; names->u1.AddressOfData; ++names, ++iat)
         {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
               continue;
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            for (int k = 0; k < A_Count; ++k)
            {
               if (g_orig[k] || strcmp(reinterpret_cast<const char*>(ibn->Name), kApiNames[k]) != 0)
                  continue;
               DWORD old_protect = 0;
               if (!VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), PAGE_READWRITE, &old_protect))
                  break;
               g_orig[k] = reinterpret_cast<void*>(iat->u1.Function);
               iat->u1.Function = reinterpret_cast<ULONG_PTR>(hooks[k]);
               DWORD ignored = 0;
               VirtualProtect(&iat->u1.Function, sizeof(iat->u1.Function), old_protect, &ignored);
               ++hooked;
               break;
            }
         }
      }
      return hooked;
   }

   static std::filesystem::path CsvPath()
   {
      wchar_t buf[MAX_PATH] = {};
      GetModuleFileNameW(nullptr, buf, MAX_PATH);
      return std::filesystem::path(buf).parent_path() / L"Luma_BL_Frame.csv";
   }

   static void LogThreadNames()
   {
      for (uint32_t i = 0; i < kMaxThreads; ++i)
      {
         const ThreadSlot& s = g_slots[i];
         const DWORD tid = s.tid.load(std::memory_order_acquire);
         if (tid)
            Log(reshade::log::level::info, "[BL Frame] thread %lu = %s", static_cast<unsigned long>(tid), s.name);
      }
   }

   // Writes buffered rows. The first flush of the session truncates the file and writes the header once.
   static void FlushRows()
   {
      if (g_rows.empty())
         return;

      if (!g_names_logged)
      {
         g_names_logged = true;
         LogThreadNames();
      }
      Log(reshade::log::level::info, "[BL Frame] last %u rows: calls sleep %llu wait %llu io %llu, overflow %u",
          static_cast<unsigned>(g_rows.size()), static_cast<unsigned long long>(g_window_calls[G_Sleep]),
          static_cast<unsigned long long>(g_window_calls[G_Wait]), static_cast<unsigned long long>(g_window_calls[G_IO]),
          g_overflow_calls.load(std::memory_order_relaxed));
      for (uint64_t& c : g_window_calls)
         c = 0;

      std::ofstream f;
      if (!g_csv_started)
         f.open(CsvPath(), std::ios::out | std::ios::trunc);
      else
         f.open(CsvPath(), std::ios::out | std::ios::app);
      if (!f)
      {
         if (!g_csv_error_logged)
         {
            g_csv_error_logged = true;
            Log(reshade::log::level::error, "[BL Frame] could not open Luma_BL_Frame.csv");
         }
         g_rows.clear();
         return;
      }
      if (!g_csv_started)
      {
         f << "time_ms,present_dt_ms,main_wait_ms,main_sleep_ms,main_io_ms,render_wait_ms,render_sleep_ms,render_io_ms,"
              "other_io_bytes,other_io_ms,busiest_other_tid,busiest_other_blocked_ms,present_call_ms,present_tid,"
              "rt_create_n,rt_create_ms,rt_upload_n,rt_upload_ms,rt_upload_mb,oth_create_n,oth_create_ms,oth_upload_n,rt_sleep_ms,"
              "draws,dispatches,binds,desc_binds,rt_binds,clears,map_cb_n,map_vb_n,map_ib_n,map_other_n,map_tex_n,map_bytes,"
              "rt_cpu_ms,main_cpu_ms,proc_cpu_ms,sim_ticks,sim_dt_ms,fp_hold,fp_under,fp_passthrough\n";
         g_csv_started = true;
      }
      f.setf(std::ios::fixed);
      f.precision(3);
      for (const Row& r : g_rows)
      {
         f << r.time_ms << ',' << r.present_dt_ms << ','
           << r.main_ms[0] << ',' << r.main_ms[1] << ',' << r.main_ms[2] << ','
           << r.render_ms[0] << ',' << r.render_ms[1] << ',' << r.render_ms[2] << ','
           << r.other_io_bytes << ',' << r.other_io_ms << ','
           << static_cast<unsigned long>(r.busiest_tid) << ',' << r.busiest_ms << ','
           << r.present_call_ms << ',' << static_cast<unsigned long>(r.present_tid) << ','
           << r.rt_create_n << ',' << r.rt_create_ms << ',' << r.rt_upload_n << ',' << r.rt_upload_ms << ','
           << static_cast<double>(r.rt_upload_bytes) / (1024.0 * 1024.0) << ','
           << r.oth_create_n << ',' << r.oth_create_ms << ',' << r.oth_upload_n << ',' << r.render_ms[G_Sleep] << ','
           << r.draws << ',' << r.dispatches << ',' << r.binds << ',' << r.desc_binds << ',' << r.rt_binds << ',' << r.clears << ','
           << r.map_cb_n << ',' << r.map_vb_n << ',' << r.map_ib_n << ',' << r.map_other_n << ',' << r.map_tex_n << ','
           << r.map_bytes << ','
           << r.rt_cpu_ms << ',' << r.main_cpu_ms << ',' << r.proc_cpu_ms << ','
           << r.sim_ticks << ',' << r.sim_dt_ms << ','
           << r.fp_hold << ',' << r.fp_under << ',' << r.fp_passthrough << '\n';
      }
      g_rows.clear();
   }

   static void ResetAllSlots()
   {
      for (ThreadSlot& s : g_slots)
      {
         for (int g = 0; g < G_Count; ++g)
         {
            s.ticks[g].store(0, std::memory_order_relaxed);
            s.calls[g].store(0, std::memory_order_relaxed);
         }
         s.bytes.store(0, std::memory_order_relaxed);
      }
   }

   // Logs GetThreadDescription of a thread once (render thread only).
   static void LogThreadDesc(DWORD tid)
   {
      if (!tid)
         return;
      for (uint32_t i = 0; i < g_desc_logged_n; ++i)
         if (g_desc_logged[i] == tid)
            return;
      if (g_desc_logged_n < kDescMax)
         g_desc_logged[g_desc_logged_n++] = tid;

      char name[128] = "(none)";
      HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
      if (th && g_get_thread_description)
      {
         PWSTR desc = nullptr;
         if (SUCCEEDED(g_get_thread_description(th, &desc)) && desc)
         {
            if (desc[0])
               WideCharToMultiByte(CP_UTF8, 0, desc, -1, name, static_cast<int>(sizeof(name) - 1), nullptr, nullptr);
            LocalFree(desc);
         }
      }
      if (th)
         CloseHandle(th);
      Log(reshade::log::level::info, "[BL Prof] thread %lu desc=%s", static_cast<unsigned long>(tid), name);
   }

   // Every kReportEvery presents (render thread): logs the top 10 wait sites by total blocked time, then clears the wait table.
   static void ReportWaits()
   {
      static WaitEntry top[kAttrEntries];
      static SigEntry sigs[kSigEntries];
      static RWaitEntry rtop[kAttrEntries];
      uint32_t n = 0;
      uint32_t sig_n = 0;
      uint32_t rn = 0;
      {
         std::lock_guard<std::mutex> lock(g_attr_mutex);
         n = g_wait_n;
         for (uint32_t i = 0; i < n; ++i)
            top[i] = g_wait[i];
         sig_n = g_sig_n;
         for (uint32_t i = 0; i < sig_n; ++i)
            sigs[i] = g_sig[i];
         g_wait_n = 0;
         rn = g_rwait_n;
         for (uint32_t i = 0; i < rn; ++i)
            rtop[i] = g_rwait[i];
         g_rwait_n = 0;
      }
      if (rn)
      {
         std::sort(rtop, rtop + rn, [](const RWaitEntry& a, const RWaitEntry& b) { return a.total_ms > b.total_ms; });
         const uint32_t rshown = rn < 10 ? rn : 10;
         for (uint32_t i = 0; i < rshown; ++i)
         {
            const RWaitEntry& e = rtop[i];
            Log(reshade::log::level::info, "[BL Prof] rwait rva=0x%X n=%u total=%.1fms max=%.2fms",
                static_cast<unsigned>(e.rva), static_cast<unsigned>(e.count), e.total_ms, e.max_ms);
         }
      }
      if (!n)
         return;

      std::sort(top, top + n, [](const WaitEntry& a, const WaitEntry& b) { return a.total_ms > b.total_ms; });
      LogThreadDesc(g_main_tid.load(std::memory_order_relaxed));
      const uint32_t shown = n < 10 ? n : 10;
      for (uint32_t i = 0; i < shown; ++i)
      {
         const WaitEntry& e = top[i];
         DWORD sig_tid = 0;
         uint32_t sig_rva = 0;
         for (uint32_t j = 0; j < sig_n; ++j)
         {
            if (sigs[j].handle == e.handle)
            {
               sig_tid = sigs[j].tid;
               sig_rva = sigs[j].rva;
               break;
            }
         }
         Log(reshade::log::level::info, "[BL Prof] wait rva=0x%X h=%p n=%u total=%.1fms max=%.2fms sig_tid=%u sig_rva=0x%X",
             static_cast<unsigned>(e.rva), reinterpret_cast<void*>(e.handle), static_cast<unsigned>(e.count), e.total_ms, e.max_ms,
             static_cast<unsigned>(sig_tid), static_cast<unsigned>(sig_rva));
         LogThreadDesc(sig_tid);
      }
   }

   // ---- Resource events (ReShade addon events, registered in main.cpp, DEVELOPMENT only) ----
   // All callbacks return false so ReShade never cancels the call. create = create_resource..init_resource on the same
   // thread. map = map..unmap on the same thread, resource and subresource. update_* fire before the native call with no
   // completion callback, so they count and sum bytes only (no ms).
   static thread_local int64_t t_create_qpc = 0;

   // Descriptor remembered per thread by create_resource (pre), read by init_resource (post).
   struct CreateInfo
   {
      int type = 0;
      uint32_t w = 0, h = 0, d = 0, levels = 0, samples = 0;
      int fmt = 0;
      uint32_t usage = 0;
      int heap = 0;
      uint32_t flags = 0;
      bool init = false;
   };
   static thread_local CreateInfo t_create_info = {};

   // Whole-frame creation totals (all threads) and slow-create count. Reset at each present.
   static std::atomic<uint32_t> g_frame_create_n{ 0 };
   static std::atomic<int64_t> g_frame_create_ticks{ 0 };
   static std::atomic<uint32_t> g_frame_slow_n{ 0 };
   static constexpr double kSlowCreateMs = 2.0;
   // The game is 32-bit: any single transfer above 4 GiB is a bogus size, so it is not counted.
   static constexpr uint64_t kMaxKnownBytes = 1ull << 32;

   static uint64_t KnownBytes(uint64_t bytes)
   {
      return bytes <= kMaxKnownBytes ? bytes : 0;
   }

   // Extended per-frame event counters. Events bump relaxed atomics; FrameProfiler_OnPresent exchanges them to zero.
   struct FrameCounts
   {
      std::atomic<uint32_t> draws{ 0 }, dispatches{ 0 }, binds{ 0 }, desc_binds{ 0 }, rt_binds{ 0 }, clears{ 0 };
      std::atomic<uint32_t> map_cb{ 0 }, map_vb{ 0 }, map_ib{ 0 }, map_other{ 0 }, map_tex{ 0 };
      std::atomic<uint64_t> map_bytes{ 0 };
   };
   static FrameCounts g_fc;

   // Read-only sources wired from main.cpp (DEVELOPMENT). A null source leaves its column at 0.
   struct ExtSources
   {
      const uint32_t* sim_ticks = nullptr;
      const std::atomic<float>* sim_dt = nullptr;
      const bool* sim60 = nullptr;
      const bool* pacing = nullptr;
      const uint32_t* fp_hold = nullptr;
      const uint32_t* fp_under = nullptr;
      const uint32_t* fp_passthrough = nullptr;
   };
   static ExtSources g_ext;

   // Creations of the current frame (any thread). Keeps the kFrameCreates longest. Guarded by g_create_mutex.
   struct CreateRec { double ms; int type; uint32_t w, h, fmt, usage; uint32_t rva; };
   static constexpr uint32_t kFrameCreates = 16;
   static constexpr double kRecordCreateMs = 0.05; // creates at least this long are candidates for the list
   static std::mutex g_create_mutex;
   static CreateRec g_creates[kFrameCreates] = {};
   static uint32_t g_creates_n = 0;

   // Spike detection and summary state (render thread only).
   static constexpr uint32_t kMedianWin = 240;
   static constexpr uint32_t kSummaryEvery = 1200;
   static constexpr double kSpikeRatio = 1.6;
   static double g_dt_ring[kMedianWin] = {};
   static uint32_t g_dt_n = 0;
   static Row g_hist[3] = {};
   static uint32_t g_hist_n = 0;
   struct WinSample { double dt, draws, maps, cb, rt_cpu; bool spike; };
   static std::vector<WinSample> g_win;
   static bool g_was_on = false;
   static bool g_prev_valid = false;
   static bool g_session_pending = false;
   static uint32_t g_prev_sim_ticks = 0;
   static uint32_t g_prev_fp[3] = {};   // hold, under, passthrough
   static double g_prev_cpu[3] = {};    // absolute CPU ms: render thread, main thread, process

   static void Out(std::string& s, const char* fmt, ...)
   {
      char b[256];
      va_list args;
      va_start(args, fmt);
      vsnprintf(b, sizeof(b), fmt, args);
      va_end(args);
      s += b;
   }

   static void LogBig(reshade::log::level lv, const std::string& s)
   {
      reshade::log::message(lv, s.c_str());
   }

   static uint64_t MapsOf(const Row& r)
   {
      return static_cast<uint64_t>(r.map_cb_n) + r.map_vb_n + r.map_ib_n + r.map_other_n + r.map_tex_n;
   }

   // Every column as name=value, same names and order as the CSV header.
   static void AppendRowKV(std::string& s, const Row& r)
   {
      Out(s, "time_ms=%.2f present_dt_ms=%.2f main_wait_ms=%.2f main_sleep_ms=%.2f main_io_ms=%.2f render_wait_ms=%.2f render_sleep_ms=%.2f render_io_ms=%.2f ",
          r.time_ms, r.present_dt_ms, r.main_ms[0], r.main_ms[1], r.main_ms[2], r.render_ms[0], r.render_ms[1], r.render_ms[2]);
      Out(s, "other_io_bytes=%llu other_io_ms=%.2f busiest_other_tid=%u busiest_other_blocked_ms=%.2f present_call_ms=%.2f present_tid=%u ",
          static_cast<unsigned long long>(r.other_io_bytes), r.other_io_ms, static_cast<unsigned>(r.busiest_tid), r.busiest_ms,
          r.present_call_ms, static_cast<unsigned>(r.present_tid));
      Out(s, "rt_create_n=%u rt_create_ms=%.2f rt_upload_n=%u rt_upload_ms=%.2f rt_upload_mb=%.3f oth_create_n=%u oth_create_ms=%.2f oth_upload_n=%u rt_sleep_ms=%.2f ",
          r.rt_create_n, r.rt_create_ms, r.rt_upload_n, r.rt_upload_ms, static_cast<double>(r.rt_upload_bytes) / (1024.0 * 1024.0),
          r.oth_create_n, r.oth_create_ms, r.oth_upload_n, r.render_ms[G_Sleep]);
      Out(s, "draws=%u dispatches=%u binds=%u desc_binds=%u rt_binds=%u clears=%u ",
          r.draws, r.dispatches, r.binds, r.desc_binds, r.rt_binds, r.clears);
      Out(s, "map_cb_n=%u map_vb_n=%u map_ib_n=%u map_other_n=%u map_tex_n=%u map_bytes=%llu ",
          r.map_cb_n, r.map_vb_n, r.map_ib_n, r.map_other_n, r.map_tex_n, static_cast<unsigned long long>(r.map_bytes));
      Out(s, "rt_cpu_ms=%.2f main_cpu_ms=%.2f proc_cpu_ms=%.2f sim_ticks=%u sim_dt_ms=%.2f fp_hold=%u fp_under=%u fp_passthrough=%u",
          r.rt_cpu_ms, r.main_cpu_ms, r.proc_cpu_ms, r.sim_ticks, r.sim_dt_ms, r.fp_hold, r.fp_under, r.fp_passthrough);
   }

   // First BrutalLegend.exe frame on the current stack (0 if none).
   static uint32_t CallerRva()
   {
      void* frames[32] = {};
      const ULONG n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
      for (ULONG i = 0; i < n; ++i)
      {
         const uint32_t rva = RvaOf(frames[i]);
         if (rva)
            return rva;
      }
      return 0;
   }

   static void RecordCreate(const CreateInfo& ci, double ms, uint32_t rva)
   {
      const CreateRec rec = { ms, ci.type, ci.w, ci.h, static_cast<uint32_t>(ci.fmt), ci.usage, rva };
      std::lock_guard<std::mutex> lock(g_create_mutex);
      if (g_creates_n < kFrameCreates)
      {
         g_creates[g_creates_n++] = rec;
         return;
      }
      uint32_t min_i = 0;
      for (uint32_t i = 1; i < kFrameCreates; ++i)
         if (g_creates[i].ms < g_creates[min_i].ms)
            min_i = i;
      if (ms > g_creates[min_i].ms)
         g_creates[min_i] = rec;
   }

   static uint64_t FileTimeU64(const FILETIME& f)
   {
      return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
   }

   // Absolute kernel+user CPU ms of a thread (0 when it cannot be read).
   static double ThreadCpuTotalMs(DWORD tid)
   {
      if (!tid)
         return 0.0;
      HANDLE th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
      if (!th)
         return 0.0;
      FILETIME c = {}, e = {}, k = {}, u = {};
      double ms = 0.0;
      if (GetThreadTimes(th, &c, &e, &k, &u))
         ms = static_cast<double>(FileTimeU64(k) + FileTimeU64(u)) / 10000.0;
      CloseHandle(th);
      return ms;
   }

   static double ProcessCpuTotalMs()
   {
      FILETIME c = {}, e = {}, k = {}, u = {};
      if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
         return 0.0;
      return static_cast<double>(FileTimeU64(k) + FileTimeU64(u)) / 10000.0;
   }

   // Least-squares slope of y on x (0 when x has no spread).
   static double LsSlope(const std::vector<double>& x, const std::vector<double>& y)
   {
      const double n = static_cast<double>(x.size());
      double sx = 0.0, sy = 0.0, sxy = 0.0, sxx = 0.0;
      for (size_t i = 0; i < x.size(); ++i)
      {
         sx += x[i];
         sy += y[i];
         sxy += x[i] * y[i];
         sxx += x[i] * x[i];
      }
      const double den = n * sxx - sx * sx;
      return (n < 2.0 || den <= 1e-9) ? 0.0 : (n * sxy - sx * sy) / den;
   }

   static ResAcc& ResCat()
   {
      const bool render = GetCurrentThreadId() == g_render_tid.load(std::memory_order_relaxed);
      return g_res[render ? 0 : 1];
   }

   // Bytes of a box copy. Returns 0 unless the box and row pitch are sane (row_pitch can be garbage before the native call).
   static uint64_t BoxBytes(uint32_t row_pitch, const reshade::api::subresource_box* box)
   {
      if (!box || !row_pitch || row_pitch > (1u << 20))
         return 0;
      const uint64_t rows = box->bottom > box->top ? box->bottom - box->top : 0;
      const uint64_t slices = box->back > box->front ? box->back - box->front : 1;
      if (rows > (1u << 20) || slices > (1u << 16))
         return 0;
      return KnownBytes(rows * slices * row_pitch);
   }

   static bool OnCreateResource(reshade::api::device* device, reshade::api::resource_desc& desc, reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state)
   {
      (void)device; (void)initial_state;
      t_create_qpc = g_frame_profile ? QpcNow() : 0;
      if (g_frame_profile)
      {
         CreateInfo& ci = t_create_info;
         const bool buf = desc.type == reshade::api::resource_type::buffer;
         ci.type = static_cast<int>(desc.type);
         ci.w = buf ? static_cast<uint32_t>(desc.buffer.size) : desc.texture.width;
         ci.h = buf ? 0 : desc.texture.height;
         ci.d = buf ? 0 : desc.texture.depth_or_layers;
         ci.levels = buf ? 1 : desc.texture.levels;
         ci.samples = buf ? 1 : desc.texture.samples;
         ci.fmt = buf ? 0 : static_cast<int>(desc.texture.format);
         ci.usage = static_cast<uint32_t>(desc.usage);
         ci.heap = static_cast<int>(desc.heap);
         ci.flags = static_cast<uint32_t>(desc.flags);
         ci.init = initial_data != nullptr;
      }
      return false;
   }

   // Logs one create that took longer than kSlowCreateMs. caller_rva = first BrutalLegend.exe frame (0 if none).
   static void LogSlowCreate(const CreateInfo& ci, double ms, uint32_t rva)
   {
      g_frame_slow_n.fetch_add(1, std::memory_order_relaxed);
      const DWORD tid = GetCurrentThreadId();
      const bool render = tid == g_render_tid.load(std::memory_order_relaxed);
      Log(reshade::log::level::info,
          "[BL Prof] slow create %.2fms tid=%u rt=%d type=%d %ux%ux%u levels=%u fmt=%d usage=0x%X heap=%d flags=0x%X init=%d caller_rva=0x%X",
          ms, static_cast<unsigned>(tid), render ? 1 : 0, ci.type,
          static_cast<unsigned>(ci.w), static_cast<unsigned>(ci.h), static_cast<unsigned>(ci.d),
          static_cast<unsigned>(ci.levels), ci.fmt, static_cast<unsigned>(ci.usage), ci.heap,
          static_cast<unsigned>(ci.flags), ci.init ? 1 : 0, static_cast<unsigned>(rva));
   }

   static void OnInitResource(reshade::api::device* device, const reshade::api::resource_desc& desc, const reshade::api::subresource_data* initial_data, reshade::api::resource_usage initial_state, reshade::api::resource resource)
   {
      (void)device; (void)desc; (void)initial_data; (void)initial_state; (void)resource;
      const int64_t t0 = t_create_qpc;
      t_create_qpc = 0;
      if (!g_frame_profile)
         return;
      const int64_t dt = t0 ? QpcNow() - t0 : 0;
      ResAcc& a = ResCat();
      a.create_n.fetch_add(1, std::memory_order_relaxed);
      if (t0)
         a.create_ticks.fetch_add(dt, std::memory_order_relaxed);
      g_frame_create_n.fetch_add(1, std::memory_order_relaxed);
      g_frame_create_ticks.fetch_add(dt, std::memory_order_relaxed);
      const double ms = t0 ? TicksToMs(dt) : 0.0;
      if (t0 && ms >= kRecordCreateMs)
      {
         const uint32_t rva = CallerRva();
         RecordCreate(t_create_info, ms, rva);
         if (ms > kSlowCreateMs)
            LogSlowCreate(t_create_info, ms, rva);
      }
   }

   static bool OnUpdateBufferRegion(reshade::api::device* device, const void* data, reshade::api::resource dest, uint64_t dest_offset, uint64_t size)
   {
      (void)device; (void)data; (void)dest; (void)dest_offset;
      if (g_frame_profile)
      {
         ResAcc& a = ResCat();
         a.upload_n.fetch_add(1, std::memory_order_relaxed);
         a.upload_bytes.fetch_add(KnownBytes(size), std::memory_order_relaxed);
      }
      return false;
   }

   static bool OnUpdateTextureRegion(reshade::api::device* device, const reshade::api::subresource_data& data, reshade::api::resource dest, uint32_t dest_subresource, const reshade::api::subresource_box* dest_box)
   {
      (void)device; (void)dest; (void)dest_subresource;
      if (g_frame_profile)
      {
         ResAcc& a = ResCat();
         a.upload_n.fetch_add(1, std::memory_order_relaxed);
         a.upload_bytes.fetch_add(BoxBytes(data.row_pitch, dest_box), std::memory_order_relaxed);
      }
      return false;
   }

   static void PushMap(uint64_t handle, uint32_t sub, uint64_t bytes)
   {
      std::lock_guard<std::mutex> lock(g_map_mutex);
      for (PendingMap& m : g_maps)
      {
         if (!m.active)
         {
            m = { handle, sub, GetCurrentThreadId(), QpcNow(), bytes, true };
            return;
         }
      }
   }

   static void PopMap(uint64_t handle, uint32_t sub)
   {
      const DWORD tid = GetCurrentThreadId();
      const int64_t now = QpcNow();
      int64_t t0 = 0;
      uint64_t bytes = 0;
      bool found = false;
      {
         std::lock_guard<std::mutex> lock(g_map_mutex);
         for (PendingMap& m : g_maps)
         {
            if (m.active && m.handle == handle && m.sub == sub && m.tid == tid)
            {
               t0 = m.t0;
               bytes = m.bytes;
               m.active = false;
               found = true;
               break;
            }
         }
      }
      if (!found)
         return;
      ResAcc& a = ResCat();
      a.upload_n.fetch_add(1, std::memory_order_relaxed);
      a.upload_ticks.fetch_add(now - t0, std::memory_order_relaxed);
      a.upload_bytes.fetch_add(bytes, std::memory_order_relaxed);
   }

   static void OnMapBufferRegion(reshade::api::device* device, reshade::api::resource resource, uint64_t offset, uint64_t size, reshade::api::map_access access, void** data)
   {
      (void)offset; (void)access; (void)data;
      if (g_frame_profile && device)
      {
         const reshade::api::resource_desc desc = device->get_resource_desc(resource);
         // Size unknown or bogus: fall back to the buffer size from its desc.
         uint64_t bytes = KnownBytes(size);
         if (!bytes)
            bytes = KnownBytes(desc.buffer.size);
         const uint32_t usage = static_cast<uint32_t>(desc.usage);
         if (usage & static_cast<uint32_t>(reshade::api::resource_usage::constant_buffer))
            g_fc.map_cb.fetch_add(1, std::memory_order_relaxed);
         else if (usage & static_cast<uint32_t>(reshade::api::resource_usage::vertex_buffer))
            g_fc.map_vb.fetch_add(1, std::memory_order_relaxed);
         else if (usage & static_cast<uint32_t>(reshade::api::resource_usage::index_buffer))
            g_fc.map_ib.fetch_add(1, std::memory_order_relaxed);
         else
            g_fc.map_other.fetch_add(1, std::memory_order_relaxed);
         g_fc.map_bytes.fetch_add(bytes, std::memory_order_relaxed);
         PushMap(resource.handle, 0, bytes);
      }
   }

   static void OnUnmapBufferRegion(reshade::api::device* device, reshade::api::resource resource)
   {
      (void)device;
      if (g_frame_profile)
         PopMap(resource.handle, 0);
   }

   static void OnMapTextureRegion(reshade::api::device* device, reshade::api::resource resource, uint32_t subresource, const reshade::api::subresource_box* box, reshade::api::map_access access, reshade::api::subresource_data* data)
   {
      (void)device; (void)access;
      if (g_frame_profile)
      {
         const uint64_t bytes = BoxBytes(data ? data->row_pitch : 0, box);
         g_fc.map_tex.fetch_add(1, std::memory_order_relaxed);
         g_fc.map_bytes.fetch_add(bytes, std::memory_order_relaxed);
         PushMap(resource.handle, subresource, bytes);
      }
   }

   static void OnUnmapTextureRegion(reshade::api::device* device, reshade::api::resource resource, uint32_t subresource)
   {
      (void)device;
      if (g_frame_profile)
         PopMap(resource.handle, subresource);
   }

   // ---- Extended event counters (DEVELOPMENT, frame.profile only). Each callback bumps one atomic and returns false
   // (or returns void for the void events), so ReShade never cancels the call.
   static bool OnDraw(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance)
   {
      (void)cmd_list; (void)vertex_count; (void)instance_count; (void)first_vertex; (void)first_instance;
      if (g_frame_profile)
         g_fc.draws.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance)
   {
      (void)cmd_list; (void)index_count; (void)instance_count; (void)first_index; (void)vertex_offset; (void)first_instance;
      if (g_frame_profile)
         g_fc.draws.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static bool OnDrawIndirect(reshade::api::command_list* cmd_list, reshade::api::indirect_command type, reshade::api::resource buffer, uint64_t offset, uint32_t draw_count, uint32_t stride)
   {
      (void)cmd_list; (void)type; (void)buffer; (void)offset; (void)draw_count; (void)stride;
      if (g_frame_profile)
         g_fc.draws.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static bool OnDispatch(reshade::api::command_list* cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
   {
      (void)cmd_list; (void)group_count_x; (void)group_count_y; (void)group_count_z;
      if (g_frame_profile)
         g_fc.dispatches.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static void OnBindPipeline(reshade::api::command_list* cmd_list, reshade::api::pipeline_stage stages, reshade::api::pipeline pipeline)
   {
      (void)cmd_list; (void)stages; (void)pipeline;
      if (g_frame_profile)
         g_fc.binds.fetch_add(1, std::memory_order_relaxed);
   }

   static void OnPushDescriptors(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t layout_param, const reshade::api::descriptor_table_update& update)
   {
      (void)cmd_list; (void)stages; (void)layout; (void)layout_param; (void)update;
      if (g_frame_profile)
         g_fc.desc_binds.fetch_add(1, std::memory_order_relaxed);
   }

   static void OnBindDescriptorTables(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t first, uint32_t count, const reshade::api::descriptor_table* tables, uint32_t dynamic_offset_count, const uint32_t* dynamic_offsets)
   {
      (void)cmd_list; (void)stages; (void)layout; (void)first; (void)count; (void)tables; (void)dynamic_offset_count; (void)dynamic_offsets;
      if (g_frame_profile)
         g_fc.desc_binds.fetch_add(1, std::memory_order_relaxed);
   }

   static void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count, const reshade::api::resource_view* rtvs, reshade::api::resource_view dsv)
   {
      (void)cmd_list; (void)count; (void)rtvs; (void)dsv;
      if (g_frame_profile)
         g_fc.rt_binds.fetch_add(1, std::memory_order_relaxed);
   }

   static bool OnClearDepthStencil(reshade::api::command_list* cmd_list, reshade::api::resource_view dsv, const float* depth, const uint8_t* stencil, uint32_t rect_count, const reshade::api::rect* rects)
   {
      (void)cmd_list; (void)dsv; (void)depth; (void)stencil; (void)rect_count; (void)rects;
      if (g_frame_profile)
         g_fc.clears.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static bool OnClearRenderTarget(reshade::api::command_list* cmd_list, reshade::api::resource_view rtv, const float color[4], uint32_t rect_count, const reshade::api::rect* rects)
   {
      (void)cmd_list; (void)rtv; (void)color; (void)rect_count; (void)rects;
      if (g_frame_profile)
         g_fc.clears.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static bool OnClearUavUint(reshade::api::command_list* cmd_list, reshade::api::resource_view uav, const uint32_t values[4], uint32_t rect_count, const reshade::api::rect* rects)
   {
      (void)cmd_list; (void)uav; (void)values; (void)rect_count; (void)rects;
      if (g_frame_profile)
         g_fc.clears.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   static bool OnClearUavFloat(reshade::api::command_list* cmd_list, reshade::api::resource_view uav, const float values[4], uint32_t rect_count, const reshade::api::rect* rects)
   {
      (void)cmd_list; (void)uav; (void)values; (void)rect_count; (void)rects;
      if (g_frame_profile)
         g_fc.clears.fetch_add(1, std::memory_order_relaxed);
      return false;
   }

   // One [BL Spike] line: every column of the frame, the 3 previous frames' dt/draws/maps, then the creations of the frame.
   static void LogSpike(const Row& r, double median)
   {
      std::string s;
      Out(s, "[BL Spike] frame dt=%.2fms median=%.2fms ratio=%.2f ", r.present_dt_ms, median, r.present_dt_ms / median);
      AppendRowKV(s, r);
      for (uint32_t k = 1; k <= 3; ++k)
      {
         if (g_hist_n < k)
            break;
         const Row& p = g_hist[(g_hist_n - k) % 3];
         Out(s, " | prev%u dt=%.2f draws=%u maps=%llu", k, p.present_dt_ms, static_cast<unsigned>(p.draws),
             static_cast<unsigned long long>(MapsOf(p)));
      }
      CreateRec snap[kFrameCreates] = {};
      uint32_t n = 0;
      {
         std::lock_guard<std::mutex> lock(g_create_mutex);
         n = g_creates_n;
         for (uint32_t i = 0; i < n; ++i)
            snap[i] = g_creates[i];
      }
      Out(s, " | creates=%u:", static_cast<unsigned>(n));
      for (uint32_t i = 0; i < n; ++i)
         Out(s, " [%.2fms type=%d %ux%u fmt=%u usage=0x%X rva=0x%X]", snap[i].ms, snap[i].type, static_cast<unsigned>(snap[i].w),
             static_cast<unsigned>(snap[i].h), static_cast<unsigned>(snap[i].fmt), static_cast<unsigned>(snap[i].usage),
             static_cast<unsigned>(snap[i].rva));
      LogBig(reshade::log::level::info, s);
   }

   // One [BL Summary] line over the last kSummaryEvery presents.
   static void LogSummary()
   {
      const size_t n = g_win.size();
      if (!n)
         return;
      std::vector<double> dts, draws, maps, cbs;
      dts.reserve(n); draws.reserve(n); maps.reserve(n); cbs.reserve(n);
      double sum_d = 0.0, sum_m = 0.0, sum_c = 0.0, sum_r = 0.0;
      uint32_t spikes = 0;
      for (const WinSample& w : g_win)
      {
         dts.push_back(w.dt);
         draws.push_back(w.draws);
         maps.push_back(w.maps);
         cbs.push_back(w.cb);
         sum_d += w.draws;
         sum_m += w.maps;
         sum_c += w.cb;
         sum_r += w.rt_cpu;
         if (w.spike)
            ++spikes;
      }
      std::vector<double> sorted = dts;
      std::sort(sorted.begin(), sorted.end());
      const auto pct = [&sorted, n](double p) { return sorted[static_cast<size_t>(p * static_cast<double>(n - 1))]; };
      const double inv = 1.0 / static_cast<double>(n);
      const double fit_maps = LsSlope(maps, dts) * 1000.0;
      const double fit_draws = LsSlope(draws, dts) * 1000.0;
      std::string s;
      Out(s, "[BL Summary] presents=%u dt p50=%.2f p99=%.2f p99.9=%.2f max=%.2f spikes=%u mean_draws=%.1f mean_maps=%.1f mean_cb_maps=%.1f mean_rt_cpu_ms=%.2f fit_ms_per_1000_maps=%.3f fit_ms_per_1000_draws=%.3f",
          static_cast<unsigned>(n), pct(0.5), pct(0.99), pct(0.999), sorted[n - 1], static_cast<unsigned>(spikes),
          sum_d * inv, sum_m * inv, sum_c * inv, sum_r * inv, fit_maps, fit_draws);
      LogBig(reshade::log::level::info, s);
   }

   // Per-frame counters and the creation list. Also called while the toggle is off.
   static void ResetFrameState()
   {
      g_fc.draws.store(0, std::memory_order_relaxed);
      g_fc.dispatches.store(0, std::memory_order_relaxed);
      g_fc.binds.store(0, std::memory_order_relaxed);
      g_fc.desc_binds.store(0, std::memory_order_relaxed);
      g_fc.rt_binds.store(0, std::memory_order_relaxed);
      g_fc.clears.store(0, std::memory_order_relaxed);
      g_fc.map_cb.store(0, std::memory_order_relaxed);
      g_fc.map_vb.store(0, std::memory_order_relaxed);
      g_fc.map_ib.store(0, std::memory_order_relaxed);
      g_fc.map_other.store(0, std::memory_order_relaxed);
      g_fc.map_tex.store(0, std::memory_order_relaxed);
      g_fc.map_bytes.store(0, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(g_create_mutex);
      g_creates_n = 0;
   }

   static void ResetResAcc()
   {
      ResetFrameState();
      for (ResAcc& a : g_res)
      {
         a.create_n.store(0, std::memory_order_relaxed);
         a.create_ticks.store(0, std::memory_order_relaxed);
         a.upload_n.store(0, std::memory_order_relaxed);
         a.upload_ticks.store(0, std::memory_order_relaxed);
         a.upload_bytes.store(0, std::memory_order_relaxed);
      }
      g_frame_create_n.store(0, std::memory_order_relaxed);
      g_frame_create_ticks.store(0, std::memory_order_relaxed);
      g_frame_slow_n.store(0, std::memory_order_relaxed);
      std::lock_guard<std::mutex> lock(g_map_mutex);
      for (PendingMap& m : g_maps)
         m.active = false;
   }

   static void Install()
   {
      if (g_installed)
         return;
      QueryPerformanceFrequency(&g_qpf);
      g_get_thread_description = reinterpret_cast<PFN_GetThreadDescription>(
         GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));

      if (const auto* exe = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr)))
      {
         const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(exe + reinterpret_cast<const IMAGE_DOS_HEADER*>(exe)->e_lfanew);
         g_exe_base = reinterpret_cast<uintptr_t>(exe);
         g_exe_size = nt->OptionalHeader.SizeOfImage;
      }

      const int hooked = InstallIatHooks();
      if (hooked < 0)
      {
         Log(reshade::log::level::error, "[BL Frame] could not read the import table, frame profiler inactive");
         return;
      }
      if (hooked < A_Count)
         Log(reshade::log::level::warning, "[BL Frame] hooked %d of %d KERNEL32 imports", hooked, static_cast<int>(A_Count));
      else
         Log(reshade::log::level::info, "[BL Frame] frame profiler hooks installed (%d imports)", hooked);
      g_installed = true;
   }
} // namespace BLFrameProfiler

// Called at the top of the sim AdvanceAccum hook (sim_tick.h): that is the main thread.
static void FrameProfiler_MarkMainThread()
{
   BLFrameProfiler::g_main_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
}

// Called from OnPresent (main.cpp): the render thread. Builds one CSV row per present while the toggle is on.
static void FrameProfiler_OnPresent()
{
   using namespace BLFrameProfiler;

   g_render_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
   if (!g_frame_profile)
   {
      ResetAllSlots();
      g_start_qpc = 0;
      g_last_present_qpc = 0;
      g_pending_present_qpc = 0;
      g_presents_since_report = 0;
      {
         std::lock_guard<std::mutex> lock(g_attr_mutex);
         g_wait_n = 0;
         g_rwait_n = 0;
      }
      ResetResAcc();
      g_was_on = false;
      g_prev_valid = false;
      return;
   }
   if (!g_was_on)
   {
      // Session start: reset the rolling state, and let finish_present log the session header.
      g_was_on = true;
      g_session_pending = true;
      g_dt_n = 0;
      g_hist_n = 0;
      g_win.clear();
      g_win.reserve(kSummaryEvery);
      g_prev_valid = false;
   }
   if (g_qpf.QuadPart == 0)
      QueryPerformanceFrequency(&g_qpf);

   const int64_t now = QpcNow();
   if (g_start_qpc == 0)
      g_start_qpc = now;
   g_pending_present_qpc = now;

   Row row = {};
   row.time_ms = TicksToMs(now - g_start_qpc);
   row.present_dt_ms = g_last_present_qpc ? TicksToMs(now - g_last_present_qpc) : 0.0;
   g_last_present_qpc = now;

   const DWORD main_tid = g_main_tid.load(std::memory_order_relaxed);
   const DWORD render_tid = g_render_tid.load(std::memory_order_relaxed);

   for (ThreadSlot& s : g_slots)
   {
      const DWORD tid = s.tid.load(std::memory_order_acquire);
      if (!tid)
         continue;

      int64_t t[G_Count];
      for (int g = 0; g < G_Count; ++g)
      {
         t[g] = s.ticks[g].exchange(0, std::memory_order_relaxed);
         g_window_calls[g] += s.calls[g].exchange(0, std::memory_order_relaxed);
      }
      const uint64_t bytes = s.bytes.exchange(0, std::memory_order_relaxed);

      if (tid == main_tid)
      {
         for (int g = 0; g < G_Count; ++g)
            row.main_ms[g] = TicksToMs(t[g]);
      }
      if (tid == render_tid)
      {
         for (int g = 0; g < G_Count; ++g)
            row.render_ms[g] = TicksToMs(t[g]);
      }
      if (tid != main_tid && tid != render_tid)
      {
         row.other_io_bytes += bytes;
         row.other_io_ms += TicksToMs(t[G_IO]);
         const double blocked = TicksToMs(t[G_Sleep] + t[G_Wait] + t[G_IO]);
         if (blocked > row.busiest_ms)
         {
            row.busiest_ms = blocked;
            row.busiest_tid = tid;
         }
      }
   }

   {
      ResAcc& rt = g_res[0];
      ResAcc& ot = g_res[1];
      row.rt_create_n = rt.create_n.exchange(0, std::memory_order_relaxed);
      row.rt_create_ms = TicksToMs(rt.create_ticks.exchange(0, std::memory_order_relaxed));
      row.rt_upload_n = rt.upload_n.exchange(0, std::memory_order_relaxed);
      row.rt_upload_ms = TicksToMs(rt.upload_ticks.exchange(0, std::memory_order_relaxed));
      row.rt_upload_bytes = rt.upload_bytes.exchange(0, std::memory_order_relaxed);
      row.oth_create_n = ot.create_n.exchange(0, std::memory_order_relaxed);
      row.oth_create_ms = TicksToMs(ot.create_ticks.exchange(0, std::memory_order_relaxed));
      row.oth_upload_n = ot.upload_n.exchange(0, std::memory_order_relaxed);
      ot.upload_ticks.store(0, std::memory_order_relaxed);
      ot.upload_bytes.store(0, std::memory_order_relaxed);
   }

   // Extended columns: event counters, CPU deltas (render, main, process), sim and frame-pacing deltas.
   {
      row.draws = g_fc.draws.exchange(0, std::memory_order_relaxed);
      row.dispatches = g_fc.dispatches.exchange(0, std::memory_order_relaxed);
      row.binds = g_fc.binds.exchange(0, std::memory_order_relaxed);
      row.desc_binds = g_fc.desc_binds.exchange(0, std::memory_order_relaxed);
      row.rt_binds = g_fc.rt_binds.exchange(0, std::memory_order_relaxed);
      row.clears = g_fc.clears.exchange(0, std::memory_order_relaxed);
      row.map_cb_n = g_fc.map_cb.exchange(0, std::memory_order_relaxed);
      row.map_vb_n = g_fc.map_vb.exchange(0, std::memory_order_relaxed);
      row.map_ib_n = g_fc.map_ib.exchange(0, std::memory_order_relaxed);
      row.map_other_n = g_fc.map_other.exchange(0, std::memory_order_relaxed);
      row.map_tex_n = g_fc.map_tex.exchange(0, std::memory_order_relaxed);
      row.map_bytes = g_fc.map_bytes.exchange(0, std::memory_order_relaxed);

      const double cpu_now[3] = { ThreadCpuTotalMs(render_tid), ThreadCpuTotalMs(main_tid), ProcessCpuTotalMs() };
      double* const cpu_out[3] = { &row.rt_cpu_ms, &row.main_cpu_ms, &row.proc_cpu_ms };
      for (int i = 0; i < 3; ++i)
      {
         if (cpu_now[i] <= 0.0)
            continue; // unreadable this frame: keep the previous total
         if (g_prev_valid && cpu_now[i] >= g_prev_cpu[i])
            *cpu_out[i] = cpu_now[i] - g_prev_cpu[i];
         g_prev_cpu[i] = cpu_now[i];
      }

      if (g_ext.sim_ticks)
      {
         const uint32_t cur = *g_ext.sim_ticks;
         row.sim_ticks = g_prev_valid ? cur - g_prev_sim_ticks : 0;
         g_prev_sim_ticks = cur;
      }
      if (g_ext.sim_dt)
         row.sim_dt_ms = static_cast<double>(g_ext.sim_dt->load(std::memory_order_relaxed)) * 1000.0;

      const uint32_t* const fp_src[3] = { g_ext.fp_hold, g_ext.fp_under, g_ext.fp_passthrough };
      uint32_t fp_out[3] = {};
      for (int i = 0; i < 3; ++i)
      {
         if (!fp_src[i])
            continue;
         const uint32_t cur = *fp_src[i];
         fp_out[i] = g_prev_valid ? cur - g_prev_fp[i] : 0;
         g_prev_fp[i] = cur;
      }
      row.fp_hold = fp_out[0];
      row.fp_under = fp_out[1];
      row.fp_passthrough = fp_out[2];
      g_prev_valid = true;
   }

   // Frame creation summary, only when this frame had a slow create.
   {
      const uint32_t slow = g_frame_slow_n.exchange(0, std::memory_order_relaxed);
      const uint32_t fn = g_frame_create_n.exchange(0, std::memory_order_relaxed);
      const int64_t fticks = g_frame_create_ticks.exchange(0, std::memory_order_relaxed);
      if (slow)
         Log(reshade::log::level::info, "[BL Prof] frame creates n=%u ms=%.2f slow=%u",
             static_cast<unsigned>(fn), TicksToMs(fticks), static_cast<unsigned>(slow));
   }

   // Spike check against the rolling median of the previous frames, then window and history bookkeeping.
   {
      bool spike = false;
      if (row.present_dt_ms > 0.0)
      {
         const uint32_t cnt = g_dt_n < kMedianWin ? g_dt_n : kMedianWin;
         if (cnt >= 30)
         {
            double tmp[kMedianWin] = {};
            for (uint32_t i = 0; i < cnt; ++i)
               tmp[i] = g_dt_ring[i];
            std::nth_element(tmp, tmp + cnt / 2, tmp + cnt);
            const double median = tmp[cnt / 2];
            if (median > 0.0 && row.present_dt_ms > kSpikeRatio * median && row.present_dt_ms < 2000.0)
            {
               spike = true;
               LogSpike(row, median);
            }
         }
         g_dt_ring[g_dt_n % kMedianWin] = row.present_dt_ms;
         ++g_dt_n;
         g_win.push_back({ row.present_dt_ms, static_cast<double>(row.draws), static_cast<double>(MapsOf(row)),
                           static_cast<double>(row.map_cb_n), row.rt_cpu_ms, spike });
         if (g_win.size() >= kSummaryEvery)
         {
            LogSummary();
            g_win.clear();
         }
      }
      {
         std::lock_guard<std::mutex> lock(g_create_mutex);
         g_creates_n = 0;
      }
      g_hist[g_hist_n % 3] = row;
      ++g_hist_n;
   }

   // Flush before pushing: the newest row stays unflushed until its finish_present has filled present_call_ms.
   if (g_rows.size() >= kRowsPerFlush)
      FlushRows();
   g_rows.push_back(row);

   if (++g_presents_since_report >= kReportEvery)
   {
      g_presents_since_report = 0;
      ReportWaits();
   }
}

// Registered for addon_event::finish_present (main.cpp). Completes the row started by FrameProfiler_OnPresent.
static void FrameProfiler_OnFinishPresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain)
{
   using namespace BLFrameProfiler;
   (void)queue;
   if (g_session_pending && g_frame_profile && swapchain)
   {
      g_session_pending = false;
      unsigned w = 0, h = 0;
      reshade::api::device* const dev = swapchain->get_device();
      if (dev)
      {
         const reshade::api::resource_desc d = dev->get_resource_desc(swapchain->get_back_buffer(0));
         w = d.texture.width;
         h = d.texture.height;
      }
      Log(reshade::log::level::info, "[BL Prof] session res=%ux%u sim60=%d framepacing=%d", w, h,
          (g_ext.sim60 && *g_ext.sim60) ? 1 : 0, (g_ext.pacing && *g_ext.pacing) ? 1 : 0);
   }
   if (!g_frame_profile || !g_pending_present_qpc || g_rows.empty())
      return;
   Row& row = g_rows.back();
   row.present_call_ms = TicksToMs(QpcNow() - g_pending_present_qpc);
   row.present_tid = GetCurrentThreadId();
   g_pending_present_qpc = 0;
}

// Called from OnInit (main.cpp), after InstallSimTick.
static void InstallFrameProfiler()
{
   BLFrameProfiler::Install();
}
#else
static void FrameProfiler_MarkMainThread() {}
static void FrameProfiler_OnPresent() {}
static void InstallFrameProfiler() {}
#endif
