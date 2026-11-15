// d3d9_cs_queue.h - single-producer byte ring and worker thread for the D3D9 command stream.
// Self-contained: no game names, no addresses, no dependency on other cs headers.
// Producer = owner thread (calls Reserve/Commit/Drain/Stop). Worker runs commands in stream order.
#pragma once

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#pragma comment(lib, "Synchronization.lib")
#pragma comment(lib, "User32.lib")

struct CsCmd { uint16_t op; uint16_t reserved; uint32_t bytes; };  // bytes includes the header, multiple of 8
enum : uint16_t { CsOp_Pad, CsOp_Call, CsOp_CallBlocking, CsOp_Callback, CsOp_BufferUpload, CsOp_Free, CsOp_Stop };
struct CsDone { volatile LONG done; DWORD result; };

// Decision 10: called once when a CsWait passes 5 s. Dev code may set it; default null.
inline void (*g_cs_wait_timeout_cb)(volatile LONG*) = nullptr;

#if DEVELOPMENT
// Dev diagnostics. CsDispatch sets g_cs_diag_cb. event 0 = a thread waited more than 100 ms (kind "call", "mailbox" or "wait";
// slot set for calls, what set for "wait"); event 1 = a worker item took more than 100 ms (slot may be null).
// Event 0 runs on the waiting thread, event 1 on the worker.
inline void (*g_cs_diag_cb)(int event, const char* kind, const char* what, const void* slot, uint64_t ticks) = nullptr;
inline volatile LONG64 g_cs_worker_busy_ticks = 0;           // worker time executing items since the last window read
inline volatile LONG64 g_cs_worker_spin_ticks = 0;           // worker time spinning for work (includes mailbox service while spinning)
inline volatile LONG64 g_cs_worker_sleep_ticks = 0;          // worker time asleep in WaitOnAddress
inline volatile LONG64 g_cs_worker_cmd_start = 0;            // QPC at the start of the current worker item (0 = idle)
inline volatile LONG g_cs_worker_op = 0;                     // CsOp of the current item
inline const void* volatile g_cs_worker_slot = nullptr;      // CsSlot of the current call item (set by CsExecute / CsPollMailbox)
#endif

namespace cs_queue_detail {

#pragma warning(push)
#pragma warning(disable : 4324) // padded for cache-line separation on purpose
struct alignas(64) CsLine { std::atomic<uint32_t> v; };
#pragma warning(pop)

struct CsQueueState {
    CsLine head;       // published write index
    CsLine tail;       // consumed read index, written by the worker
    CsLine sleeping;   // 1 while the worker is (about to be) asleep
    CsLine wake_gen;   // wake generation: the sleeping worker WaitOnAddress()es on it; bumped by every wake
    CsLine quit;       // 1 once CsQueueStop asked the worker to exit (it exits only when the ring is empty)
    uint8_t* ring;     // owner-private from here on
    uint32_t ring_bytes;
    uint32_t reserved; // owner-private: bytes reserved so far (includes uncommitted)
    void (*execute)(const CsCmd*);
    void (*poll)();
    HANDLE thread;
    DWORD tid;
    uint32_t pending_cmds; // owner-private: commands written since the last CsQueueCommit (publish batching)
    CsCmd* coal_cmd;       // owner-private: last command, if the owner may still extend it (CsQueueAfterWrite)
    uint32_t coal_end;     // owner-private: `reserved` right after coal_cmd was written
    bool running;
};

inline CsQueueState g_cs_queue{};

inline uint64_t CsNowTicks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<uint64_t>(t.QuadPart);
}

inline uint64_t CsTicksPerSec() {
    static const uint64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<uint64_t>(f.QuadPart);
    }();
    return freq;
}

