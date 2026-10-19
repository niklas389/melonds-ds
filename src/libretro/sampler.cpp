/*
    Copyright 2026 melonDS DS contributors

    melonDS DS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS DS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS DS. If not, see http://www.gnu.org/licenses/.
*/

// Minimal sampling profiler for devices without perf.
// Samples the program counter of the thread that calls retro_run
// on a CPU-time timer and periodically writes a histogram to a text file.
// Only built when MELONDSDS_SAMPLER is enabled; Linux/aarch64 only.

#include "sampler.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
    constexpr size_t MaxSamples = 1 << 19;
    // the emulation thread is slot 0; other threads get the next free one
    constexpr int MaxThreadSlots = 8;
    constexpr long SampleIntervalNs = 1'000'000; // 1 ms of thread CPU time (kernel tick-limited)
    constexpr double DumpIntervalSec = 15.0;
    // Gaps between frames longer than this are pauses (menu, loading), not slow frames
    constexpr double PauseThresholdSec = 0.25;
    constexpr double BucketLimitsMs[] = {16.7, 20, 25, 33.4, 50, 100};
    constexpr size_t NumBuckets = std::size(BucketLimitsMs) + 1;
    constexpr double TimelineWindowSec = 5.0;
    constexpr size_t MaxTimelineWindows = 720; // one hour

    struct Sample {
        uint64_t pc;
        uint64_t lr;
        uint32_t slot;
    };

    // static, so that threads started before the first frame can be sampled
    Sample Samples[MaxSamples];
    std::atomic<bool> SlotUsed[MaxThreadSlots] {};
    std::atomic<pid_t> SlotThread[MaxThreadSlots] {};
    std::atomic<size_t> SampleCount {0};
    timer_t Timer {};
    bool Started = false;
    bool Failed = false;

    uint64_t Frames = 0;
    double FrameTimeTotal = 0;
    double FrameTimeMax = 0;
    double LastDump = 0;
    double StartTime = 0;
    double LastFrameStart = 0;
    double FrameIntervalTotal = 0;
    uint64_t ActiveIntervals = 0;
    double ActiveIntervalTotal = 0;
    double ActiveRunTotal = 0;
    double PausedTotal = 0;
    uint64_t Pauses = 0;
    double PendingInterval = -1;
    uint64_t RunBuckets[NumBuckets] {};
    uint64_t IntervalBuckets[NumBuckets] {};
    float Timeline[MaxTimelineWindows] {};
    size_t TimelineWindows = 0;
    double WindowStart = 0;
    uint64_t WindowFrames = 0;

    // only the thread that runs the frames is measured
    thread_local bool OnEmuThread = false;
    pid_t EmuThreadId = 0;
    double CpuStart = 0;
    double WaitTotals[3] {};
    uint64_t WaitCounts[3] {};
    struct CallerWait {
        uint64_t caller;
        double seconds;
        uint64_t count;
    };
    constexpr size_t MaxCallers = 32;
    CallerWait CallerWaits[MaxCallers] {};
    size_t NumCallers = 0;

    double ThreadCpu() noexcept {
        timespec ts {};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }

    // CPU time of every thread in this process, from /proc
    void WriteThreads(FILE* f, double elapsed) {
        fprintf(f, "## threads\n");
        DIR* dir = opendir("/proc/self/task");
        if (!dir) return;
        long ticks = sysconf(_SC_CLK_TCK);
        while (dirent* entry = readdir(dir)) {
            if (entry->d_name[0] == '.') continue;
            char path[64];
            snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
            FILE* stat = fopen(path, "r");
            if (!stat) continue;
            char buf[512] = {};
            size_t len = fread(buf, 1, sizeof(buf) - 1, stat);
            fclose(stat);
            buf[len] = 0;
            // the name is in parentheses and may contain spaces
            char* open = strchr(buf, '(');
            char* close = strrchr(buf, ')');
            if (!open || !close) continue;
            std::string name(open + 1, close);
            unsigned long utime = 0, stime = 0;
            // fields after the name: state ppid pgrp session tty_nr tpgid flags minflt cminflt majflt cmajflt utime stime
            if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &utime, &stime) != 2) continue;
            double cpu = double(utime + stime) / ticks;
            int tid = atoi(entry->d_name);
            fprintf(f, "%d %s %.1f%s\n", tid, name.c_str(), elapsed > 0 ? 100.0 * cpu / elapsed : 0.0,
                    tid == EmuThreadId ? " emulation" : "");
        }
        closedir(dir);
    }

    size_t Bucket(double seconds) noexcept {
        double ms = seconds * 1000.0;
        size_t i = 0;
        while (i < std::size(BucketLimitsMs) && ms >= BucketLimitsMs[i]) i++;
        return i;
    }

    void WriteBuckets(FILE* f, const char* name, const uint64_t* buckets) {
        fprintf(f, "%s", name);
        for (size_t i = 0; i < NumBuckets; i++) {
            if (i < std::size(BucketLimitsMs))
                fprintf(f, " <%g:%llu", BucketLimitsMs[i], (unsigned long long)buckets[i]);
            else
                fprintf(f, " >=%g:%llu", BucketLimitsMs[i - 1], (unsigned long long)buckets[i]);
        }
        fprintf(f, "\n");
    }

    double Now() noexcept {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }

    void Handler(int, siginfo_t* info, void* context) {
        size_t i = SampleCount.fetch_add(1, std::memory_order_relaxed);
        if (i >= MaxSamples) {
            SampleCount.store(MaxSamples, std::memory_order_relaxed);
            return;
        }
        const ucontext_t* uc = static_cast<const ucontext_t*>(context);
        uint32_t slot = (uint32_t)info->si_value.sival_int;
#if defined(__aarch64__)
        Samples[i] = { uc->uc_mcontext.pc, uc->uc_mcontext.regs[30], slot };
#elif defined(__x86_64__)
        Samples[i] = { (uint64_t)uc->uc_mcontext.gregs[REG_RIP], 0, slot };
#else
        Samples[i] = { 0, 0, slot };
#endif
    }

    const char* OutputPath() noexcept {
        const char* path = getenv("MELONDSDS_PROFILE_PATH");
        return path ? path : "/userdata/system/melondsds-profile.txt";
    }

    void WriteHistogram(FILE* f, const char* title, std::vector<uint64_t>& values) {
        std::sort(values.begin(), values.end());
        std::vector<std::pair<uint64_t, uint64_t>> counts; // (count, address)
        for (size_t i = 0; i < values.size();) {
            size_t j = i;
            while (j < values.size() && values[j] == values[i]) j++;
            counts.emplace_back(j - i, values[i]);
            i = j;
        }
        std::sort(counts.begin(), counts.end(), [](auto& a, auto& b) { return a.first > b.first; });
        fprintf(f, "## %s (unique=%zu)\n", title, counts.size());
        size_t n = std::min<size_t>(counts.size(), 4000);
        for (size_t i = 0; i < n; i++) {
            fprintf(f, "%llx %llu\n", (unsigned long long)counts[i].second, (unsigned long long)counts[i].first);
        }
    }

    void Dump() {
        FILE* f = fopen(OutputPath(), "w");
        if (!f) return;

        size_t n = std::min(SampleCount.load(std::memory_order_relaxed), MaxSamples);
        double elapsed = Now() - StartTime;
        fprintf(f, "# melonDS DS sampler\n");
        fprintf(f, "elapsed_s %.3f\n", elapsed);
        fprintf(f, "frames %llu\n", (unsigned long long)Frames);
        fprintf(f, "fps %.2f\n", elapsed > 0 ? Frames / elapsed : 0.0);
        fprintf(f, "retro_run_avg_ms %.3f\n", Frames ? FrameTimeTotal * 1000.0 / Frames : 0.0);
        fprintf(f, "retro_run_max_ms %.3f\n", FrameTimeMax * 1000.0);
        fprintf(f, "frame_interval_avg_ms %.3f\n", Frames > 1 ? FrameIntervalTotal * 1000.0 / (Frames - 1) : 0.0);
        fprintf(f, "active_fps %.2f\n", ActiveIntervalTotal > 0 ? ActiveIntervals / ActiveIntervalTotal : 0.0);
        fprintf(f, "active_run_avg_ms %.3f\n", ActiveIntervals ? ActiveRunTotal * 1000.0 / ActiveIntervals : 0.0);
        fprintf(f, "active_outside_avg_ms %.3f\n", ActiveIntervals ? (ActiveIntervalTotal - ActiveRunTotal) * 1000.0 / ActiveIntervals : 0.0);
        fprintf(f, "pauses %llu\n", (unsigned long long)Pauses);
        fprintf(f, "paused_s %.3f\n", PausedTotal);
        WriteBuckets(f, "run_ms", RunBuckets);
        WriteBuckets(f, "interval_ms", IntervalBuckets);
        fprintf(f, "timeline_fps_%gs", TimelineWindowSec);
        for (size_t i = 0; i < TimelineWindows; i++) fprintf(f, " %.0f", Timeline[i]);
        fprintf(f, "\n");
        fprintf(f, "emu_thread_cpu_pct %.1f\n", elapsed > 0 ? 100.0 * (ThreadCpu() - CpuStart) / elapsed : 0.0);
        if (Frames) {
            fprintf(f, "video_cb_ms_per_frame %.3f\n", WaitTotals[(int)MelonDsDs::sampler::Wait::Video] * 1000.0 / Frames);
            fprintf(f, "audio_cb_ms_per_frame %.3f\n", WaitTotals[(int)MelonDsDs::sampler::Wait::Audio] * 1000.0 / Frames);
            fprintf(f, "thread_wait_ms_per_frame %.3f\n", WaitTotals[(int)MelonDsDs::sampler::Wait::Semaphore] * 1000.0 / Frames);
        }
        fprintf(f, "samples %zu\n", n);
        fprintf(f, "page_size %ld\n", sysconf(_SC_PAGESIZE));

        fprintf(f, "## maps\n");
        if (FILE* maps = fopen("/proc/self/maps", "r")) {
            char line[1024];
            while (fgets(line, sizeof(line), maps)) {
                // Only executable mappings are of interest
                char perms[8] = {};
                if (sscanf(line, "%*llx-%*llx %7s", perms) == 1 && perms[2] == 'x') {
                    fputs(line, f);
                }
            }
            fclose(maps);
        }

        std::vector<uint64_t> values;
        values.reserve(n);
        for (size_t i = 0; i < n; i++) if (Samples[i].slot == 0) values.push_back(Samples[i].pc);
        WriteHistogram(f, "pc", values);
        values.clear();
        for (size_t i = 0; i < n; i++) if (Samples[i].slot == 0) values.push_back(Samples[i].lr);
        WriteHistogram(f, "lr", values);

        // the other sampled threads, by thread ID
        for (int slot = 1; slot < MaxThreadSlots; slot++) {
            values.clear();
            for (size_t i = 0; i < n; i++) if (Samples[i].slot == (uint32_t)slot) values.push_back(Samples[i].pc);
            if (values.empty()) continue;
            char title[32];
            snprintf(title, sizeof(title), "pc.%d", (int)SlotThread[slot].load());
            WriteHistogram(f, title, values);
        }

        // where the emulation thread waited for other threads: caller address, ms per frame, waits per frame
        fprintf(f, "## waits\n");
        for (size_t i = 0; i < NumCallers; i++) {
            fprintf(f, "%llx %.3f %.2f\n", (unsigned long long)CallerWaits[i].caller,
                    Frames ? CallerWaits[i].seconds * 1000.0 / Frames : 0.0,
                    Frames ? double(CallerWaits[i].count) / Frames : 0.0);
        }

        WriteThreads(f, elapsed);
        fclose(f);
    }

    bool InstallHandler() {
        static std::atomic<int> installed {0};
        int expected = 0;
        if (installed.compare_exchange_strong(expected, 1)) {
            struct sigaction sa {};
            sa.sa_sigaction = Handler;
            sa.sa_flags = SA_SIGINFO | SA_RESTART;
            sigemptyset(&sa.sa_mask);
            installed = sigaction(SIGPROF, &sa, nullptr) == 0 ? 2 : 3;
        }
        while (installed.load() == 1) {}
        return installed.load() == 2;
    }

    // a CPU-time timer that interrupts the calling thread
    bool StartThreadTimer(timer_t& timer, int slot) {
        sigevent sev {};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev.sigev_value.sival_int = slot;
        sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &timer) != 0) return false;

        itimerspec spec {};
        spec.it_interval.tv_nsec = SampleIntervalNs;
        spec.it_value.tv_nsec = SampleIntervalNs;
        if (timer_settime(timer, 0, &spec, nullptr) != 0) { timer_delete(timer); return false; }
        return true;
    }

    thread_local timer_t ThreadTimer {};

    void Start() {
        Started = true;
        if (!InstallHandler()) { Failed = true; return; }
        if (!StartThreadTimer(Timer, 0)) { Failed = true; return; }

        StartTime = LastDump = WindowStart = Now();
        OnEmuThread = true;
        EmuThreadId = static_cast<pid_t>(syscall(SYS_gettid));
        CpuStart = ThreadCpu();
    }
}

