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
#include <vector>

#include <sys/syscall.h>
#include <sys/types.h>
#include <ucontext.h>
#include <unistd.h>

namespace {
    constexpr size_t MaxSamples = 1 << 18;
    constexpr long SampleIntervalNs = 1'000'000; // 1 ms of thread CPU time (kernel tick-limited)
    constexpr double DumpIntervalSec = 15.0;

    struct Sample {
        uint64_t pc;
        uint64_t lr;
    };

    Sample* Samples = nullptr;
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

    double Now() noexcept {
        timespec ts {};
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
    }

    void Handler(int, siginfo_t*, void* context) {
        size_t i = SampleCount.fetch_add(1, std::memory_order_relaxed);
        if (i >= MaxSamples) {
            SampleCount.store(MaxSamples, std::memory_order_relaxed);
            return;
        }
        const ucontext_t* uc = static_cast<const ucontext_t*>(context);
#if defined(__aarch64__)
        Samples[i] = { uc->uc_mcontext.pc, uc->uc_mcontext.regs[30] };
#elif defined(__x86_64__)
        Samples[i] = { (uint64_t)uc->uc_mcontext.gregs[REG_RIP], 0 };
#else
        Samples[i] = { 0, 0 };
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

        std::vector<uint64_t> values(n);
        for (size_t i = 0; i < n; i++) values[i] = Samples[i].pc;
        WriteHistogram(f, "pc", values);
        for (size_t i = 0; i < n; i++) values[i] = Samples[i].lr;
        WriteHistogram(f, "lr", values);
        fclose(f);
    }

    void Start() {
        Started = true;
        Samples = static_cast<Sample*>(calloc(MaxSamples, sizeof(Sample)));
        if (!Samples) { Failed = true; return; }

        struct sigaction sa {};
        sa.sa_sigaction = Handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGPROF, &sa, nullptr) != 0) { Failed = true; return; }

        sigevent sev {};
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
        if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &Timer) != 0) { Failed = true; return; }

        itimerspec spec {};
        spec.it_interval.tv_nsec = SampleIntervalNs;
        spec.it_value.tv_nsec = SampleIntervalNs;
        if (timer_settime(Timer, 0, &spec, nullptr) != 0) { Failed = true; return; }

        StartTime = LastDump = Now();
    }
}

void MelonDsDs::sampler::BeginFrame() noexcept {
    if (!Started) Start();
    double now = Now();
    if (LastFrameStart > 0) FrameIntervalTotal += now - LastFrameStart;
    LastFrameStart = now;
}

void MelonDsDs::sampler::EndFrame() noexcept {
    if (Failed) return;
    double now = Now();
    double frameTime = now - LastFrameStart;
    FrameTimeTotal += frameTime;
    FrameTimeMax = std::max(FrameTimeMax, frameTime);
    Frames++;

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
    SampleCount.store(0);
    free(Samples);
    Samples = nullptr;
}
