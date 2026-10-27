// VP2S_PROFILE=<seconds>: samples every thread of the process about 1000 times per second and
// prints, every <seconds>, where the busiest thread spends its time (exclusive and inclusive
// functions, resolved from vp2s.pdb). Waiting threads (leaf in ntdll/win32u) count as idle.
// VP2S_HANG_DUMP=<seconds>: when the host frame loop stops for that long, writes the stack of
// every thread to vp2s_hang.txt once (again after each recovery). It also catches a host stack
// overflow and writes the overflowing thread's whole call chain there before the process dies.

#if defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    constexpr size_t kMaxFrames = 24;

    struct ThreadSamples
    {
        uint64_t total = 0;
        uint64_t busy = 0;
        std::unordered_map<uint64_t, uint64_t> leaf;      // address -> samples
        std::vector<std::array<uint64_t, kMaxFrames>> stacks;
        std::vector<std::array<uint64_t, kMaxFrames>> idleStacks; // blocked in a wait: what it waits from
    };

    size_t unwindStack(CONTEXT context, uint64_t *frames, size_t maxFrames)
    {
        size_t count = 0;
        while (count < maxFrames && context.Rip != 0)
        {
            frames[count++] = context.Rip;
            DWORD64 imageBase = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
            if (!function)
            {
                // Leaf function: the return address is on top of the stack.
                context.Rip = *reinterpret_cast<const DWORD64 *>(context.Rsp);
                context.Rsp += 8;
                continue;
            }
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                             &establisher, nullptr);
        }
        return count;
    }

    size_t captureStack(HANDLE thread, std::array<uint64_t, kMaxFrames> &frames)
    {
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        if (!GetThreadContext(thread, &context))
        {
            return 0;
        }
        return unwindStack(context, frames.data(), frames.size());
    }

    std::string moduleOf(uint64_t address)
    {
        HMODULE module = nullptr;
        char name[MAX_PATH] = {};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(address), &module) &&
            GetModuleFileNameA(module, name, MAX_PATH))
        {
            const char *slash = std::strrchr(name, '\\');
            return slash ? slash + 1 : name;
        }
        return "?";
    }

    std::string symbolOf(uint64_t address)
    {
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 512] = {};
        auto *symbol = reinterpret_cast<SYMBOL_INFO *>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 511;
        DWORD64 displacement = 0;
        if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol))
        {
            return std::string(symbol->Name, symbol->NameLen);
        }
        char text[64];
        std::snprintf(text, sizeof(text), "%s+0x%llx", moduleOf(address).c_str(), static_cast<unsigned long long>(address));
        return text;
    }

    void ensureSymbols()
    {
        static bool symbolsReady = false;
        if (!symbolsReady)
        {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(GetCurrentProcess(), nullptr, TRUE);
            symbolsReady = true;
        }
    }

    bool isIdleLeaf(uint64_t address)
    {
        static std::unordered_map<uint64_t, bool> cache;
        const uint64_t page = address >> 16;
        if (auto it = cache.find(page); it != cache.end())
        {
            return it->second;
        }
        const std::string module = moduleOf(address);
        const bool idle = _stricmp(module.c_str(), "ntdll.dll") == 0 || _stricmp(module.c_str(), "win32u.dll") == 0;
        cache.emplace(page, idle);
        return idle;
    }

    void printTop(const char *title, const std::unordered_map<std::string, uint64_t> &counts, uint64_t busy)
    {
        std::vector<std::pair<std::string, uint64_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
        std::cout << "[vp2s-prof]   " << title << ":" << std::endl;
        for (size_t i = 0; i < sorted.size() && i < 25; ++i)
        {
            std::cout << "[vp2s-prof]     " << (sorted[i].second * 1000 / std::max<uint64_t>(busy, 1)) / 10.0 << "% "
                      << sorted[i].first << std::endl;
        }
    }

    void report(std::unordered_map<DWORD, ThreadSamples> &threads)
    {
        ensureSymbols();
        DWORD busiest = 0;
        uint64_t busiestCount = 0;
        for (const auto &[id, samples] : threads)
        {
            std::cout << "[vp2s-prof] thread " << id << ": " << samples.busy << "/" << samples.total << " busy" << std::endl;
            if (samples.busy > busiestCount)
            {
                busiest = id;
                busiestCount = samples.busy;
            }
        }
        if (busiestCount == 0)
        {
            return;
        }
        ThreadSamples &samples = threads[busiest];
        std::unordered_map<std::string, uint64_t> exclusive;
        for (const auto &[address, count] : samples.leaf)
        {
            exclusive[symbolOf(address)] += count;
        }
        std::unordered_map<uint64_t, std::string> names;
        std::unordered_map<std::string, uint64_t> inclusive;
        for (const auto &stack : samples.stacks)
        {
            std::unordered_set<std::string> seen;
            for (const uint64_t address : stack)
            {
                if (address == 0)
                {
                    break;
                }
                auto it = names.find(address);
                if (it == names.end())
                {
                    it = names.emplace(address, symbolOf(address)).first;
                }
                if (seen.insert(it->second).second)
                {
                    ++inclusive[it->second];
                }
            }
        }
        std::cout << "[vp2s-prof] busiest thread " << busiest << " (" << samples.busy << " samples)" << std::endl;
        printTop("exclusive", exclusive, samples.busy);
        printTop("inclusive", inclusive, samples.busy);
        // Where it waits: the functions on its blocked stacks, as shares of all its samples.
        std::unordered_map<std::string, uint64_t> idle;
        for (const auto &stack : samples.idleStacks)
        {
            std::unordered_set<std::string> seen;
            for (const uint64_t address : stack)
            {
                if (address == 0)
                    break;
                auto it = names.find(address);
                if (it == names.end())
                    it = names.emplace(address, symbolOf(address)).first;
                if (seen.insert(it->second).second)
                    ++idle[it->second];
            }
        }
        printTop("waiting in", idle, samples.total);

        // Other threads busy over 30% of the time (e.g. GS raster workers): their top functions.
        for (auto &[id, other] : threads)
        {
            if (id == busiest || other.busy * 10u < other.total * 3u)
                continue;
            std::unordered_map<std::string, uint64_t> top;
            for (const auto &[address, count] : other.leaf)
                top[symbolOf(address)] += count;
            std::vector<std::pair<std::string, uint64_t>> sorted(top.begin(), top.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::cout << "[vp2s-prof] thread " << id << " (" << other.busy << "/" << other.total << "):";
            for (size_t i = 0; i < sorted.size() && i < 8; ++i)
                std::cout << " " << (sorted[i].second * 1000 / std::max<uint64_t>(other.busy, 1)) / 10.0 << "% " << sorted[i].first << ";";
            std::cout << std::endl;
        }
    }

    void dumpAllThreads(unsigned stalledSeconds)
    {
        // Written to its own file: the stuck thread may hold the stdout lock.
        FILE *out = std::fopen("vp2s_hang.txt", "a");
        if (!out)
        {
            return;
        }
        ensureSymbols();
        std::fprintf(out, "=== host frame loop stalled for %u s ===\n", stalledSeconds);
        const DWORD self = GetCurrentThreadId();
        const DWORD process = GetCurrentProcessId();
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
        {
            if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
            {
                continue;
            }
            HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                       entry.th32ThreadID);
            if (!handle)
            {
                continue;
            }
            std::array<uint64_t, kMaxFrames> frames{};
            size_t count = 0;
            if (SuspendThread(handle) != static_cast<DWORD>(-1))
            {
                count = captureStack(handle, frames);
                ResumeThread(handle);
            }
            CloseHandle(handle);
            std::fprintf(out, "thread %lu:\n", entry.th32ThreadID);
            for (size_t i = 0; i < count; ++i)
            {
                std::fprintf(out, "  %s\n", symbolOf(frames[i]).c_str());
            }
        }
        CloseHandle(snapshot);
        std::fclose(out);
        std::cout << "[vp2s] host frame loop stalled: thread stacks written to vp2s_hang.txt" << std::endl;
    }

    // Stack overflow: the faulting thread has almost no stack left, so it hands its context to
    // this thread and blocks while its stack is walked.
    CONTEXT g_overflowContext;
    HANDLE g_overflowEvent = nullptr;
    HANDLE g_overflowDone = nullptr;
    uint64_t g_overflowFrames[100000];

    LONG CALLBACK overflowHandler(EXCEPTION_POINTERS *info)
    {
        if (info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        g_overflowContext = *info->ContextRecord;
        SetEvent(g_overflowEvent);
        WaitForSingleObject(g_overflowDone, 30000);
        return EXCEPTION_CONTINUE_SEARCH;
    }

    void overflowWatchMain()
    {
        WaitForSingleObject(g_overflowEvent, INFINITE);
        const size_t count = unwindStack(g_overflowContext, g_overflowFrames, std::size(g_overflowFrames));
        if (FILE *out = std::fopen("vp2s_hang.txt", "a"))
        {
            ensureSymbols();
            std::fprintf(out, "=== host stack overflow: %zu frames (innermost first) ===\n", count);
            std::unordered_map<std::string, uint64_t> histogram;
            for (size_t i = 0; i < count; ++i)
            {
                const std::string name = symbolOf(g_overflowFrames[i]);
                ++histogram[name];
                if (i < 60 || i + 60 >= count)
                {
                    std::fprintf(out, "  #%zu %s\n", i, name.c_str());
                }
                else if (i == 60)
                {
                    std::fprintf(out, "  ...\n");
                }
            }
            std::vector<std::pair<std::string, uint64_t>> sorted(histogram.begin(), histogram.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
            std::fprintf(out, "most frequent frames:\n");
            for (size_t i = 0; i < sorted.size() && i < 30; ++i)
            {
                std::fprintf(out, "  %llu x %s\n", static_cast<unsigned long long>(sorted[i].second), sorted[i].first.c_str());
            }
            std::fclose(out);
        }
        SetEvent(g_overflowDone);
    }

    void hangWatchMain(const std::atomic<uint64_t> *heartbeat, unsigned limitSeconds)
    {
        uint64_t last = heartbeat->load();
        unsigned stalled = 0;
        for (;;)
        {
            Sleep(1000);
            const uint64_t now = heartbeat->load();
            stalled = (now == last) ? stalled + 1u : 0u;
            last = now;
            if (stalled == limitSeconds)
            {
                dumpAllThreads(stalled);
            }
        }
    }

    void samplerMain(unsigned intervalSeconds)
    {
        const DWORD self = GetCurrentThreadId();
        const DWORD process = GetCurrentProcessId();
        std::unordered_map<DWORD, ThreadSamples> threads;
        std::vector<std::pair<DWORD, HANDLE>> handles;
        ULONGLONG nextRefresh = 0;
        ULONGLONG nextReport = GetTickCount64() + intervalSeconds * 1000ull;
        timeBeginPeriod(1);
        for (;;)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= nextRefresh)
            {
                for (auto &[id, handle] : handles)
                {
                    CloseHandle(handle);
                }
                handles.clear();
                HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 entry{};
                entry.dwSize = sizeof(entry);
                for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
                {
                    if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self)
                    {
                        continue;
                    }
                    if (HANDLE handle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                                                   FALSE, entry.th32ThreadID))
                    {
                        handles.emplace_back(entry.th32ThreadID, handle);
                    }
                }
                CloseHandle(snapshot);
                nextRefresh = now + 1000;
            }
            for (auto &[id, handle] : handles)
            {
                std::array<uint64_t, kMaxFrames> frames{};
                if (SuspendThread(handle) == static_cast<DWORD>(-1))
                {
                    continue;
                }
                const size_t count = captureStack(handle, frames); // no allocation while suspended
                ResumeThread(handle);
                if (count == 0)
                {
                    continue;
                }
                ThreadSamples &samples = threads[id];
                ++samples.total;
                if (isIdleLeaf(frames[0]))
                {
                    samples.idleStacks.push_back(frames);
                    continue;
                }
                ++samples.busy;
                ++samples.leaf[frames[0]];
                samples.stacks.push_back(frames);
            }
            if (GetTickCount64() >= nextReport)
            {
                report(threads);
                std::cout.flush();
                threads.clear();
                nextReport = GetTickCount64() + intervalSeconds * 1000ull;
            }
            Sleep(1);
        }
    }
}

void startProfiler()
{
    const char *value = std::getenv("VP2S_PROFILE");
    if (!value || std::atoi(value) <= 0)
    {
        return;
    }
    std::thread(samplerMain, static_cast<unsigned>(std::atoi(value))).detach();
}

void startHangWatch(const std::atomic<uint64_t> &heartbeat)
{
    const char *value = std::getenv("VP2S_HANG_DUMP");
    if (!value || std::atoi(value) <= 0)
    {
        return;
    }
    std::thread(hangWatchMain, &heartbeat, static_cast<unsigned>(std::atoi(value))).detach();
    g_overflowEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    g_overflowDone = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    std::thread(overflowWatchMain).detach();
    AddVectoredExceptionHandler(1, overflowHandler);
}

#else
#include <atomic>
void startProfiler() {}
void startHangWatch(const std::atomic<uint64_t> &) {}
#endif
