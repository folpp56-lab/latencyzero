// LatencyZero - sub-frame input sampling, sharper presentation and audio pacing.
//
// What this can and cannot do, stated up front because the numbers depend on it:
//   * The sampling grid in module 1 is the mod's read cadence. The report rate of a
//     keyboard or mouse is set by the device and its driver and cannot be raised from an
//     application, so 8 kHz means "we notice a change within 0.125 ms of the OS seeing
//     it", not "the device sends 8000 reports per second".
//   * What is actually saved is the game-side delay: an edge is handed to the engine on
//     the physics substep it belongs to instead of waiting for the input queue to be
//     pumped on the next frame.
//   * Module 3's glFinish empties the driver's queue at the cost of a stall, which is
//     why it is off by default; the queue depth under DWM is not the mod's to control.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifdef GEODE_IS_WINDOWS
#include <Geode/modify/CCEGLView.hpp>
#include <Geode/modify/CCParticleSystemQuad.hpp>
#include <Windows.h>
#include <gl/GL.h>
#include <mmsystem.h>
#include <fmod.hpp>
#endif

using namespace geode::prelude;

namespace lz {

// ============================================================================
// Clock
// ============================================================================

#ifdef GEODE_IS_WINDOWS

namespace qpc {
    inline int64_t freq() {
        static const int64_t value = [] {
            LARGE_INTEGER raw;
            QueryPerformanceFrequency(&raw);
            return raw.QuadPart;
        }();
        return value;
    }
    inline int64_t now() {
        LARGE_INTEGER raw;
        QueryPerformanceCounter(&raw);
        return raw.QuadPart;
    }
    inline double toMs(int64_t ticks) { return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq()); }
    inline int64_t fromMs(double ms) { return static_cast<int64_t>(ms * static_cast<double>(freq()) / 1000.0); }
    inline int64_t fromNs(double ns) { return static_cast<int64_t>(ns * static_cast<double>(freq()) / 1e9); }
}

#else

namespace qpc {
    inline int64_t freq() { return 1000000000LL; }
    inline int64_t now() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    inline double toMs(int64_t ticks) { return static_cast<double>(ticks) / 1e6; }
    inline int64_t fromMs(double ms) { return static_cast<int64_t>(ms * 1e6); }
    inline int64_t fromNs(double ns) { return static_cast<int64_t>(ns); }
}

#endif

// ============================================================================
// Settings and measurements
// ============================================================================

struct Settings {
    bool boost = true;
    int pollHz = 8000;
    bool advanceClicks = true;
    bool timerResolution = true;
    bool highPriority = true;
    bool zeroFrameQueue = false;
    bool uncapFps = false;
    bool particleGuard = false;
    int frameBudgetMs = 20;
    bool audioLowLatency = false;
    int audioSamples = 128;
    int audioBlocks = 4;
    bool hud = true;
};

// Read on the game thread only; the sampler thread is given just what it needs through
// g_pollHz, so no lock and no lifetime problem exists between them.
Settings g_cfg;
std::atomic<int> g_pollHz{8000};
std::atomic<bool> g_boost{true};
std::atomic<bool> g_boostActive{false};
// Set while the mod itself calls handleButton, so its own hook does not swallow the
// call it made or time it as if it were the engine's.
std::atomic<bool> g_injecting{false};

bool boostActive() { return g_boostActive.load(std::memory_order_relaxed); }

struct Stats {
    std::atomic<int64_t> sampleStep{0};
    std::atomic<uint64_t> edges{0};
    std::atomic<uint64_t> ringDropped{0};
    std::atomic<uint64_t> samplesIgnored{0};
    std::atomic<double> jitterAvgMs{0.0};
    std::atomic<double> jitterMaxMs{0.0};
    std::atomic<double> pressToApplyMs{0.0};
    std::atomic<double> pressToRenderMs{0.0};
    std::atomic<double> frameMs{0.0};
    std::atomic<double> audioMs{0.0};
    // Physical time of the newest press, consumed by the next presentation to report the
    // whole press-to-picture delay of the frame that shows the jump.
    std::atomic<int64_t> pendingRenderPress{0};
};

Stats& stats() {
    static Stats value;
    return value;
}