MelonDsDs::sampler::ThreadScope::ThreadScope() noexcept : _slot(-1) {
    if (!InstallHandler()) return;
    for (int slot = 1; slot < MaxThreadSlots; slot++) {
        bool expected = false;
        if (SlotUsed[slot].compare_exchange_strong(expected, true)) {
            SlotThread[slot] = static_cast<pid_t>(syscall(SYS_gettid));
            if (StartThreadTimer(ThreadTimer, slot)) _slot = slot;
            else SlotUsed[slot] = false;
            return;
        }
    }
}

MelonDsDs::sampler::ThreadScope::~ThreadScope() noexcept {
    if (_slot < 0) return;
    timer_delete(ThreadTimer);
    SlotUsed[_slot] = false;
}

double MelonDsDs::sampler::Clock() noexcept {
    return OnEmuThread ? Now() : 0;
}

void MelonDsDs::sampler::AddWait(Wait kind, double seconds, const void* caller) noexcept {
    if (!OnEmuThread || !Started || Failed) return;
    WaitTotals[(int)kind] += seconds;
    WaitCounts[(int)kind]++;
    if (kind != Wait::Semaphore) return;
    uint64_t address = reinterpret_cast<uint64_t>(caller);
    for (size_t i = 0; i < NumCallers; i++) {
        if (CallerWaits[i].caller == address) {
            CallerWaits[i].seconds += seconds;
            CallerWaits[i].count++;
            return;
        }
    }
    if (NumCallers < MaxCallers)
        CallerWaits[NumCallers++] = { address, seconds, 1 };
}