inline DWORD WINAPI CsWorkerMain(LPVOID) {
    CsQueueState& s = g_cs_queue;
    SetThreadDescription(GetCurrentThread(), L"BL D3D9 CS");
    const uint32_t mask = s.ring_bytes - 1;
    uint32_t t = s.tail.v.load(std::memory_order_relaxed);
    for (;;) {
        uint32_t h = s.head.v.load(std::memory_order_acquire);
        if (h == t) {
            // Spin about 200 us before sleeping. The mailbox is polled throughout, so foreign calls do not wait for a wake.
            const uint64_t start = CsNowTicks();
            const uint64_t limit = CsTicksPerSec() / 5000;
            do {
                _mm_pause();
                if (s.poll) s.poll();
                h = s.head.v.load(std::memory_order_acquire);
            } while (h == t && CsNowTicks() - start < limit);
#if DEVELOPMENT
            InterlockedExchangeAdd64(&g_cs_worker_spin_ticks, static_cast<LONG64>(CsNowTicks() - start));
#endif
            if (h == t) {
                // Sleep protocol (no lost wake-up). Read the wake generation BEFORE the final checks. Every wake
                // after that read changes the generation, so WaitOnAddress returns at once instead of blocking.
                uint32_t gen = s.wake_gen.v.load(std::memory_order_seq_cst);
                if (s.poll) s.poll();                       // serves mailbox items posted before the read
                h = s.head.v.load(std::memory_order_seq_cst);
                if (h == t && s.quit.v.load(std::memory_order_seq_cst)) return 0;
                if (h == t) {
                    // Dekker pair with CsQueueCommit/CsQueueWake: either they see sleeping == 1 and bump the generation,
                    // or this re-check sees their work.
                    s.sleeping.v.store(1, std::memory_order_seq_cst);
                    h = s.head.v.load(std::memory_order_seq_cst);
                    if (h == t && s.quit.v.load(std::memory_order_seq_cst) == 0) {
#if DEVELOPMENT
                        const uint64_t sleep0 = CsNowTicks();
#endif
                        WaitOnAddress(static_cast<void*>(&s.wake_gen.v), &gen, sizeof(uint32_t), 1); // 1 ms slice as a safety net
#if DEVELOPMENT
                        InterlockedExchangeAdd64(&g_cs_worker_sleep_ticks, static_cast<LONG64>(CsNowTicks() - sleep0));
#endif
                    }
                    s.sleeping.v.store(0, std::memory_order_seq_cst);
                    if (s.poll) s.poll();                   // immediately after every wake
                }
                continue;
            }
        }
        while (t != h) {
            uint8_t* p = s.ring + (t & mask);
            const CsCmd* c = reinterpret_cast<const CsCmd*>(p);
            bool stop = false;
#if DEVELOPMENT
            const uint64_t cstart = CsNowTicks();
            g_cs_worker_op = c->op;
            g_cs_worker_slot = nullptr;
            InterlockedExchange64(&g_cs_worker_cmd_start, static_cast<LONG64>(cstart));
#endif
            switch (c->op) {
            case CsOp_Pad:
                break;
            case CsOp_Callback: {
                void (*fn)(const void*);
                std::memcpy(&fn, p + 8, sizeof(fn));
                fn(p + 8 + sizeof(fn));
                break;
            }
            case CsOp_Free: {
                void* q;
                std::memcpy(&q, p + 8, sizeof(q));
                std::free(q);
                break;
            }
            case CsOp_Stop:
                stop = true;
                break;
            default:
                s.execute(c);
                break;
            }
#if DEVELOPMENT
            {
                const uint64_t dur = CsNowTicks() - cstart;
                InterlockedExchange64(&g_cs_worker_cmd_start, 0);
                InterlockedExchangeAdd64(&g_cs_worker_busy_ticks, static_cast<LONG64>(dur));
                if (g_cs_diag_cb && dur > CsTicksPerSec() / 10)
                    g_cs_diag_cb(1, nullptr, nullptr, g_cs_worker_slot, dur);
            }
#endif
            t += c->bytes;
            s.tail.v.store(t, std::memory_order_release);
            if (stop) return 0;
            if (s.poll) s.poll();
        }
    }
}

} // namespace cs_queue_detail

#if DEVELOPMENT
// Reports a wait that lasted more than 100 ms since t0 (QPC ticks, same clock as CsQpc).
inline void CsNoteLongWait(const char* kind, const char* what, const void* slot, uint64_t t0) {
    using namespace cs_queue_detail;
    if (!g_cs_diag_cb) return;
    const uint64_t ticks = CsNowTicks() - t0;
    if (ticks > CsTicksPerSec() / 10) g_cs_diag_cb(0, kind, what, slot, ticks);
}
#endif

// ring_bytes: power of two, >= 4096. execute gets Call, CallBlocking and BufferUpload commands.
// poll_mailbox may be null. Returns false on bad arguments or allocation failure.
inline bool CsQueueStart(uint32_t ring_bytes, void (*execute)(const CsCmd*), void (*poll_mailbox)()) {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    if (s.running || !execute || ring_bytes < 4096u || (ring_bytes & (ring_bytes - 1)) != 0 || ring_bytes > (1u << 30))
        return false;
    void* mem = VirtualAlloc(nullptr, ring_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!mem) return false;
    s.ring = static_cast<uint8_t*>(mem);
    s.ring_bytes = ring_bytes;
    s.reserved = 0;
    s.head.v.store(0, std::memory_order_relaxed);
    s.tail.v.store(0, std::memory_order_relaxed);
    s.sleeping.v.store(0, std::memory_order_relaxed);
    s.wake_gen.v.store(0, std::memory_order_relaxed);
    s.quit.v.store(0, std::memory_order_relaxed);
    s.execute = execute;
    s.poll = poll_mailbox;
    s.pending_cmds = 0;
    s.coal_cmd = nullptr;
    s.coal_end = 0;
    DWORD tid = 0;
    s.thread = CreateThread(nullptr, 0, CsWorkerMain, nullptr, 0, &tid);
    if (!s.thread) {
        VirtualFree(mem, 0, MEM_RELEASE);
        s.ring = nullptr;
        return false;
    }
    s.tid = tid;
    s.running = true;
    return true;
}