Settings readSettings() {
    auto* mod = Mod::get();
    Settings out;
    out.boost = mod->getSettingValue<bool>("boost-enabled");

    const std::string rate = mod->getSettingValue<std::string>("poll-rate");
    out.pollHz = rate == "1000" ? 1000 : rate == "2000" ? 2000 : rate == "4000" ? 4000 : 8000;

    out.advanceClicks = mod->getSettingValue<bool>("advance-clicks");
    out.timerResolution = mod->getSettingValue<bool>("timer-resolution");
    out.highPriority = mod->getSettingValue<bool>("high-priority");
    out.zeroFrameQueue = mod->getSettingValue<bool>("zero-frame-queue");
    out.uncapFps = mod->getSettingValue<bool>("uncap-fps");
    out.particleGuard = mod->getSettingValue<bool>("particle-guard");
    // Geode v5 stores integer settings as int64_t; getSettingValue<int> would not find them.
    out.frameBudgetMs = static_cast<int>(mod->getSettingValue<int64_t>("frame-budget-ms"));
    out.audioLowLatency = mod->getSettingValue<bool>("audio-low-latency");
    out.audioSamples = static_cast<int>(mod->getSettingValue<int64_t>("audio-buffer-samples"));
    out.audioBlocks = static_cast<int>(mod->getSettingValue<int64_t>("audio-buffer-blocks"));
    out.hud = mod->getSettingValue<bool>("hud-visible");
    return out;
}

// ============================================================================
// Module 1 - the jump edge stream
// ============================================================================

struct Edge {
    int64_t qpc = 0;
    bool down = false;
};

// Single producer (the sampler thread), single consumer (the game thread). A ring is
// needed rather than one "latest state" cell because a fast tap can open and close
// entirely inside one frame; replaying the sequence keeps such taps from vanishing.
class Ring {
public:
    static constexpr size_t kCapacity = 4096;
    static constexpr size_t kMask = kCapacity - 1;

    bool push(const Edge& edge) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;
        if (next == m_tail.load(std::memory_order_acquire)) {
            stats().ringDropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        m_slots[head] = edge;
        m_head.store(next, std::memory_order_release);
        return true;
    }

    bool pop(Edge& out) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == m_head.load(std::memory_order_acquire)) return false;
        out = m_slots[tail];
        m_tail.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

private:
    std::array<Edge, kCapacity> m_slots;
    std::atomic<size_t> m_head{0};
    std::atomic<size_t> m_tail{0};
};

Ring g_ring;

// Game-thread state of the jump button. Only the producer half of this is shared, and it
// is shared through the ring, so everything below is plain.
struct JumpOwner {
    bool injectedDown = false;
    bool awaitingNative = false;
    bool expectedNativeDown = false;
    int64_t nativeDeadline = 0;
    // Physical time of a press whose arrival through the input queue has not been timed
    // yet. Cleared when the sample is taken so one press is never counted twice.
    int64_t pendingPress = 0;
    int64_t ownershipOffUntil = 0;
    uint64_t injectedCount = 0;
};

JumpOwner g_jump;

// ============================================================================
// Module 6 - A/B benchmark
// ============================================================================

// Pushes the boost flag into the system (timer resolution, priority, injection). Called
// right when the flag changes, so a benchmark phase switch acts on this tick instead of
// waiting for the next settings read half a second later.
void applyBoostState();

struct Benchmark {
    bool running = false;
    int phase = 0;
    int target = 10;
    std::vector<double> samples[2];
    int64_t startedAt = 0;
    bool restoreBoost = false;
    std::string line;
    int64_t lineUntil = 0;
};

Benchmark g_bench;

void benchAddSample(double ms) {
    if (!g_bench.running) return;
    if (ms <= 0.0 || ms > 250.0) return;
    // Bound the memory a held-down key could otherwise grow without limit.
    if (g_bench.samples[g_bench.phase].size() < static_cast<size_t>(g_bench.target) * 4u)
        g_bench.samples[g_bench.phase].push_back(ms);
}

double meanOf(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    double sum = 0.0;
    for (double value : values) sum += value;
    return sum / static_cast<double>(values.size());
}

void benchEnd(std::string text) {
    g_bench.running = false;
    g_bench.line = std::move(text);
    g_bench.lineUntil = qpc::now() + qpc::fromMs(12000.0);
    g_boost.store(g_bench.restoreBoost, std::memory_order_release);
    applyBoostState();
    log::info("LatencyZero: {}", g_bench.line);
}

