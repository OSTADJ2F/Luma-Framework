#pragma once

// d3d9_cs_watchdog.h - dev-only gap watchdog for the D3D9 CS worker (Brutal Legend).
// While the owner (render) thread has not Presented for 500 ms, a watchdog thread samples the owner and
// the CS worker every 50 ms: suspend, copy Eip/Esp and 32 stack dwords, resume. Nothing is resolved,
// logged or allocated while a thread is suspended. When the gap ends (next Present) it logs the summary.
// Included by d3d9_cs_dispatch.h. Real code only in DEVELOPMENT builds on _M_IX86; everything else is a stub.

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "d3d9_cs_tables.h"

#if DEVELOPMENT && defined(_M_IX86)
namespace cs_watchdog_detail {

constexpr DWORD kPeriodMs = 50;
constexpr double kGapThresholdMs = 500.0;
constexpr uint32_t kMaxSamples = 512;
constexpr uint32_t kStackDwords = 256;
constexpr uint32_t kCandidates = 10;
constexpr uint32_t kExtraThreads = 4;            // CsWatchdogAddThread slots
constexpr uint32_t kThreads = 2 + kExtraThreads; // [0] owner, [1] worker, [2..] extra threads

struct Sample {
    DWORD eip[kThreads];                   // 0 = not sampled
    DWORD stack[kThreads][kStackDwords];   // raw dwords from Esp; filtered to module addresses at log time
};

inline std::atomic<DWORD> g_present_seq{0};          // bumped by every owner Present
inline std::atomic<LONGLONG> g_last_present_qpc{0};  // QPC of the last owner Present (0 = none yet)
inline const std::atomic<DWORD>* g_owner_tid = nullptr;
inline std::atomic<DWORD> g_worker_tid{0};
inline HANDLE g_thread = nullptr;
inline HANDLE g_stop_event = nullptr;
inline Sample g_samples[kMaxSamples];  // written by the watchdog thread only
inline uint32_t g_sample_count = 0;    // samples in the current gap (may exceed kMaxSamples)

struct ExtraThread {
    std::atomic<DWORD> tid{0};  // 0 = free; written last by CsWatchdogAddThread
    char name[32] = {};         // written before tid
};
inline ExtraThread g_extra[kExtraThreads];

inline LONGLONG Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline LONGLONG QpcFreq()
{
    static const LONGLONG freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return freq;
}

inline double QpcToMs(LONGLONG d)
{
    return static_cast<double>(d) * 1000.0 / static_cast<double>(QpcFreq());
}

// Suspends tid just long enough to copy its Eip and stack. Returns false if the thread could not be opened.
inline bool SampleThread(DWORD tid, DWORD& eip, DWORD (&stack)[kStackDwords])
{
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h)
        return false;

    std::memset(stack, 0, sizeof(DWORD) * kStackDwords);
    bool ok = false;
    if (SuspendThread(h) != static_cast<DWORD>(-1))
    {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(h, &ctx))
        {
            eip = ctx.Eip;
            SIZE_T got = 0;
            // Own process, raw copy only. Unreadable stack leaves zeros.
            ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ctx.Esp)),
                              stack, sizeof(DWORD) * kStackDwords, &got);
            ok = true;
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    return ok;
}

// "module.exe+0xoffset" for an address inside a loaded module image. False otherwise.
inline bool ModuleOffset(DWORD addr, std::string& out)
{
    HMODULE mod = nullptr;
    if (!addr || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &mod) ||
        !mod)
        return false;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(mod, path, MAX_PATH);
    std::wstring full(path);
    const size_t slash = full.find_last_of(L"\\/");
    const std::wstring wname = (slash == std::wstring::npos) ? full : full.substr(slash + 1);
    std::string name;
    for (wchar_t c : wname)
        name.push_back(c < 128 ? static_cast<char>(c) : '?');

    const uintptr_t off = static_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod);
    out = std::format("{}+0x{:X}", name, off);
    return true;
}

inline std::string LocationOf(DWORD addr)
{
    std::string loc;
    if (!ModuleOffset(addr, loc))
        loc = std::format("unknown+0x{:08X}", addr);
    return loc;
}

inline void Log(const std::string& msg)
{
    if (g_cs_log)
        g_cs_log(0, msg.c_str());
}

// Watchdog thread only. Summarizes the samples of one gap.
inline void ReportGap(LONGLONG gap_start, LONGLONG gap_end)
{
    const uint32_t stored = (std::min)(g_sample_count, kMaxSamples);
    Log(std::format("[BL CS] gap {:.1f} ms, {} samples", QpcToMs(gap_end - gap_start), g_sample_count));

    std::string who[kThreads];
    who[0] = "owner";
    who[1] = "worker";
    for (uint32_t e = 0; e < kExtraThreads; ++e)
        if (g_extra[e].tid.load(std::memory_order_acquire))
            who[2 + e] = g_extra[e].name;
    for (uint32_t t = 0; t < kThreads; ++t)
    {
        if (who[t].empty())
            continue;
        std::map<DWORD, uint32_t> counts;
        for (uint32_t i = 0; i < stored; ++i)
        {
            if (g_samples[i].eip[t])
                counts[g_samples[i].eip[t]]++;
        }

        std::vector<std::pair<DWORD, uint32_t>> top(counts.begin(), counts.end());
        std::stable_sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });

        std::string line = std::format("[BL CS] {} eip:", who[t]);
        if (top.empty())
            line += " none";
        for (size_t k = 0; k < top.size() && k < 3; ++k)
            line += std::format("{} {} ({})", k ? "," : "", LocationOf(top[k].first), top[k].second);
        Log(line);

        if (top.empty())
            continue;

        // Return candidates from the first sample at the most frequent Eip.
        uint32_t idx = 0;
        while (idx < stored && g_samples[idx].eip[t] != top[0].first)
            ++idx;

        std::string cands;
        uint32_t n = 0;
        for (uint32_t j = 0; j < kStackDwords && n < kCandidates && idx < stored; ++j)
        {
            std::string loc;
            if (ModuleOffset(g_samples[idx].stack[t][j], loc))
            {
                cands += std::format("{}{}", n ? ", " : "", loc);
                ++n;
            }
        }
        Log(std::format("[BL CS] {} top stack: {}", who[t], n ? cands : std::string("none")));
    }
}