inline void CsQueueCommit();

// Owner thread only. Waits until the worker has executed everything committed so far (publishes the pending batch first).
inline void CsQueueDrain() {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    if (!s.running) return;
    if (s.reserved != s.head.v.load(std::memory_order_relaxed)) CsQueueCommit();
    const uint32_t h = s.head.v.load(std::memory_order_relaxed);
#if DEVELOPMENT
    const uint64_t t0 = CsNowTicks();
#endif
    for (uint32_t spins = 0; s.tail.v.load(std::memory_order_acquire) != h; ++spins) {
        if (spins < 4000) _mm_pause();
        else Sleep(0);
    }
#if DEVELOPMENT
    CsNoteLongWait("wait", "drain", nullptr, t0);
#endif
}

// Owner thread only. Returns space for `bytes` (rounded up to 8, header included). The caller sets
// op and fills the body, then calls CsQueueCommit. Pads at the wrap point. Waits while the ring is full.
// Returns null if not running or the command is larger than half the ring.
inline CsCmd* CsQueueReserve(uint32_t bytes) {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    s.coal_cmd = nullptr;   // any new command ends the previous one's extendable state
    if (!s.running || bytes < 8 || bytes > s.ring_bytes / 2) return nullptr;
    bytes = (bytes + 7u) & ~7u;
    const uint32_t mask = s.ring_bytes - 1;
    uint32_t pos = s.reserved & mask;
    const uint32_t to_end = s.ring_bytes - pos;
    const uint32_t pad = (to_end < bytes) ? to_end : 0;
    const uint32_t need = pad + bytes;
#if DEVELOPMENT
    uint64_t full_t0 = 0;   // set on the first full check only, so the normal path takes no clock reads
#endif
    for (uint32_t spins = 0;; ++spins) {
        const uint32_t tl = s.tail.v.load(std::memory_order_acquire);
        if (static_cast<uint32_t>(s.reserved + need - tl) <= s.ring_bytes) break;
        // The worker only sees published commands: publish the pending batch, or a full ring would never drain.
        if (s.reserved != s.head.v.load(std::memory_order_relaxed)) CsQueueCommit();
#if DEVELOPMENT
        if (!full_t0) full_t0 = CsNowTicks();
#endif
        if (spins < 4000) _mm_pause();
        else Sleep(0);
    }
#if DEVELOPMENT
    if (full_t0) CsNoteLongWait("wait", "ring full", nullptr, full_t0);
#endif
    if (pad) {
        CsCmd* p = reinterpret_cast<CsCmd*>(s.ring + pos);
        p->op = CsOp_Pad;
        p->reserved = 0;
        p->bytes = pad;
        s.reserved += pad;
        pos = 0;
    }
    CsCmd* c = reinterpret_cast<CsCmd*>(s.ring + pos);
    c->reserved = 0;
    c->bytes = bytes;
    s.reserved += bytes;
    return c;
}

// Owner thread only. Publishes everything reserved so far and wakes the worker if it sleeps.
inline void CsQueueCommit() {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    s.pending_cmds = 0;
    s.head.v.store(s.reserved, std::memory_order_seq_cst);
    if (s.sleeping.v.load(std::memory_order_seq_cst)) {
        s.wake_gen.v.fetch_add(1, std::memory_order_seq_cst);
        WakeByAddressSingle(static_cast<void*>(&s.wake_gen.v));
    }
}

// Owner thread only. Call after a command was reserved and filled. extendable = the command may later be merged
// into (CsQueueTryExtend). Publishes once `batch` commands are pending; otherwise they stay unpublished until then.
inline void CsQueueAfterWrite(CsCmd* c, bool extendable, uint32_t batch) {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    s.coal_cmd = extendable ? c : nullptr;
    s.coal_end = s.reserved;
    if (++s.pending_cmds >= batch) CsQueueCommit();
}

// Owner thread only. The last command if it is still unpublished (the worker has not seen it, so it may be rewritten
// or extended), else null.
inline CsCmd* CsQueueUnpublishedTail() {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    if (!s.coal_cmd || s.coal_end != s.reserved || s.head.v.load(std::memory_order_relaxed) == s.reserved) return nullptr;
    return s.coal_cmd;
}