void benchFinish() {
    const double vanilla = meanOf(g_bench.samples[0]);
    const double boosted = meanOf(g_bench.samples[1]);
    const auto& s = stats();
    const double jitter = s.jitterAvgMs.load(std::memory_order_relaxed);
    const double jitterMax = s.jitterMaxMs.load(std::memory_order_relaxed);
    benchEnd(fmt::format(
        "Vanilla Avg: {:.2f} ms | Boost {}K Avg: {:.2f} ms | Saved: {:+.2f} ms | Poll Jitter: {:.3f} ms (max {:.3f})",
        vanilla, g_cfg.pollHz / 1000, boosted, vanilla - boosted, jitter, jitterMax));
}

void benchTick() {
    if (!g_bench.running) return;

    const size_t have = g_bench.samples[g_bench.phase].size();
    if (have >= static_cast<size_t>(g_bench.target)) {
        if (g_bench.phase == 0) {
            g_bench.phase = 1;
            g_boost.store(true, std::memory_order_release);
            applyBoostState();
            return;
        }
        benchFinish();
        return;
    }

    if (qpc::now() - g_bench.startedAt > qpc::fromMs(30000.0)) {
        benchEnd(fmt::format("A/B aborted: {} of {} vanilla clicks, {} boost clicks measured",
            g_bench.samples[0].size(), g_bench.target, g_bench.samples[1].size()));
    }
}

// The player supplies every click; the mod only switches its own behaviour between the
// two halves. Nothing here generates input.
void benchToggle() {
    if (g_bench.running) {
        benchEnd("A/B cancelled");
        return;
    }

    g_bench.samples[0].clear();
    g_bench.samples[1].clear();
    g_bench.phase = 0;
    g_bench.running = true;
    g_bench.startedAt = qpc::now();
    g_bench.restoreBoost = g_boost.load(std::memory_order_acquire);
    g_boost.store(false, std::memory_order_release);
    applyBoostState();
    g_bench.line = "A/B: click 10 times with boost OFF";
    g_bench.lineUntil = qpc::now() + qpc::fromMs(30000.0);
    log::info("LatencyZero: A/B started (F9 again cancels)");
}

void toggleBoost() {
    if (g_bench.running) return;
    const bool next = !g_boost.load(std::memory_order_acquire);
    g_boost.store(next, std::memory_order_release);
    Mod::get()->setSettingValue<bool>("boost-enabled", next);
    applyBoostState();
    g_bench.line = next ? "Boost ON" : "Boost OFF (vanilla)";
    g_bench.lineUntil = qpc::now() + qpc::fromMs(2500.0);
}

// ============================================================================
// Modules 2, 3, 4, 5 - Windows tuning, presentation, audio, particles
// ============================================================================

std::atomic<bool> g_skipParticles{false};

#ifdef GEODE_IS_WINDOWS

using NtDelayExecutionFn = NTSTATUS(NTAPI*)(BOOLEAN alertable, PLONGLONG delay);
using NtSetTimerResolutionFn = NTSTATUS(NTAPI*)(ULONG resolutionIn100ns, BOOLEAN set, PULONG actual);
using SetProcessInformationFn = BOOL(WINAPI*)(HANDLE, int, PVOID, DWORD);
using SetThreadInformationFn = BOOL(WINAPI*)(HANDLE, int, PVOID, DWORD);
using SwapIntervalFn = void(WINAPI*)(int);

// PROCESS_POWER_THROTTLING_STATE is spelled out so the file builds against an SDK older
// than Windows 10 2004 as well; the class numbers are ABI fixed.
#ifndef PROCESS_POWER_THROTTLING_CURRENT_VERSION
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1
struct PROCESS_POWER_THROTTLING_STATE {
    DWORD Version;
    DWORD ControlMask;
    DWORD StateMask;
};
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 0x1u
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_UPDATE 0x2u
#endif

constexpr int kProcessPowerThrottlingClass = 7;
constexpr int kThreadPowerThrottlingClass = 3;

struct WinApi {
    HMODULE ntdll = nullptr;
    NtDelayExecutionFn delayExecution = nullptr;
    NtSetTimerResolutionFn setTimerResolution = nullptr;
    SetProcessInformationFn setProcessInformation = nullptr;
    SetThreadInformationFn setThreadInformation = nullptr;

    static WinApi& get() {
        static WinApi api = [] {
            WinApi value;
            value.ntdll = GetModuleHandleA("ntdll.dll");
            value.delayExecution = reinterpret_cast<NtDelayExecutionFn>(reinterpret_cast<void*>(
                GetProcAddress(value.ntdll, "NtDelayExecution")));
            value.setTimerResolution = reinterpret_cast<NtSetTimerResolutionFn>(reinterpret_cast<void*>(
                GetProcAddress(value.ntdll, "NtSetTimerResolution")));
            HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
            value.setProcessInformation = reinterpret_cast<SetProcessInformationFn>(reinterpret_cast<void*>(
                GetProcAddress(kernel32, "SetProcessInformation")));
            value.setThreadInformation = reinterpret_cast<SetThreadInformationFn>(reinterpret_cast<void*>(
                GetProcAddress(kernel32, "SetThreadInformation")));
            return value;
        }();
        return api;
    }
};