void MelonDsDs::sampler::BeginFrame() noexcept {
    if (!Started) Start();
    double now = Now();
    if (LastFrameStart > 0) {
        double interval = now - LastFrameStart;
        FrameIntervalTotal += interval;
        if (interval >= PauseThresholdSec) {
            Pauses++;
            PausedTotal += interval;
        } else {
            PendingInterval = interval;
        }
    }
    LastFrameStart = now;
}

void MelonDsDs::sampler::EndFrame() noexcept {
    if (Failed) return;
    double now = Now();
    double frameTime = now - LastFrameStart;
    FrameTimeTotal += frameTime;
    FrameTimeMax = std::max(FrameTimeMax, frameTime);
    Frames++;
    RunBuckets[Bucket(frameTime)]++;
    if (PendingInterval >= 0) {
        // The interval that ended when this frame began, paired with this frame's run time
        ActiveIntervals++;
        ActiveIntervalTotal += PendingInterval;
        ActiveRunTotal += frameTime;
        IntervalBuckets[Bucket(PendingInterval)]++;
        PendingInterval = -1;
    }

    WindowFrames++;
    if (now - WindowStart >= TimelineWindowSec) {
        if (TimelineWindows < MaxTimelineWindows)
            Timeline[TimelineWindows++] = static_cast<float>(WindowFrames / (now - WindowStart));
        WindowStart = now;
        WindowFrames = 0;
    }

    if (now - LastDump >= DumpIntervalSec) {
        Dump();
        LastDump = Now();
    }
}

void MelonDsDs::sampler::Stop() noexcept {
    if (!Started || Failed) return;
    timer_delete(Timer);
    Dump();
    Started = false;
    Frames = 0;
    FrameTimeTotal = FrameTimeMax = FrameIntervalTotal = 0;
    LastFrameStart = 0;
    ActiveIntervals = Pauses = 0;
    ActiveIntervalTotal = ActiveRunTotal = PausedTotal = 0;
    PendingInterval = -1;
    std::fill(std::begin(RunBuckets), std::end(RunBuckets), 0);
    std::fill(std::begin(IntervalBuckets), std::end(IntervalBuckets), 0);
    TimelineWindows = 0;
    WindowFrames = 0;
    std::fill(std::begin(WaitTotals), std::end(WaitTotals), 0);
    std::fill(std::begin(WaitCounts), std::end(WaitCounts), 0);
    NumCallers = 0;
    SampleCount.store(0);
}