inline DWORD WINAPI WatchdogMain(LPVOID)
{
    using namespace cs_watchdog_detail;
    bool in_gap = false;
    LONGLONG gap_start = 0;
    DWORD seen_seq = g_present_seq.load(std::memory_order_seq_cst);

    while (WaitForSingleObject(g_stop_event, kPeriodMs) == WAIT_TIMEOUT)
    {
        // Read seq before qpc: Present writes qpc first, then bumps seq.
        const DWORD seq = g_present_seq.load(std::memory_order_seq_cst);
        const LONGLONG last = g_last_present_qpc.load(std::memory_order_seq_cst);

        if (seq != seen_seq)
        {
            seen_seq = seq;
            if (in_gap)
            {
                ReportGap(gap_start, last);
                in_gap = false;
            }
        }

        if (last == 0 || QpcToMs(Qpc() - last) <= kGapThresholdMs)
            continue;

        if (!in_gap)
        {
            in_gap = true;
            gap_start = last;
            g_sample_count = 0;
        }

        const DWORD owner = g_owner_tid ? g_owner_tid->load(std::memory_order_relaxed) : 0;
        const DWORD worker = g_worker_tid.load(std::memory_order_relaxed);
        if (owner == 0 || worker == 0)
            continue;

        Sample s{};
        SampleThread(owner, s.eip[0], s.stack[0]);
        SampleThread(worker, s.eip[1], s.stack[1]);
        for (uint32_t e = 0; e < kExtraThreads; ++e)
        {
            const DWORD extra = g_extra[e].tid.load(std::memory_order_acquire);
            if (extra)
                SampleThread(extra, s.eip[2 + e], s.stack[2 + e]);
        }
        if (g_sample_count < kMaxSamples)
            g_samples[g_sample_count] = s;
        ++g_sample_count;
    }
    return 0;
}

} // namespace cs_watchdog_detail

// Owner thread, every Present. Cheap: two atomics.
inline void CsWatchdogNotePresent()
{
    cs_watchdog_detail::g_last_present_qpc.store(cs_watchdog_detail::Qpc(), std::memory_order_seq_cst);
    cs_watchdog_detail::g_present_seq.fetch_add(1, std::memory_order_seq_cst);
}

// Starts the watchdog thread (idempotent). Called when the CS worker is running; updates the worker id.
inline void CsWatchdogStart(const std::atomic<DWORD>* owner_tid, DWORD worker_tid)
{
    using namespace cs_watchdog_detail;
    g_owner_tid = owner_tid;
    g_worker_tid.store(worker_tid, std::memory_order_relaxed);
    if (g_thread)
        return;

    g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!g_stop_event)
        return;
    g_thread = CreateThread(nullptr, 0, WatchdogMain, nullptr, 0, nullptr);
    if (!g_thread)
    {
        CloseHandle(g_stop_event);
        g_stop_event = nullptr;
    }
}

// Adds a thread (up to kExtraThreads) to the gap sampling, labelled name. Idempotent per tid. Call from any thread, not DllMain.
inline void CsWatchdogAddThread(DWORD tid, const char* name)
{
    using namespace cs_watchdog_detail;
    if (!tid)
        return;
    for (auto& e : g_extra)
        if (e.tid.load(std::memory_order_acquire) == tid)
            return;
    for (auto& e : g_extra)
    {
        if (e.tid.load(std::memory_order_acquire) == 0)
        {
            std::strncpy(e.name, name ? name : "extra", sizeof(e.name) - 1);
            e.name[sizeof(e.name) - 1] = 0;
            e.tid.store(tid, std::memory_order_release);
            return;
        }
    }
}

// Stops and joins the watchdog thread. Owner thread only, never from DllMain.
inline void CsWatchdogStop()
{
    using namespace cs_watchdog_detail;
    if (!g_thread)
        return;
    SetEvent(g_stop_event);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    CloseHandle(g_stop_event);
    g_thread = nullptr;
    g_stop_event = nullptr;
}

#else

inline void CsWatchdogNotePresent() {}
inline void CsWatchdogStart(const std::atomic<DWORD>* /*owner_tid*/, DWORD /*worker_tid*/) {}
inline void CsWatchdogStop() {}
inline void CsWatchdogAddThread(DWORD /*tid*/, const char* /*name*/) {}

#endif