HMODULE winmmModule() {
    static HMODULE module = LoadLibraryA("winmm.dll");
    return module;
}

std::atomic<bool> g_stopRequested{false};

using TimeBeginPeriodFn = MMRESULT(WINAPI*)(UINT);
using TimeEndPeriodFn = MMRESULT(WINAPI*)(UINT);

void sleepForTicks(int64_t ticks) {
    auto& api = WinApi::get();
    if (!api.delayExecution) {
        Sleep(static_cast<DWORD>(std::max<int64_t>(1, static_cast<int64_t>(qpc::toMs(ticks)))));
        return;
    }
    // Relative wait: negative, in 100 ns units.
    LONGLONG relative = -static_cast<LONGLONG>(
        static_cast<double>(ticks) * 10000.0 / static_cast<double>(qpc::freq()));
    if (relative >= 0) relative = -1;
    api.delayExecution(FALSE, &relative);
}

// Sleep while the remaining wait is long, spin on Pause when it is short. The spin is
// what makes a 0.125 ms grid possible; it also means 8 kHz keeps one logical core busy,
// which is the price of that grid and the reason the rate is a setting.
void waitUntil(int64_t deadline) {
    const int64_t spinBudget = qpc::fromMs(0.5);
    for (;;) {
        if (g_stopRequested.load(std::memory_order_relaxed)) return;
        const int64_t now = qpc::now();
        if (now >= deadline) return;
        if (deadline - now > spinBudget) sleepForTicks(deadline - now - spinBudget);
        else YieldProcessor();
    }
}

int g_mainCore = 0;

void bindAwayFromRenderCore() {
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (!GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask)) return;

    DWORD_PTR others = processMask & ~(static_cast<DWORD_PTR>(1) << g_mainCore);
    if (!others) others = processMask;

    // Highest numbered bit: with SMT the sibling can still share the physical core, so
    // this is a preference, not a guarantee.
    DWORD_PTR chosen = 1;
    for (DWORD_PTR bit = 1; bit; bit <<= 1) {
        if (others & bit) chosen = bit;
    }
    SetThreadAffinityMask(GetCurrentThread(), chosen);
}

void disableThreadThrottling() {
    auto& api = WinApi::get();
    if (!api.setThreadInformation) return;

    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;
    api.setThreadInformation(GetCurrentThread(), kThreadPowerThrottlingClass,
        &state, sizeof(state));
}

bool anyJumpInputDown() {
    auto pressed = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
    return pressed(VK_SPACE) || pressed(VK_UP) || pressed('W') || pressed(VK_LBUTTON);
}

std::thread g_sampler;

void sampleOnce() {
    const bool down = anyJumpInputDown();
    static bool previous = false;
    if (down == previous) return;
    previous = down;

    const int64_t now = qpc::now();
    stats().edges.fetch_add(1, std::memory_order_relaxed);
    if (down) stats().pendingRenderPress.store(now, std::memory_order_relaxed);
    g_ring.push(Edge{now, down});
}

void samplerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    bindAwayFromRenderCore();
    disableThreadThrottling();

    int hz = 0;
    int64_t step = qpc::fromNs(1e9 / 8000.0);
    int64_t previousSample = qpc::now();
    int64_t next = previousSample + step;
    int64_t deviationSum = 0;
    int64_t deviationMax = 0;
    uint64_t deviationCount = 0;

    while (!g_stopRequested.load(std::memory_order_relaxed)) {
        const int wanted = g_pollHz.load(std::memory_order_relaxed);
        if (wanted != hz && wanted > 0) {
            hz = wanted;
            step = std::max<int64_t>(1, qpc::fromNs(1e9 / static_cast<double>(hz)));
            stats().sampleStep.store(step, std::memory_order_relaxed);
            previousSample = qpc::now();
            next = previousSample + step;
        }

        waitUntil(next);
        if (g_stopRequested.load(std::memory_order_relaxed)) break;

        const int64_t now = qpc::now();
        int64_t deviation = now - previousSample - step;
        if (deviation < 0) deviation = -deviation;
        deviationSum += deviation;
        deviationMax = std::max(deviationMax, deviation);
        previousSample = now;

        if (++deviationCount % 256 == 0) {
            auto& s = stats();
            s.jitterAvgMs.store(qpc::toMs(deviationSum) / static_cast<double>(deviationCount),
                std::memory_order_relaxed);
            s.jitterMaxMs.store(qpc::toMs(deviationMax), std::memory_order_relaxed);
            deviationSum = 0;
            deviationMax = 0;
        }

        sampleOnce();

        next += step;
        // After a suspend or a long spin, restart the grid instead of catching up.
        if (qpc::now() - next > step) next = qpc::now() + step;
    }
}

