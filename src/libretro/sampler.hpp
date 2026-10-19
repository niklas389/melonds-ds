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

#ifndef MELONDSDS_SAMPLER_HPP
#define MELONDSDS_SAMPLER_HPP

namespace MelonDsDs::sampler {
    void BeginFrame() noexcept;
    void EndFrame() noexcept;
    void Stop() noexcept;

    // Time the emulation thread spends blocked rather than computing,
    // e.g. in the frontend's video or audio callback or waiting for another thread.
    enum class Wait { Video, Audio, Semaphore };
    double Clock() noexcept;
    void AddWait(Wait kind, double seconds, const void* caller = nullptr) noexcept;

    // Samples the calling thread too (melonDS's render threads), for as long as the scope lives
    class ThreadScope {
    public:
        ThreadScope() noexcept;
        ~ThreadScope() noexcept;
        ThreadScope(const ThreadScope&) = delete;
        ThreadScope& operator=(const ThreadScope&) = delete;
    private:
        int _slot;
    };

    class WaitScope {
    public:
        explicit WaitScope(Wait kind, const void* caller = nullptr) noexcept : _kind(kind), _caller(caller), _start(Clock()) {}
        ~WaitScope() noexcept { AddWait(_kind, Clock() - _start, _caller); }
        WaitScope(const WaitScope&) = delete;
        WaitScope& operator=(const WaitScope&) = delete;
    private:
        Wait _kind;
        const void* _caller;
        double _start;
    };
}

#endif // MELONDSDS_SAMPLER_HPP