// Owner thread only. Grows the unpublished tail command by `extra` bytes (multiple of 8) when the ring has room and the
// command ends the ring's contiguous run. Returns the command, or null (the caller writes a new command instead).
inline CsCmd* CsQueueTryExtend(uint32_t extra) {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    CsCmd* c = CsQueueUnpublishedTail();
    if (!c) return nullptr;
    const uint32_t off = static_cast<uint32_t>(reinterpret_cast<uint8_t*>(c) - s.ring);
    if (off + c->bytes + extra > s.ring_bytes) return nullptr;
    if (static_cast<uint32_t>(s.reserved + extra - s.tail.v.load(std::memory_order_acquire)) > s.ring_bytes) return nullptr;
    c->bytes += extra;
    s.reserved += extra;
    s.coal_end = s.reserved;
    return c;
}

// Any thread. Wakes the worker if it sleeps, so it polls the mailbox now. Publish the work BEFORE calling.
inline void CsQueueWake() {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    s.wake_gen.v.fetch_add(1, std::memory_order_seq_cst);
    if (s.sleeping.v.load(std::memory_order_seq_cst))
        WakeByAddressSingle(static_cast<void*>(&s.wake_gen.v));
}

// Stops the worker. Any thread except the worker itself; it does not touch the ring, so the producer must not be
// committing while it runs (CsShutdown drains first). The worker finishes everything committed, then exits and is
// joined. Mailbox items still pending after the join are served on the calling thread. Never call from DllMain.
inline void CsQueueStop() {
    using namespace cs_queue_detail;
    CsQueueState& s = g_cs_queue;
    if (!s.running || GetCurrentThreadId() == s.tid) return;
    s.quit.v.store(1, std::memory_order_seq_cst);
    CsQueueWake();
    WaitForSingleObject(s.thread, INFINITE);
    CloseHandle(s.thread);
    s.thread = nullptr;
    s.running = false;
    if (s.poll) s.poll();
    VirtualFree(s.ring, 0, MEM_RELEASE);
    s.ring = nullptr;
    s.reserved = 0;
}

// Owner thread only. Frees p on the worker, in stream order.
inline void CsEnqueueFree(void* p) {
    CsCmd* c = CsQueueReserve(static_cast<uint32_t>(8 + sizeof(void*)));
    if (!c) return;
    c->op = CsOp_Free;
    std::memcpy(reinterpret_cast<uint8_t*>(c) + 8, &p, sizeof(p));
    CsQueueCommit();
}

// Owner thread only. Runs fn(data) on the worker in stream order; the data is copied into the ring.
inline void CsEnqueueCallback(void (*fn)(const void*), const void* data, uint32_t bytes) {
    const uint32_t total = (static_cast<uint32_t>(8 + sizeof(fn) + bytes) + 7u) & ~7u;
    CsCmd* c = CsQueueReserve(total);
    if (!c) return;
    c->op = CsOp_Callback;
    uint8_t* p = reinterpret_cast<uint8_t*>(c);
    std::memcpy(p + 8, &fn, sizeof(fn));
    if (bytes) std::memcpy(p + 8 + sizeof(fn), data, bytes);
    CsQueueCommit();
}

// Wait until *flag becomes nonzero (decision 10): spin, then WaitOnAddress in 1 ms slices.
// Window threads pump PeekMessage so sent messages are served while waiting.
inline void CsWait(volatile LONG* flag) {
    using namespace cs_queue_detail;
    for (int i = 0; i < 2000; ++i) {
        if (*flag) {
            _ReadWriteBarrier();
            return;
        }
        _mm_pause();
    }
    const uint64_t start = CsNowTicks();
    const bool gui = IsGUIThread(FALSE) != FALSE;
    bool reported = false;
    while (!*flag) {
        LONG expect = 0;
        WaitOnAddress(const_cast<LONG*>(flag), &expect, sizeof(LONG), 1);
        if (gui) {
            MSG m;
            PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);
        }
        if (!reported && g_cs_wait_timeout_cb && CsNowTicks() - start > 5 * CsTicksPerSec()) {
            reported = true;
            g_cs_wait_timeout_cb(flag);
        }
    }
    _ReadWriteBarrier();
}

// Worker side: store the result and release CsWait on d.
inline void CsSignalDone(CsDone* d, DWORD result) {
    d->result = result;
    _InterlockedExchange(&d->done, 1);
    WakeByAddressAll(const_cast<LONG*>(&d->done));
}

inline bool CsOnWorkerThread() {
    return GetCurrentThreadId() == cs_queue_detail::g_cs_queue.tid;
}