// ---------------------------------------------------------------- module 2

bool g_timerHeld = false;
bool g_winmmHeld = false;

void applySystemTuning(bool on) {
    auto& api = WinApi::get();

    if (g_cfg.timerResolution && api.setTimerResolution) {
        if (on && !g_timerHeld) {
            ULONG actual = 0;
            // 5000 units of 100 ns = 0.5 ms; the kernel rounds to what the clock offers,
            // so read the granted value back instead of assuming it.
            if (api.setTimerResolution(5000, TRUE, &actual) == 0) {
                g_timerHeld = true;
                log::info("LatencyZero: kernel timer set, granted {:.3f} ms",
                    static_cast<double>(actual) / 10000.0);
            }
        } else if (!on && g_timerHeld) {
            ULONG actual = 0;
            api.setTimerResolution(0, FALSE, &actual);
            g_timerHeld = false;
        }
    }

    if (g_cfg.timerResolution && winmmModule()) {
        auto timeBegin = reinterpret_cast<TimeBeginPeriodFn>(reinterpret_cast<void*>(
            GetProcAddress(winmmModule(), "timeBeginPeriod")));
        auto timeEnd = reinterpret_cast<TimeEndPeriodFn>(reinterpret_cast<void*>(
            GetProcAddress(winmmModule(), "timeEndPeriod")));
        if (on && !g_winmmHeld && timeBegin) { timeBegin(1); g_winmmHeld = true; }
        else if (!on && g_winmmHeld && timeEnd) { timeEnd(1); g_winmmHeld = false; }
    }

    if (g_cfg.highPriority) {
        SetPriorityClass(GetCurrentProcess(), on ? HIGH_PRIORITY_CLASS : NORMAL_PRIORITY_CLASS);
        if (api.setProcessInformation) {
            PROCESS_POWER_THROTTLING_STATE state{};
            state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
            state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                                PROCESS_POWER_THROTTLING_IGNORE_TIMER_UPDATE;
            // StateMask 0 means "do not throttle this process".
            state.StateMask = on ? 0u : PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            if (!api.setProcessInformation(GetCurrentProcess(),
                    kProcessPowerThrottlingClass, &state, sizeof(state))) {
                log::debug("LatencyZero: power throttling control unavailable on this Windows");
            }
        }
    }
}

// ---------------------------------------------------------------- module 3

struct Pacing {
    int64_t lastSwap = 0;
    double frameMs = 0.0;
};

Pacing g_pacing;

void applySwapInterval(int interval) {
    static auto fn = reinterpret_cast<SwapIntervalFn>(reinterpret_cast<void*>(
        wglGetProcAddress("wglSwapIntervalEXT")));
    if (fn) fn(interval);
}

bool g_swapIntervalLowered = false;

void noteSwap() {
    const int64_t now = qpc::now();
    auto& s = stats();

    // F9 / F10 on a rising edge, read directly so the hotkeys do not depend on the
    // Geode KeyboardInputEvent listener signature, which moved between SDK versions.
    // Called once per presented frame, which is fast enough for a two-key shortcut.
    auto rising = [](int vk, bool& wasDown) {
        const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
        const bool edge = down && !wasDown;
        wasDown = down;
        return edge;
    };
    static bool wasF9 = false;
    static bool wasF10 = false;
    if (rising(VK_F10, wasF10)) toggleBoost();
    else if (rising(VK_F9, wasF9)) benchToggle();

    if (g_pacing.lastSwap) {
        const double ms = qpc::toMs(now - g_pacing.lastSwap);
        if (ms > 0.05 && ms < 250.0) {
            g_pacing.frameMs = g_pacing.frameMs > 0.0 ? g_pacing.frameMs * 0.9 + ms * 0.1 : ms;
            s.frameMs.store(g_pacing.frameMs, std::memory_order_relaxed);
        }
    }
    g_pacing.lastSwap = now;

    const int64_t press = s.pendingRenderPress.load(std::memory_order_relaxed);
    if (press) {
        const double ms = qpc::toMs(now - press);
        if (ms > 0.0 && ms < 250.0) {
            const double previous = s.pressToRenderMs.load(std::memory_order_relaxed);
            s.pressToRenderMs.store(previous > 0.0 ? previous * 0.7 + ms * 0.3 : ms, std::memory_order_relaxed);
        }
        s.pendingRenderPress.store(0, std::memory_order_relaxed);
    }

    if (g_cfg.particleGuard && boostActive()) {
        if (g_pacing.frameMs > static_cast<double>(g_cfg.frameBudgetMs)) g_skipParticles.store(true);
        else if (g_pacing.frameMs < static_cast<double>(g_cfg.frameBudgetMs) * 0.8) g_skipParticles.store(false);
    } else {
        g_skipParticles.store(false);
    }

    if (g_cfg.uncapFps && !g_swapIntervalLowered) { applySwapInterval(0); g_swapIntervalLowered = true; }
    else if (!g_cfg.uncapFps && g_swapIntervalLowered) { applySwapInterval(1); g_swapIntervalLowered = false; }
}

// ---------------------------------------------------------------- module 4

int g_audioAppliedSamples = -1;
int g_audioAppliedBlocks = -1;

void applyAudioBuffer() {
    auto* engine = FMODAudioEngine::sharedEngine();
    if (!engine || !engine->m_system) return;

    if (!g_cfg.audioLowLatency) {
        g_audioAppliedSamples = -1;
        g_audioAppliedBlocks = -1;
        stats().audioMs.store(0.0, std::memory_order_relaxed);
        return;
    }
    if (g_audioAppliedSamples == g_cfg.audioSamples && g_audioAppliedBlocks == g_cfg.audioBlocks) return;

    // getSoftwareFormat reports the mix rate in an int; FMOD takes the block length as
    // unsigned and the block count as int.
    int rate = 48000;
    engine->m_system->getSoftwareFormat(&rate, nullptr, nullptr);
    if (rate <= 0) rate = 48000;

    if (engine->m_system->setDSPBufferSize(static_cast<unsigned>(g_cfg.audioSamples), g_cfg.audioBlocks) != FMOD_OK) {
        log::warn("LatencyZero: FMOD refused a {} x {} sample mix buffer", g_cfg.audioSamples, g_cfg.audioBlocks);
        return;
    }

    unsigned length = 0;
    int blocks = 0;
    engine->m_system->getDSPBufferSize(&length, &blocks);
    g_audioAppliedSamples = static_cast<int>(length);
    g_audioAppliedBlocks = blocks;

    // Only the game's share of the delay: the audio driver adds its own buffer on top.
    const double ms = static_cast<double>(length) * static_cast<double>(std::max(1, blocks)) /
        static_cast<double>(rate) * 1000.0;
    stats().audioMs.store(ms, std::memory_order_relaxed);
    log::info("LatencyZero: FMOD mix buffer {} x {} at {} Hz = {:.2f} ms", length, blocks, rate, ms);
}

// ---------------------------------------------------------------- module 6

cocos2d::CCLabelBMFont* g_hud = nullptr;
GJBaseGameLayer* g_hudOwner = nullptr;
int64_t g_hudNext = 0;

void updateHud(GJBaseGameLayer* layer) {
    // The label is a child of the layer it was added to, so a different layer means the
    // old label was freed with its parent; forget it instead of touching it again.
    if (g_hudOwner != layer) { g_hud = nullptr; g_hudOwner = nullptr; }

    if (!g_cfg.hud) {
        if (g_hud) {
            g_hud->removeFromParent();
            g_hud = nullptr;
            g_hudOwner = nullptr;
        }
        return;
    }

    if (!g_hud) {
        g_hud = cocos2d::CCLabelBMFont::create("", "bigFont.fnt");
        if (!g_hud) return;
        g_hud->setAnchorPoint({0.f, 0.f});
        g_hud->setScale(0.32f);
        g_hud->setColor({170, 255, 210});
        g_hud->setID("latencyzero-hud"_spr);
        layer->addChild(g_hud, 9999);
        g_hudOwner = layer;
        g_hudNext = 0;
    }

    const int64_t now = qpc::now();
    if (now < g_hudNext) return;
    g_hudNext = now + qpc::fromMs(250.0);
    g_hud->setPosition(10.f, 10.f);

    auto& s = stats();
    std::string text;
    if (g_bench.running) {
        text = fmt::format("A/B {} half: {}/{} clicks - press jump",
            g_bench.phase == 0 ? "VANILLA (boost off)" : "BOOST 8K",
            g_bench.samples[g_bench.phase].size(), g_bench.target);
    } else if (g_bench.lineUntil > now && !g_bench.line.empty()) {
        text = g_bench.line;
    } else {
        text = fmt::format("[{}K Boost {}] Input-to-Render: {:.2f} ms | Apply: {:.2f} ms | Poll: {:.3f} ms (+/- {:.3f}) | Frame: {:.1f} ms | Audio: {} | Dropped: {}",
            g_cfg.pollHz / 1000,
            g_boost.load(std::memory_order_relaxed) ? "ON" : "OFF",
            s.pressToRenderMs.load(std::memory_order_relaxed),
            s.pressToApplyMs.load(std::memory_order_relaxed),
            qpc::toMs(s.sampleStep.load(std::memory_order_relaxed)),
            s.jitterAvgMs.load(std::memory_order_relaxed),
            g_pacing.frameMs,
            g_cfg.audioLowLatency ? fmt::format("{:.1f} ms", s.audioMs.load(std::memory_order_relaxed))
                                  : std::string("native"),
            s.ringDropped.load(std::memory_order_relaxed));
    }
    g_hud->setString(text.c_str());
}

// ---------------------------------------------------------------- plumbing

// Forward-declared at the top so the benchmark and the F10 toggle can switch the boost
// without waiting for the next settings read.
void applyBoostState() {
    // A running benchmark drives the phases itself, so it stays authoritative even when
    // the user's own boost setting is off.
    const bool wanted = g_boost.load(std::memory_order_acquire);
    const bool on = wanted && (g_cfg.boost || g_bench.running);

    applySystemTuning(on);
    g_boostActive.store(on, std::memory_order_release);
}

void refreshConfig() {
    g_cfg = readSettings();
    // Without the boost bundle the poller still runs, but at 1 kHz: measuring needs edge
    // timestamps, burning a core at 8 kHz for a passive measurement is not justified.
    g_pollHz.store(g_cfg.boost ? g_cfg.pollHz : std::min(g_cfg.pollHz, 1000), std::memory_order_relaxed);

    applyBoostState();
    applyAudioBuffer();
}

void reportNativeArrival(bool down) {
    if (!down) return;
    const int64_t pending = g_jump.pendingPress;
    if (!pending) return;

    const double ms = qpc::toMs(qpc::now() - pending);
    g_jump.pendingPress = 0;
    auto& s = stats();
    s.pressToApplyMs.store(ms, std::memory_order_relaxed);
    benchAddSample(ms);
}

// True when this call is the engine's own copy of an edge the mod already applied.
bool shouldSwallowNative(bool down, int button, bool isPlayer1) {
    if (g_injecting.load(std::memory_order_relaxed)) return false;
    if (button != 1 || !isPlayer1) return false;

    const bool ownsJump = g_boostActive.load(std::memory_order_relaxed) && g_cfg.advanceClicks;
    const int64_t now = qpc::now();

    if (!ownsJump || now < g_jump.ownershipOffUntil) {
        reportNativeArrival(down);
        return false;
    }

    if (g_jump.awaitingNative) {
        if (down == g_jump.expectedNativeDown) {
            g_jump.awaitingNative = false;
            g_jump.expectedNativeDown = false;
            g_jump.injectedDown = down;
            return true;
        }
        // The queue disagrees with what the sampler saw: stop owning the button for a
        // while and let the game's own input path run, rather than fight it.
        g_jump.awaitingNative = false;
        g_jump.ownershipOffUntil = now + qpc::fromMs(1500.0);
        stats().samplesIgnored.fetch_add(1, std::memory_order_relaxed);
        log::debug("LatencyZero: untracked jump input seen, handing the button back for 1.5 s");
        reportNativeArrival(down);
        return false;
    }

    if (down != g_jump.injectedDown) {
        // A queue event that no sampler edge produced. Same conclusion as above.
        g_jump.ownershipOffUntil = now + qpc::fromMs(1500.0);
        g_jump.injectedDown = down;
        stats().samplesIgnored.fetch_add(1, std::memory_order_relaxed);
    }
    reportNativeArrival(down);
    return false;
}

void consumeEdges(GJBaseGameLayer* layer) {
    const bool ownsJump = g_boostActive.load(std::memory_order_relaxed) && g_cfg.advanceClicks &&
        qpc::now() >= g_jump.ownershipOffUntil;

    Edge edge;
    uint64_t budget = 64;
    while (budget-- > 0 && g_ring.pop(edge)) {
        if (!ownsJump) {
            // Passive: remember the press so the engine's own handling of it can be timed.
            if (edge.down) g_jump.pendingPress = edge.qpc;
            continue;
        }
        if (edge.down == g_jump.injectedDown) continue;

        g_jump.injectedDown = edge.down;
        if (edge.down) g_jump.pendingPress = edge.qpc;

        g_injecting.store(true, std::memory_order_relaxed);
        layer->handleButton(edge.down, 1, true);
        g_injecting.store(false, std::memory_order_relaxed);
        ++g_jump.injectedCount;

        g_jump.awaitingNative = true;
        g_jump.expectedNativeDown = edge.down;
        g_jump.nativeDeadline = qpc::now() + qpc::fromMs(120.0);

        const double ms = qpc::toMs(qpc::now() - edge.qpc);
        stats().pressToApplyMs.store(ms, std::memory_order_relaxed);
        benchAddSample(ms);
        // A press measured here will not also arrive for timing.
        g_jump.pendingPress = 0;
    }

    // The queue event we expected never came; the next mismatch is then a real foreign
    // input rather than a stale expectation.
    if (g_jump.awaitingNative && qpc::now() > g_jump.nativeDeadline) {
        g_jump.awaitingNative = false;
        g_jump.ownershipOffUntil = qpc::now() + qpc::fromMs(1500.0);
    }
}

void onProcessCommands(GJBaseGameLayer* layer, bool isLastTick) {
    static int64_t nextConfigRead = 0;
    const int64_t now = qpc::now();
    if (now >= nextConfigRead) {
        nextConfigRead = now + qpc::fromMs(500.0);
        refreshConfig();
    }

    benchTick();
    consumeEdges(layer);
    if (isLastTick) updateHud(layer);
}

void startSampler() {
    static bool started = false;
    if (started) return;
    started = true;
    g_mainCore = static_cast<int>(GetCurrentProcessorNumber());
    g_sampler = std::thread(samplerLoop);
    g_sampler.detach();
    log::info("LatencyZero: input sampler started; the game thread keeps core {}", g_mainCore);
}

#else  // not Windows

void startSampler() {}
void onProcessCommands(GJBaseGameLayer*, bool) {}
bool shouldSwallowNative(bool, int, bool) { return false; }
// Declared at the top and called by the shared benchmark / F10 path, so it needs a body
// here too or the non-Windows translation unit will not link.
void applyBoostState() {}

#endif

}  // namespace lz

// ============================================================================
// Engine hooks
// ============================================================================

#ifdef GEODE_IS_WINDOWS

class $modify(LatencyZeroGJBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        lz::onProcessCommands(this, isLastTick);
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (lz::shouldSwallowNative(down, button, isPlayer1)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(LatencyZeroCCEGLView, cocos2d::CCEGLView) {
    void swapBuffers() override {
        // glFinish, not glFlush: only glFinish actually waits for the queue to drain, and
        // that wait is the frame cost this option trades against presentation delay.
        if (lz::boostActive() && lz::g_cfg.zeroFrameQueue) glFinish();
        CCEGLView::swapBuffers();
        lz::noteSwap();
    }
};

class $modify(LatencyZeroCCParticleSystemQuad, cocos2d::CCParticleSystemQuad) {
    void draw() {
        if (lz::g_skipParticles.load(std::memory_order_relaxed)) return;
        CCParticleSystemQuad::draw();
    }
};

#endif

$execute {
    auto* mod = Mod::get();
    lz::g_cfg = lz::readSettings();
    lz::g_pollHz.store(lz::g_cfg.boost ? lz::g_cfg.pollHz : std::min(lz::g_cfg.pollHz, 1000),
        std::memory_order_relaxed);
    lz::g_boost.store(lz::g_cfg.boost, std::memory_order_release);

#ifdef GEODE_IS_WINDOWS
    lz::startSampler();
    log::info("LatencyZero ready: F10 toggles boost, F9 runs the {}-click A/B (you supply the clicks)",
        lz::g_bench.target * 2);
#else
    log::info("LatencyZero: input tuning is implemented for Windows only");
#endif

    // F9 / F10 are handled by the rising-edge poll in noteSwap(), not a Geode listener.

    (void)mod;
}
