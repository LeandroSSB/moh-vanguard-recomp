#include "MiniTest.h"
#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <exception>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace ps2_syscalls;

namespace
{
    constexpr int KE_OK = 0;
    constexpr int KE_EVF_COND = -421;

    constexpr uint32_t WEF_OR = 1u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;

    struct Ps2EventFlagInfo
    {
        uint32_t attr;
        uint32_t option;
        uint32_t initBits;
        uint32_t currBits;
        int32_t numThreads;
        int32_t reserved1;
        int32_t reserved2;
    };

    static_assert(sizeof(Ps2EventFlagInfo) == 28u, "Unexpected Ps2EventFlagInfo layout.");

    struct TestEnv
    {
        std::vector<uint8_t> rdram;
        PS2Runtime runtime;

        TestEnv() : rdram(PS2_RAM_SIZE, 0u)
        {
        }
    };

    std::atomic<uint32_t> g_lastIntcArg{0u};
    constexpr uint32_t kIdleVSyncWaitPc = 0x00160000u;
    constexpr uint32_t kVSyncWaitPc = 0x00160100u;
    constexpr uint32_t kVSyncResumePc = 0x00160110u;
    constexpr uint32_t kIrqWaitPc = 0x00160200u;
    constexpr uint32_t kIrqResumePc = 0x00160210u;
    constexpr uint32_t kIntcHandlerPc = 0x00160220u;
    constexpr uint32_t kIrqStackWaitPc = 0x00160230u;
    constexpr uint32_t kIrqStackResumePc = 0x00160240u;
    constexpr uint32_t kIrqStackHandlerPc = 0x00160250u;
    constexpr uint32_t kIrqRegistrationSp = 0x001E0000u;
    constexpr uint32_t kIrqRegistrationGuardAddr = kIrqRegistrationSp - 16u;
    constexpr uint32_t kISemaWaitPc = 0x00160300u;
    constexpr uint32_t kISemaResumePc = 0x00160310u;
    constexpr uint32_t kISemaDriverPc = 0x00160320u;
    constexpr uint32_t kISemaHandlerPc = 0x00160330u;
    constexpr uint32_t kEventWaitPc = 0x00160400u;
    constexpr uint32_t kEventResumePc = 0x00160410u;
    constexpr uint32_t kEventProducerPc = 0x00160420u;
    constexpr uint32_t kTimer2WaitPc = 0x00160500u;
    constexpr uint32_t kTimer2ResumePc = 0x00160510u;
    constexpr uint32_t kTimer2HandlerPc = 0x00160520u;
    constexpr uint32_t kInvocationQueuePc = 0x00160530u;
    constexpr uint32_t kInvocationQueueResumePc = 0x00160540u;
    constexpr uint32_t kInvocationQueueHandlerPc = 0x00160550u;
    constexpr uint32_t kVifDmaEntryPc = 0x00160600u;
    constexpr uint32_t kVifDmaResumePc = 0x00160610u;
    constexpr uint32_t kVifDmaDmacHandlerPc = 0x00160620u;
    constexpr uint32_t kVifDmaIntcHandlerPc = 0x00160630u;
    constexpr uint32_t kVif1ChannelChcr = 0x10009000u;
    constexpr uint32_t kVif1ChannelMadr = 0x10009010u;
    constexpr uint32_t kVif1ChannelQwc = 0x10009020u;

    // M5-T18: timer/alarm delivery shaping harness. The T17 capture (main
    // 0x6A0310 out of lastGood 0x355300 = the INTC cause-10 dispatch, mid-
    // movie) showed the timer wheel callbacks riding the very checkpoint
    // that raised them - mid-fill, on the interrupted stack. These entries
    // let the tests post a timer IRQ / SetAlarm from main and observe the
    // vsync tick + bind sp the callback actually ran at.
    constexpr uint32_t kT18TimerPostPc = 0x00160700u;
    constexpr uint32_t kT18TimerResumePc = 0x00160710u;
    constexpr uint32_t kT18TimerHandlerPc = 0x00160720u;
    constexpr uint32_t kT18AlarmPostPc = 0x00160730u;
    constexpr uint32_t kT18AlarmResumePc = 0x00160740u;
    constexpr uint32_t kT18AlarmHandlerPc = 0x00160750u;
    // Main parks shallow-but-deep enough that the pre-fix stackless
    // fallback finds headroom and binds AT this sp (inside main's below-sp
    // band - the T13/T14/T17 poison band), so the carve assertion
    // discriminates the bind shape too.
    constexpr uint32_t kT18MainSp = 0x1FFFE000u;

    std::atomic<bool> g_t18Resumed{false};
    uint32_t g_t18PostVsync = 0u;
    uint32_t g_t18HandlerVsync = 0u;
    uint32_t g_t18HandlerSp = 0u;
    uint32_t g_t18HandlerArg = 0u;
    int g_t18SemaId = 0;

    std::atomic<uint32_t> g_vifDmaDmacCause{0u};
    std::atomic<uint32_t> g_vifDmaDmacArg{0u};
    std::atomic<uint32_t> g_vifDmaIntcCause{0u};
    std::atomic<uint32_t> g_vifDmaIntcArg{0u};
    std::atomic<bool> g_vifDmaResumed{false};

    constexpr uint32_t kTimer2Count = 0x10001000u;
    constexpr uint32_t kTimer2Mode = 0x10001010u;
    constexpr uint32_t kTimer2Compare = 0x10001020u;
    constexpr uint32_t kTimerModeBusClockDiv256 = 2u;
    constexpr uint32_t kTimerModeCue = 1u << 7u;
    constexpr uint32_t kTimerModeCmpe = 1u << 8u;
    constexpr uint32_t kTimerModeEquf = 1u << 10u;

    constexpr uint32_t kVSyncFlagAddr = 0x1800u;
    constexpr uint32_t kVSyncTickAddr = 0x1810u;
    constexpr uint32_t kEventResultAddr = 0x1820u;

    std::vector<int> g_dispatchTrace;
    int g_testSemaphoreId = 0;
    int g_testEventFlagId = 0;
    int32_t g_resumedResult = 0;
    uint32_t g_vsyncFlag = 0;
    uint64_t g_vsyncTick = 0;
    uint64_t g_vsyncCsr = 0;
    std::atomic<bool> g_timer2Resumed{false};
    uint32_t g_irqObservedSp = 0u;
    uint32_t g_invocationQueueRuns = 0u;
    uint32_t g_invocationQueueSp = 0u;
    bool g_invocationQueueSpChanged = false;

    // ---- M6-T2: clock pacing (wall-master vsync delivery) ------------------
    // Simulates the measured T1 baseline shape (contrib/m6-clock-baseline.md):
    // a guest whose cycles arrive at ~9.7% of realtime (the movie-decode
    // regime), a total executor stall, then a ~5x-realtime recovery (the T1
    // overshoot-plateau shape, amplified). Falsifiable predictions:
    //   H1: delivered vsyncs in the slow window track the DEADLINE schedule
    //       (~59.94/s wall), not cycle% x 60 (~5.8/s - the T1 identity).
    //   H2: the recovery window shows no overshoot plateau - catch-up is
    //       rate-limited to the wall beat.
    constexpr uint32_t kClockThrottlePc = 0x00161000u;
    constexpr uint32_t kM6t2PhaseSlow = 1u;
    constexpr uint32_t kM6t2PhaseStall = 2u;
    constexpr uint32_t kM6t2PhaseFast = 3u;

    struct M6T2ClockSample
    {
        uint64_t vsyncTick;
        uint32_t phase;
        int64_t tNs;
    };
    std::atomic<uint32_t> g_m6t2Phase{0u};
    std::chrono::steady_clock::time_point g_m6t2PhaseStart{};
    std::vector<M6T2ClockSample> g_m6t2Samples;

    void m6t2SpinFor(std::chrono::steady_clock::time_point until)
    {
        while (std::chrono::steady_clock::now() < until)
        {
        }
    }

    // The "guest block": charges a calibrated accounted-cycle chunk per
    // dispatch and eats a precise wall slice (spin-wait, immune to the
    // Windows timer granularity), then loops on the same pc forever.
    void m6t2ThrottleBlock(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        using clock = std::chrono::steady_clock;
        EeScheduler &scheduler = runtime->eeScheduler();
        const uint32_t phase = g_m6t2Phase.load(std::memory_order_acquire);
        const auto now = clock::now();
        g_m6t2Samples.push_back({scheduler.currentVSyncTick(), phase,
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     now.time_since_epoch())
                                     .count()});
        switch (phase)
        {
        case kM6t2PhaseSlow:
            // ~9.7% of realtime: 28.6k accounted cycles per ~1.05 ms wall.
            scheduler.accountCycles(30000u);
            m6t2SpinFor(now + std::chrono::microseconds(1050));
            if (now - g_m6t2PhaseStart >= std::chrono::milliseconds(1500))
            {
                g_m6t2Phase.store(kM6t2PhaseStall, std::memory_order_release);
                g_m6t2PhaseStart = clock::now();
            }
            break;
        case kM6t2PhaseStall:
            // The T1 total stall: wall advances, guest cycles do not.
            m6t2SpinFor(now + std::chrono::milliseconds(300));
            g_m6t2Phase.store(kM6t2PhaseFast, std::memory_order_release);
            g_m6t2PhaseStart = clock::now();
            break;
        case kM6t2PhaseFast:
            // ~5x realtime: with the old double gate the wall chain ends up
            // in the past and the CYCLE gate paces recovery (the T1 82/s
            // plateau shape) with unbounded multi-fire bursts.
            scheduler.accountCycles(1500000u);
            m6t2SpinFor(now + std::chrono::microseconds(1000));
            if (now - g_m6t2PhaseStart >= std::chrono::milliseconds(1000))
            {
                g_m6t2Phase.store(4u, std::memory_order_release);
                runtime->requestStop();
            }
            break;
        default:
            break;
        }
    }

    // ---- M6-T3: bounded replay of stall-missed beats ------------------------
    // The measured boot-regime shape: ~10%-realtime guest work, then a
    // several-hundred-ms ZERO-cycle stretch (the executor blocked inside a
    // host call - ISO streaming), then more guest work. The wall chain owes
    // ~59.94 beats per wall second across the whole span; the pre-T3
    // re-anchor silently DROPPED every beat past the 2-period catch-up grace
    // (the measured boot loss: 45.8/s). Falsifiable prediction: the stall's
    // missed beats are REPLAYED (bounded) so the count stays wall-faithful.
    constexpr uint32_t kM6t3PhaseSlow = 1u;
    constexpr uint32_t kM6t3PhaseStall = 2u;
    constexpr uint32_t kM6t3StallMs = 600u; // ~36 wall beats, far past the grace
    std::atomic<uint32_t> g_m6t3Phase{0u};
    std::atomic<uint32_t> g_m6t3SlowWindowsLeft{0u};
    std::chrono::steady_clock::time_point g_m6t3PhaseStart{};
    std::vector<M6T2ClockSample> g_m6t3Samples;

    void m6t3SpinFor(std::chrono::steady_clock::time_point until)
    {
        while (std::chrono::steady_clock::now() < until)
        {
        }
    }

    // slow 1.5 s -> stall 600 ms -> slow 1.5 s -> stop (the T2 throttle
    // shape with the stall in the middle and no recovery-overshoot phase).
    void m6t3ThrottleBlock(uint8_t *, R5900Context *, PS2Runtime *runtime)
    {
        using clock = std::chrono::steady_clock;
        EeScheduler &scheduler = runtime->eeScheduler();
        const uint32_t phase = g_m6t3Phase.load(std::memory_order_acquire);
        const auto now = clock::now();
        g_m6t3Samples.push_back({scheduler.currentVSyncTick(), phase,
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     now.time_since_epoch())
                                     .count()});
        switch (phase)
        {
        case kM6t3PhaseSlow:
            // ~9.7% of realtime: 30k accounted cycles per ~1.05 ms wall
            // (the T2-calibrated boot/film shape).
            scheduler.accountCycles(30000u);
            m6t3SpinFor(now + std::chrono::microseconds(1050));
            if (now - g_m6t3PhaseStart >= std::chrono::milliseconds(1500))
            {
                if (g_m6t3SlowWindowsLeft.fetch_sub(1u, std::memory_order_acq_rel) <= 1u)
                {
                    g_m6t3Phase.store(3u, std::memory_order_release);
                    runtime->requestStop();
                }
                else
                {
                    g_m6t3Phase.store(kM6t3PhaseStall, std::memory_order_release);
                    g_m6t3PhaseStart = clock::now();
                }
            }
            break;
        case kM6t3PhaseStall:
            // The zero-cycle host stall: wall advances, guest cycles do
            // not, and no scheduling pass runs for the whole stretch.
            m6t3SpinFor(now + std::chrono::milliseconds(kM6t3StallMs));
            g_m6t3Phase.store(kM6t3PhaseSlow, std::memory_order_release);
            g_m6t3PhaseStart = clock::now();
            break;
        default:
            break;
        }
    }

    void setRegU32(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    }

    int32_t getRegS32(const R5900Context &ctx, int reg)
    {
        return static_cast<int32_t>(::getRegU32(&ctx, reg));
    }

    bool callSyscall(uint32_t syscallNumber, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        return dispatchNumericSyscall(syscallNumber, rdram, ctx, runtime);
    }

    void writeGuestU32(uint8_t *rdram, uint32_t addr, uint32_t value)
    {
        std::memcpy(rdram + addr, &value, sizeof(value));
    }

    uint32_t readGuestU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    uint64_t readGuestU64(const uint8_t *rdram, uint32_t addr)
    {
        uint64_t value = 0;
        std::memcpy(&value, rdram + addr, sizeof(value));
        return value;
    }

    template <typename Predicate>
    bool waitUntil(Predicate pred, std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return pred();
    }

    void cleanupRuntime(TestEnv &env)
    {
        env.runtime.requestStop();
    }

    void idleVSyncWait(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        WaitVSyncTick(rdram, ctx, runtime, -1);
    }

    void schedulerVSyncWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.setVSyncFlag(kVSyncFlagAddr, kVSyncTickAddr);
        ctx->pc = kVSyncResumePc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    void schedulerVSyncResume(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_vsyncFlag = readGuestU32(rdram, kVSyncFlagAddr);
        g_vsyncTick = readGuestU64(rdram, kVSyncTickAddr);
        g_vsyncCsr = runtime->memory().gs().csr.load(std::memory_order_acquire);
        g_resumedResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerIntcHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_dispatchTrace.push_back(2);
        g_lastIntcArg.store(getRegU32(ctx, 5), std::memory_order_relaxed);
        ctx->pc = 0u;
    }

    void schedulerIrqWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.addIrqHandler(false, 2u, kIntcHandlerPc, true, 0xCAFEu, 0u, 0u);
        ctx->pc = kIrqResumePc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    void schedulerIrqResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerIrqStackHandler(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        g_irqObservedSp = getRegU32(ctx, 29);
        const uint64_t clobber = 0u;
        std::memcpy(rdram + g_irqObservedSp - sizeof(clobber), &clobber, sizeof(clobber));
        ctx->pc = 0u;
    }

    void schedulerIrqStackWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.addIrqHandler(false,
                                2u,
                                kIrqStackHandlerPc,
                                true,
                                0u,
                                0u,
                                kIrqRegistrationSp);
        ctx->pc = kIrqStackResumePc;
        scheduler.waitVSync(scheduler.currentVSyncTick());
    }

    void schedulerIrqStackResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerISemaHandler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        runtime->eeScheduler().signalSemaphore(g_testSemaphoreId, true);
        g_dispatchTrace.push_back(4);
        ctx->pc = 0u;
    }

    void schedulerISemaDriver(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        ctx->pc = 0u;
        runtime->eeScheduler().dispatchIrq(true, 5u);
    }

    void schedulerISemaWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        g_testSemaphoreId = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(true, 5u, kISemaHandlerPc, true, 0u, 0u, 0u);

        EeThreadCreateParams driver{};
        driver.entry = kISemaDriverPc;
        driver.stack = 0x1C000u;
        driver.stackSize = 0x1000u;
        driver.priority = 10;
        const int driverId = scheduler.createThread(driver);
        scheduler.startThread(driverId, 0u, *ctx, false);

        ctx->pc = kISemaResumePc;
        scheduler.waitSemaphore(g_testSemaphoreId);
    }

    void schedulerISemaResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(5);
        g_resumedResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerVifDmaDmacHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_vifDmaDmacCause.store(getRegU32(ctx, 4), std::memory_order_relaxed);
        g_vifDmaDmacArg.store(getRegU32(ctx, 5), std::memory_order_relaxed);
        ctx->pc = 0u;
    }

    void schedulerVifDmaIntcHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        g_vifDmaIntcCause.store(getRegU32(ctx, 4), std::memory_order_relaxed);
        g_vifDmaIntcArg.store(getRegU32(ctx, 5), std::memory_order_relaxed);
        ctx->pc = 0u;
    }

    void schedulerVifDmaEntry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        scheduler.addIrqHandler(true, 1u, kVifDmaDmacHandlerPc, true, 0xAAAAu, 0u, 0u);
        scheduler.addIrqHandler(false, 5u, kVifDmaIntcHandlerPc, true, 0xBBBBu, 0u, 0u);
        scheduler.setIrqCauseEnabled(true, 1u, true);
        scheduler.setIrqCauseEnabled(false, 5u, true);

        runtime->memory().write32(kVif1ChannelMadr, 0x00003000u);
        runtime->memory().write32(kVif1ChannelQwc, 1u);
        runtime->memory().write32(kVif1ChannelChcr, 0x100u); // STR, direct mode
        runtime->drainCompletedDmacHandlers(rdram);

        ctx->pc = kVifDmaResumePc;
    }

    void schedulerVifDmaResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_vifDmaResumed.store(true, std::memory_order_relaxed);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerEventProducer(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        ctx->pc = 0u;
        runtime->eeScheduler().setEventFlag(g_testEventFlagId, 0x6u, false);
        runtime->eeScheduler().transferIfRequested(false);
    }

    void schedulerEventWait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        g_testEventFlagId = scheduler.createEventFlag(0u, 0u, 0u);

        EeThreadCreateParams producer{};
        producer.entry = kEventProducerPc;
        producer.stack = 0x1D000u;
        producer.stackSize = 0x1000u;
        producer.priority = 10;
        const int producerId = scheduler.createThread(producer);
        scheduler.startThread(producerId, 0u, *ctx, false);

        ctx->pc = kEventResumePc;
        scheduler.waitEventFlag(g_testEventFlagId, 0x2u, WEF_OR | WEF_CLEAR, kEventResultAddr);
    }

    void schedulerEventResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        g_resumedResult = getRegS32(*ctx, 2);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerTimer2Handler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(2);
        PS2Memory &memory = runtime->memory();
        memory.writeIORegister(kTimer2Mode, memory.readIORegister(kTimer2Mode) | kTimerModeEquf);
        runtime->eeScheduler().signalSemaphore(g_testSemaphoreId, true);
        ctx->pc = 0u;
    }

    void schedulerTimer2Wait(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(1);
        EeScheduler &scheduler = runtime->eeScheduler();
        g_testSemaphoreId = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(false, 11u, kTimer2HandlerPc, true, 0u, 0u, 0u);

        PS2Memory &memory = runtime->memory();
        memory.writeIORegister(kTimer2Count, 0u);
        memory.writeIORegister(kTimer2Compare, 8u);
        memory.writeIORegister(kTimer2Mode,
                               kTimerModeBusClockDiv256 | kTimerModeCue | kTimerModeCmpe | kTimerModeEquf);

        ctx->pc = kTimer2ResumePc;
        scheduler.waitSemaphore(g_testSemaphoreId);
    }

    void schedulerTimer2Resume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dispatchTrace.push_back(3);
        g_resumedResult = getRegS32(*ctx, 2);
        g_timer2Resumed.store(true, std::memory_order_release);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    // M5-T18: the timer/alarm callback under observation - it records the
    // vsync tick and the bound sp it ran at, then releases the waiter.
    void t18TimerHandler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        g_t18HandlerVsync = static_cast<uint32_t>(scheduler.currentVSyncTick());
        g_t18HandlerSp = getRegU32(ctx, 29);
        scheduler.signalSemaphore(g_t18SemaId, true);
        ctx->pc = 0u;
    }

    void t18AlarmHandler(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        g_t18HandlerVsync = static_cast<uint32_t>(scheduler.currentVSyncTick());
        g_t18HandlerSp = getRegU32(ctx, 29);
        g_t18HandlerArg = getRegU32(ctx, 6);
        scheduler.signalSemaphore(g_t18SemaId, true);
        ctx->pc = 0u;
    }

    void t18TimerPost(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        g_t18SemaId = scheduler.createSemaphore(0, 1, 0u, 0u);
        scheduler.addIrqHandler(false, 10u, kT18TimerHandlerPc, true, 0u, 0u, 0u);
        g_t18PostVsync = static_cast<uint32_t>(scheduler.currentVSyncTick());
        // The same delivery site processPendingEvents uses for the EE timer
        // counters (dispatchIrq(false, 9u + timer)).
        scheduler.dispatchIrq(false, 10u);
        ctx->pc = kT18TimerResumePc;
        scheduler.waitSemaphore(g_t18SemaId);
    }

    void t18AlarmPost(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        EeScheduler &scheduler = runtime->eeScheduler();
        g_t18SemaId = scheduler.createSemaphore(0, 1, 0u, 0u);
        (void)scheduler.setAlarm(1u, kT18AlarmHandlerPc, 0xBEEFu, 0u, 0u);
        g_t18PostVsync = static_cast<uint32_t>(scheduler.currentVSyncTick());
        ctx->pc = kT18AlarmResumePc;
        scheduler.waitSemaphore(g_t18SemaId);
    }

    void t18Resume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_t18Resumed.store(true, std::memory_order_release);
        ctx->pc = 0u;
        runtime->requestStop();
    }

    void schedulerInvocationQueueHandler(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t sp = getRegU32(ctx, 29);
        if (g_invocationQueueSp == 0u)
        {
            g_invocationQueueSp = sp;
        }
        else if (g_invocationQueueSp != sp)
        {
            g_invocationQueueSpChanged = true;
        }
        ++g_invocationQueueRuns;
        ctx->pc = 0u;
    }

    void schedulerQueueManyInvocations(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kInvocationCount = 96u;
        EeScheduler &scheduler = runtime->eeScheduler();
        for (uint32_t i = 0u; i < kInvocationCount; ++i)
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::Interrupt;
            invocation.tag = i;
            invocation.context.pc = kInvocationQueueHandlerPc;
            setRegU32(invocation.context, 31, 0u);
            scheduler.queueInvocation(std::move(invocation));
        }
        ctx->pc = kInvocationQueueResumePc;
    }

    void schedulerInvocationQueueResume(uint8_t *, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = 0u;
        runtime->requestStop();
    }
}

void register_ps2_runtime_interrupt_tests()
{
    MiniTest::Case("PS2RuntimeInterrupt", [](TestCase &tc)
    {
        tc.Run("negative interrupt-safe EE syscall ids dispatch", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kEventParamAddr = 0x1200u;
            constexpr uint32_t kStatusAddr = 0x1210u;

            const uint32_t eventParam[3] = {
                0u,
                0u,
                0u
            };
            std::memcpy(env.rdram.data() + kEventParamAddr, eventParam, sizeof(eventParam));

            R5900Context createCtx{};
            setRegU32(createCtx, 4, kEventParamAddr);
            CreateEventFlag(env.rdram.data(), &createCtx, &env.runtime);
            const int32_t eid = getRegS32(createCtx, 2);
            t.IsTrue(eid > 0, "CreateEventFlag should return a valid event id");

            R5900Context disableIntcCtx{};
            setRegU32(disableIntcCtx, 4, 2u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1B), env.rdram.data(), &disableIntcCtx, &env.runtime),
                     "negative iDisableIntc syscall id should dispatch");
            t.Equals(getRegS32(disableIntcCtx, 2), KE_OK, "negative iDisableIntc should return KE_OK");

            R5900Context enableIntcCtx{};
            setRegU32(enableIntcCtx, 4, 2u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1A), env.rdram.data(), &enableIntcCtx, &env.runtime),
                     "negative iEnableIntc syscall id should dispatch");
            t.Equals(getRegS32(enableIntcCtx, 2), KE_OK, "negative iEnableIntc should return KE_OK");

            R5900Context disableDmacCtx{};
            setRegU32(disableDmacCtx, 4, 5u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1D), env.rdram.data(), &disableDmacCtx, &env.runtime),
                     "negative iDisableDmac syscall id should dispatch");
            t.Equals(getRegS32(disableDmacCtx, 2), KE_OK, "negative iDisableDmac should return KE_OK");

            R5900Context enableDmacCtx{};
            setRegU32(enableDmacCtx, 4, 5u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x1C), env.rdram.data(), &enableDmacCtx, &env.runtime),
                     "negative iEnableDmac syscall id should dispatch");
            t.Equals(getRegS32(enableDmacCtx, 2), KE_OK, "negative iEnableDmac should return KE_OK");

            R5900Context setEventFlagCtx{};
            setRegU32(setEventFlagCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(setEventFlagCtx, 5, 0x6u);
            t.IsTrue(callSyscall(static_cast<uint32_t>(-0x53), env.rdram.data(), &setEventFlagCtx, &env.runtime),
                     "negative iSetEventFlag syscall id should dispatch");
            t.Equals(getRegS32(setEventFlagCtx, 2), KE_OK, "negative iSetEventFlag should return KE_OK");

            R5900Context referCtx{};
            setRegU32(referCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(referCtx, 5, kStatusAddr);
            ReferEventFlagStatus(env.rdram.data(), &referCtx, &env.runtime);
            t.Equals(getRegS32(referCtx, 2), KE_OK, "ReferEventFlagStatus should succeed after iSetEventFlag");
            t.Equals(readGuestU32(env.rdram.data(), kStatusAddr + 12u), 0x6u,
                     "negative iSetEventFlag should publish the requested bits");

            R5900Context deleteCtx{};
            setRegU32(deleteCtx, 4, static_cast<uint32_t>(eid));
            DeleteEventFlag(env.rdram.data(), &deleteCtx, &env.runtime);

            cleanupRuntime(env);
        });

        tc.Run("PollEventFlag WEF_CLEAR clears only matched bits", [](TestCase &t)
        {
            TestEnv env;

            constexpr uint32_t kParamAddr = 0x1400u;
            constexpr uint32_t kResBitsAddr = 0x1410u;
            constexpr uint32_t kStatusAddr = 0x1420u;

            const uint32_t eventParam[3] = {
                0u, // attr
                0u, // option
                0x7u // init bits: 0b111
            };
            std::memcpy(env.rdram.data() + kParamAddr, eventParam, sizeof(eventParam));

            R5900Context createCtx{};
            setRegU32(createCtx, 4, kParamAddr);
            CreateEventFlag(env.rdram.data(), &createCtx, &env.runtime);
            const int32_t eid = getRegS32(createCtx, 2);
            t.IsTrue(eid > 0, "CreateEventFlag should return a valid id");

            R5900Context pollCtx{};
            setRegU32(pollCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(pollCtx, 5, 0x1u);
            setRegU32(pollCtx, 6, WEF_OR | WEF_CLEAR);
            setRegU32(pollCtx, 7, kResBitsAddr);
            PollEventFlag(env.rdram.data(), &pollCtx, &env.runtime);
            t.Equals(getRegS32(pollCtx, 2), KE_OK, "PollEventFlag should succeed when condition is met");
            t.Equals(readGuestU32(env.rdram.data(), kResBitsAddr), 0x7u, "PollEventFlag should report bits before clear");

            R5900Context referCtx{};
            setRegU32(referCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(referCtx, 5, kStatusAddr);
            ReferEventFlagStatus(env.rdram.data(), &referCtx, &env.runtime);
            t.Equals(getRegS32(referCtx, 2), KE_OK, "ReferEventFlagStatus should succeed");

            Ps2EventFlagInfo info{};
            std::memcpy(&info, env.rdram.data() + kStatusAddr, sizeof(info));
            t.Equals(info.currBits, 0x6u, "WEF_CLEAR should clear only requested bits, not all bits");

            R5900Context pollMissCtx{};
            setRegU32(pollMissCtx, 4, static_cast<uint32_t>(eid));
            setRegU32(pollMissCtx, 5, 0x1u);
            setRegU32(pollMissCtx, 6, WEF_OR);
            setRegU32(pollMissCtx, 7, 0u);
            PollEventFlag(env.rdram.data(), &pollMissCtx, &env.runtime);
            t.Equals(getRegS32(pollMissCtx, 2), KE_EVF_COND,
                     "after clearing bit 0, polling for bit 0 should fail condition");

            R5900Context deleteCtx{};
            setRegU32(deleteCtx, 4, static_cast<uint32_t>(eid));
            DeleteEventFlag(env.rdram.data(), &deleteCtx, &env.runtime);
            t.Equals(getRegS32(deleteCtx, 2), KE_OK, "DeleteEventFlag should succeed");

            cleanupRuntime(env);
        });

        tc.Run("VBlank deadline resumes the waiter and publishes flag tick and FIELD atomically", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kVSyncWaitPc, schedulerVSyncWait);
            env.runtime.registerFunction(kVSyncResumePc, schedulerVSyncResume);

            g_resumedResult = -1;
            g_vsyncFlag = 0u;
            g_vsyncTick = 0u;
            g_vsyncCsr = 0u;
            R5900Context mainContext{};
            mainContext.pc = kVSyncWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.Equals(g_vsyncFlag, 1u, "VBlank start should set the registered guest flag");
            t.Equals(g_vsyncTick, 1ull, "the first centralized VBlank deadline should publish tick one");
            t.Equals(g_resumedResult, 0, "the first VBlank field should return even-field parity");
            t.Equals(g_vsyncCsr & 0x2000ull, 0x2000ull,
                     "the first VBlank should publish GS CSR.FIELD before resuming guest code");
        });

        tc.Run("VBlank IRQ invocation completes before the resumed base context", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kIrqWaitPc, schedulerIrqWait);
            env.runtime.registerFunction(kIrqResumePc, schedulerIrqResume);
            env.runtime.registerFunction(kIntcHandlerPc, schedulerIntcHandler);

            g_dispatchTrace.clear();
            g_lastIntcArg.store(0u, std::memory_order_relaxed);
            R5900Context mainContext{};
            mainContext.pc = kIrqWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3};
            t.IsTrue(g_dispatchTrace == expected,
                     "the dispatcher should run wait, IRQ frame, then the resumed base context in exact order");
            t.Equals(g_lastIntcArg.load(std::memory_order_relaxed), 0xCAFEu,
                     "the IRQ frame should receive its registered argument");
        });

        tc.Run("IRQ callbacks use an isolated invocation stack", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kIrqStackWaitPc, schedulerIrqStackWait);
            env.runtime.registerFunction(kIrqStackResumePc, schedulerIrqStackResume);
            env.runtime.registerFunction(kIrqStackHandlerPc, schedulerIrqStackHandler);

            constexpr uint64_t guard = 0x1122334455667788ull;
            std::memcpy(env.rdram.data() + kIrqRegistrationGuardAddr, &guard, sizeof(guard));
            g_irqObservedSp = 0u;

            R5900Context mainContext{};
            mainContext.pc = kIrqStackWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            uint64_t guardAfter = 0u;
            std::memcpy(&guardAfter,
                        env.rdram.data() + kIrqRegistrationGuardAddr,
                        sizeof(guardAfter));
            t.IsTrue(g_irqObservedSp != 0u && g_irqObservedSp != kIrqRegistrationSp,
                     "IRQ handler must not reuse the transient stack captured at registration");
            t.Equals(guardAfter, guard,
                     "IRQ handler stack writes must not clobber the registering thread's live frame");
        });

        tc.Run("pending async callbacks execute sequentially on a reusable invocation stack", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kInvocationQueuePc, schedulerQueueManyInvocations);
            env.runtime.registerFunction(kInvocationQueueResumePc, schedulerInvocationQueueResume);
            env.runtime.registerFunction(kInvocationQueueHandlerPc, schedulerInvocationQueueHandler);

            g_invocationQueueRuns = 0u;
            g_invocationQueueSp = 0u;
            g_invocationQueueSpChanged = false;

            R5900Context mainContext{};
            mainContext.pc = kInvocationQueuePc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);

            bool exhausted = false;
            try
            {
                env.runtime.eeScheduler().run();
            }
            catch (const std::runtime_error &error)
            {
                exhausted = std::string_view(error.what()) == "EE invocation stack space exhausted";
            }

            t.IsFalse(exhausted, "queued callbacks must not consume one invocation stack per pending item");
            t.Equals(g_invocationQueueRuns, 96u, "every queued callback should execute exactly once");
            t.IsFalse(g_invocationQueueSpChanged, "sequential callbacks should reuse the same stack depth");
        });

        tc.Run("VIF1 DMA completion dispatches channel handler and raises INTC cause 5", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kVifDmaEntryPc, schedulerVifDmaEntry);
            env.runtime.registerFunction(kVifDmaResumePc, schedulerVifDmaResume);
            env.runtime.registerFunction(kVifDmaDmacHandlerPc, schedulerVifDmaDmacHandler);
            env.runtime.registerFunction(kVifDmaIntcHandlerPc, schedulerVifDmaIntcHandler);

            g_vifDmaDmacCause.store(0u, std::memory_order_relaxed);
            g_vifDmaDmacArg.store(0u, std::memory_order_relaxed);
            g_vifDmaIntcCause.store(0u, std::memory_order_relaxed);
            g_vifDmaIntcArg.store(0u, std::memory_order_relaxed);
            g_vifDmaResumed.store(false, std::memory_order_relaxed);

            R5900Context mainContext{};
            mainContext.pc = kVifDmaEntryPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            t.IsTrue(g_vifDmaResumed.load(std::memory_order_relaxed),
                     "the main context should resume after the DMA completion");
            t.Equals(g_vifDmaDmacCause.load(std::memory_order_relaxed), 1u,
                     "the VIF1 channel (cause 1) DMAC handler should run on completion");
            t.Equals(g_vifDmaDmacArg.load(std::memory_order_relaxed), 0xAAAAu,
                     "the DMAC handler should receive its registered argument");
            t.Equals(g_vifDmaIntcCause.load(std::memory_order_relaxed), 5u,
                     "VIF1 completion should raise the VIF1 interrupt (INTC cause 5)");
            t.Equals(g_vifDmaIntcArg.load(std::memory_order_relaxed), 0xBBBBu,
                     "the INTC handler should receive its registered argument");
        });

        tc.Run("iSignalSema defers selection until IRQ return", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kISemaWaitPc, schedulerISemaWait);
            env.runtime.registerFunction(kISemaResumePc, schedulerISemaResume);
            env.runtime.registerFunction(kISemaDriverPc, schedulerISemaDriver);
            env.runtime.registerFunction(kISemaHandlerPc, schedulerISemaHandler);

            g_dispatchTrace.clear();
            g_resumedResult = -1;
            R5900Context mainContext{};
            mainContext.pc = kISemaWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3, 4, 5};
            t.IsTrue(g_dispatchTrace == expected,
                     "iSignalSema should make the waiter ready but finish the IRQ frame before selecting it");
            t.Equals(g_resumedResult, g_testSemaphoreId,
                     "the resumed waiter should receive the semaphore id from the direct FIFO handoff");
            const EeSemaphore *semaphore = env.runtime.eeScheduler().semaphore(g_testSemaphoreId);
            t.IsTrue(semaphore != nullptr, "the signaled semaphore should still exist");
            if (semaphore)
            {
                t.Equals(semaphore->count, 0, "direct handoff must not increment the semaphore count");
                t.Equals(static_cast<uint32_t>(semaphore->waiters.size()), 0u,
                         "the awakened waiter must be removed from the semaphore queue");
            }
        });

        tc.Run("event-flag completion writes observed bits before strict-priority resume", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kEventWaitPc, schedulerEventWait);
            env.runtime.registerFunction(kEventResumePc, schedulerEventResume);
            env.runtime.registerFunction(kEventProducerPc, schedulerEventProducer);

            g_dispatchTrace.clear();
            g_resumedResult = -1;
            R5900Context mainContext{};
            mainContext.pc = kEventWaitPc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            const std::vector<int> expected{1, 2, 3};
            t.IsTrue(g_dispatchTrace == expected,
                     "the higher-priority event waiter should resume at the producer scheduling point");
            t.Equals(g_resumedResult, KE_OK, "the resumed event waiter should receive KE_OK");
            t.Equals(readGuestU32(env.rdram.data(), kEventResultAddr), 0x6u,
                     "the event output should contain the bits observed before clear mode is applied");
            const EeEventFlag *flag = env.runtime.eeScheduler().eventFlag(g_testEventFlagId);
            t.IsTrue(flag != nullptr, "the event flag should still exist");
            if (flag)
            {
                t.Equals(flag->bits, 0x4u, "WEF_CLEAR should remove only the requested matched bit");
            }
        });

        tc.Run("EE Timer2 compare IRQ wakes a DelayThread-style semaphore wait", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kTimer2WaitPc, schedulerTimer2Wait);
            env.runtime.registerFunction(kTimer2ResumePc, schedulerTimer2Resume);
            env.runtime.registerFunction(kTimer2HandlerPc, schedulerTimer2Handler);

            g_dispatchTrace.clear();
            g_resumedResult = -1;
            g_timer2Resumed.store(false, std::memory_order_release);
            R5900Context mainContext{};
            mainContext.pc = kTimer2WaitPc;
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
            });

            const bool resumed = waitUntil([]()
            {
                return g_timer2Resumed.load(std::memory_order_acquire);
            }, std::chrono::milliseconds(150));
            if (!resumed)
            {
                env.runtime.requestStop();
            }
            gameThread.join();

            t.IsTrue(resumed, "Timer2 compare should dispatch INTC_TIM2 and wake the semaphore waiter");
            t.IsFalse(schedulerThrew.load(std::memory_order_acquire), "Timer2 IRQ path should not throw");
            const std::vector<int> expected{1, 2, 3};
            t.IsTrue(g_dispatchTrace == expected,
                     "Timer2 flow should run wait, interrupt handler, then the resumed thread");
            t.Equals(g_resumedResult, g_testSemaphoreId,
                     "the Timer2 handler should hand the semaphore directly to the waiter");
        });

        // M5-T18: the timer INTC callback (the 0x355300 class - the game's
        // wheel dispatcher, walk of 0x5109F0) must ride the vsync boundary,
        // never the checkpoint that raised it. The T17 capture measured the
        // unshaped delivery dying mid-movie: main 0x6A0310 out of lastGood
        // 0x355300 at vsync 197 - the dispatcher executed mid-fill at an
        // arbitrary checkpoint and the game's own dispatcher runs due wheel
        // continuations on the interrupted stack, so the callback window
        // interleaved the movie pipeline's fill window (the same ordering
        // class the T16 SIF-reply defer closed). Shaped delivery = promote
        // on the next VBlankStart, with the T16 carve pre-bind so the frame
        // never lands in main's below-poison band via the stackless
        // fallback.
        tc.Run("a timer INTC handler invocation is delivered on a vsync boundary, never inline", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kT18TimerPostPc, t18TimerPost);
            env.runtime.registerFunction(kT18TimerResumePc, t18Resume);
            env.runtime.registerFunction(kT18TimerHandlerPc, t18TimerHandler);

            g_t18Resumed.store(false, std::memory_order_release);
            g_t18PostVsync = 0u;
            g_t18HandlerVsync = 0u;
            g_t18HandlerSp = 0u;
            g_t18SemaId = 0;
            R5900Context mainContext{};
            mainContext.pc = kT18TimerPostPc;
            setRegU32(mainContext, 29, kT18MainSp);
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
            });

            const bool resumed = waitUntil([]()
            {
                return g_t18Resumed.load(std::memory_order_acquire);
            }, std::chrono::milliseconds(300));
            if (!resumed)
            {
                env.runtime.requestStop();
            }
            gameThread.join();

            t.IsTrue(resumed, "the shaped timer delivery must still wake the waiter");
            t.IsFalse(schedulerThrew.load(std::memory_order_acquire), "the timer delivery path should not throw");
            t.Equals(g_t18HandlerVsync, g_t18PostVsync + 1u,
                     "the timer INTC callback ran INLINE at the post checkpoint - the wheel "
                     "dispatcher executes mid-fill on the interrupted stack (the T17 capture: "
                     "main 0x6A0310 out of 0x355300 at vsync 197) instead of on the vsync boundary");
            t.Equals(g_t18HandlerSp, kMainInvocationCarveTop - 0x10u,
                     "the timer callback must bind on the dedicated carve, not (via the "
                     "stackless fallback) inside main's below-sp band - the T13/T14/T17 "
                     "poison band");
        });

        // M5-T18: the same shape for SetAlarm callbacks (the kernel alarm
        // wheel): the fire may happen at any checkpoint, but the callback
        // must be delivered on the next vsync boundary with the carve
        // pre-bind - Vanguard runs 4k+ alarm callbacks per session and the
        // T3 decode named "alarm/timer invocations unshaped".
        tc.Run("a SetAlarm callback is delivered on a vsync boundary, never inline", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kT18AlarmPostPc, t18AlarmPost);
            env.runtime.registerFunction(kT18AlarmResumePc, t18Resume);
            env.runtime.registerFunction(kT18AlarmHandlerPc, t18AlarmHandler);

            g_t18Resumed.store(false, std::memory_order_release);
            g_t18PostVsync = 0u;
            g_t18HandlerVsync = 0u;
            g_t18HandlerSp = 0u;
            g_t18HandlerArg = 0u;
            g_t18SemaId = 0;
            R5900Context mainContext{};
            mainContext.pc = kT18AlarmPostPc;
            setRegU32(mainContext, 29, kT18MainSp);
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
            });

            const bool resumed = waitUntil([]()
            {
                return g_t18Resumed.load(std::memory_order_acquire);
            }, std::chrono::milliseconds(300));
            if (!resumed)
            {
                env.runtime.requestStop();
            }
            gameThread.join();

            t.IsTrue(resumed, "the shaped alarm delivery must still wake the waiter");
            t.IsFalse(schedulerThrew.load(std::memory_order_acquire), "the alarm delivery path should not throw");
            t.Equals(g_t18HandlerArg, 0xBEEFu, "the alarm callback must still receive its argument");
            t.Equals(g_t18HandlerVsync, g_t18PostVsync + 1u,
                     "the SetAlarm callback ran INLINE at the fire checkpoint (vsync 0: the "
                     "64 us alarm fired long before the first VBlank) - the unshaped alarm "
                     "delivery the T17 capture implicated");
            t.Equals(g_t18HandlerSp, kMainInvocationCarveTop - 0x10u,
                     "the alarm callback must bind on the dedicated carve, not inside main's "
                     "below-sp band");
        });

        tc.Run("scheduler stop wakes an idle VSync wait without a timeout", [](TestCase &t)
        {
            TestEnv env;
            env.runtime.registerFunction(kIdleVSyncWaitPc, idleVSyncWait);

            R5900Context mainContext{};
            mainContext.pc = kIdleVSyncWaitPc;
            std::atomic<bool> schedulerDone{false};
            std::atomic<bool> schedulerThrew{false};
            std::thread gameThread([&]()
            {
                try
                {
                    env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
                    env.runtime.eeScheduler().run();
                }
                catch (...)
                {
                    schedulerThrew.store(true, std::memory_order_release);
                }
                schedulerDone.store(true, std::memory_order_release);
            });

            const bool becameIdle = waitUntil([&]() {
                const EeKernelSnapshot snapshot = env.runtime.eeScheduler().snapshot();
                return snapshot.runningThreadId == 0 &&
                       !snapshot.threads.empty() &&
                       snapshot.threads.front().waitReason == EeWaitReason::VSync;
            }, std::chrono::milliseconds(80));

            env.runtime.requestStop();
            gameThread.join();

            t.IsTrue(becameIdle, "VSync wait should leave the sole guest thread waiting");
            t.IsTrue(schedulerDone.load(std::memory_order_acquire),
                     "requestStop should wake the scheduler's event wait");
            t.IsFalse(schedulerThrew.load(std::memory_order_acquire),
                      "the scheduler stop path should not throw");

            cleanupRuntime(env);
        });

        tc.Run("M6-T2: vsync delivery tracks the wall deadline schedule, not guest cycle throughput", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kClockThrottlePc, m6t2ThrottleBlock);

            g_m6t2Phase.store(kM6t2PhaseSlow, std::memory_order_release);
            g_m6t2PhaseStart = std::chrono::steady_clock::now();
            g_m6t2Samples.clear();
            g_m6t2Samples.reserve(8192);

            R5900Context mainContext{};
            mainContext.pc = kClockThrottlePc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            using clock = std::chrono::steady_clock;
            const auto sampleTime = [](const M6T2ClockSample &s)
            {
                return clock::time_point(std::chrono::nanoseconds(s.tNs));
            };
            // Collects the delivered-vsync count and span over the tail of one
            // phase (the head is skipped: regime transitions are noisy).
            const auto phaseDelivery = [&](uint32_t phase, std::chrono::milliseconds skip,
                                           uint64_t &outCount, double &outSeconds)
            {
                clock::time_point anchor{};
                clock::time_point first{};
                clock::time_point last{};
                uint64_t firstTick = 0;
                uint64_t lastTick = 0;
                bool haveAnchor = false;
                bool haveFirst = false;
                for (const M6T2ClockSample &s : g_m6t2Samples)
                {
                    if (s.phase != phase)
                    {
                        continue;
                    }
                    const auto ts = sampleTime(s);
                    if (!haveAnchor)
                    {
                        anchor = ts;
                        haveAnchor = true;
                    }
                    if (ts - anchor < skip)
                    {
                        continue;
                    }
                    if (!haveFirst)
                    {
                        first = ts;
                        firstTick = s.vsyncTick;
                        haveFirst = true;
                    }
                    last = ts;
                    lastTick = s.vsyncTick;
                }
                outCount = lastTick - firstTick;
                outSeconds = std::chrono::duration<double>(last - first).count();
            };

            // H1: guest at ~9.7% realtime - delivery must track the deadline
            // schedule (~59.94/s => ~75 deliveries in the 1.35 s window), not
            // the cycle deficit (the T1 identity: ~5.8/s => ~8 deliveries).
            uint64_t slowCount = 0;
            double slowSeconds = 0.0;
            phaseDelivery(kM6t2PhaseSlow, std::chrono::milliseconds(150), slowCount, slowSeconds);
            t.IsTrue(slowSeconds > 1.0, "the slow-phase measurement window should span ~1.35 s of wall");
            t.IsTrue(slowCount >= 50u,
                     "a guest executing at ~10% realtime must still receive ~59.94 vsyncs per wall "
                     "second (each deadline fires when passed), not cycle% x 60 (~6/s - the T1 "
                     "cycle-gating identity that collapsed delivery to 11-12% of NTSC)");
            t.IsTrue(slowCount <= 100u,
                     "the slow regime must not over-deliver past the wall beat schedule");

            // H2: recovery at ~5x realtime - catch-up must be rate-limited to
            // the wall beat (no T1 82/s-class overshoot plateau, no multi-fire
            // burst; pre-fix this window paces off the cycle gate at ~300/s).
            uint64_t fastCount = 0;
            double fastSeconds = 0.0;
            phaseDelivery(kM6t2PhaseFast, std::chrono::milliseconds(100), fastCount, fastSeconds);
            t.IsTrue(fastSeconds > 0.7, "the fast-phase measurement window should span ~0.9 s of wall");
            const double fastRate = fastSeconds > 0.0
                                        ? static_cast<double>(fastCount) / fastSeconds
                                        : 0.0;
            t.IsTrue(fastRate <= 75.0,
                     "recovery after a stall must be rate-limited to the wall beat (~60/s): the "
                     "T1 overshoot plateaus (82/s at 1.37x) and multi-fire bursts were the "
                     "unbounded wall-debt replay");
        });

        tc.Run("M6-T3: beats missed under a zero-cycle stall are replayed bounded, not dropped", [](TestCase &t)
        {
            TestEnv env;
            t.IsTrue(env.runtime.memory().initialize(), "runtime memory initialize should succeed");
            env.runtime.registerFunction(kClockThrottlePc, m6t3ThrottleBlock);

            g_m6t3Phase.store(kM6t3PhaseSlow, std::memory_order_release);
            g_m6t3SlowWindowsLeft.store(2u, std::memory_order_release);
            g_m6t3PhaseStart = std::chrono::steady_clock::now();
            g_m6t3Samples.clear();
            g_m6t3Samples.reserve(8192);

            R5900Context mainContext{};
            mainContext.pc = kClockThrottlePc;
            env.runtime.eeScheduler().reset(env.rdram.data(), mainContext);
            env.runtime.eeScheduler().run();

            using clock = std::chrono::steady_clock;
            // Full-run count fidelity: ~3 s of slow guest plus the 600 ms
            // stall; the delivered count over the measured span must track
            // the wall schedule (~59.94/s) - the stall's missed beats come
            // back as a bounded replay instead of being dropped.
            uint64_t firstTick = 0;
            uint64_t lastTick = 0;
            clock::time_point first{};
            clock::time_point last{};
            bool haveFirst = false;
            for (const M6T2ClockSample &s : g_m6t3Samples)
            {
                const auto ts = clock::time_point(std::chrono::nanoseconds(s.tNs));
                if (!haveFirst)
                {
                    first = ts;
                    firstTick = s.vsyncTick;
                    haveFirst = true;
                }
                last = ts;
                lastTick = s.vsyncTick;
            }
            const double span = std::chrono::duration<double>(last - first).count();
            const double rate = span > 0.0
                                    ? static_cast<double>(lastTick - firstTick) / span
                                    : 0.0;
            t.IsTrue(span > 3.2, "the run should span the programmed slow-stall-slow shape (~3.6 s)");
            t.IsTrue(rate >= 56.9,
                     "a 600 ms zero-cycle stall must not cost beats: the missed wall beats are "
                     "replayed (bounded) so the delivered count stays wall-faithful (~59.94/s over "
                     "the whole span); the pre-T3 re-anchor dropped every beat past the 2-period "
                     "grace (~34 here) - the measured boot-regime loss (45.8/s)");
            t.IsTrue(rate <= 65.0,
                     "the replay must not over-deliver past the wall schedule (the T1 82/s-class "
                     "overshoot plateau)");

            // Bounded replay: the per-call replay budget spreads the burst
            // over scheduling passes - never a single-pass flood (the T1
            // unbounded multi-fire shape).
            uint64_t maxDelta = 0;
            for (size_t i = 1; i < g_m6t3Samples.size(); ++i)
            {
                const uint64_t delta = g_m6t3Samples[i].vsyncTick - g_m6t3Samples[i - 1].vsyncTick;
                if (delta > maxDelta)
                {
                    maxDelta = delta;
                }
            }
            t.IsTrue(maxDelta <= 16u,
                     "the replay must spread over scheduling passes (a few beats per pass), not "
                     "fire an unbounded multi-fire flood within one pass");

            // Post-drain sustained rate: after the replay head, delivery is
            // the ordinary wall beat again (the T2 rate-limit invariant).
            clock::time_point anchor{};
            uint64_t skipFirstTick = 0;
            uint64_t skipLastTick = 0;
            clock::time_point skipFirst{};
            clock::time_point skipLast{};
            bool haveAnchor = false;
            bool haveSkip = false;
            for (const M6T2ClockSample &s : g_m6t3Samples)
            {
                if (s.phase != kM6t3PhaseSlow)
                {
                    continue;
                }
                const auto ts = clock::time_point(std::chrono::nanoseconds(s.tNs));
                if (!haveAnchor)
                {
                    anchor = ts;
                    haveAnchor = true;
                }
                if (ts - anchor < std::chrono::milliseconds(150))
                {
                    continue;
                }
                if (!haveSkip)
                {
                    skipFirst = ts;
                    skipFirstTick = s.vsyncTick;
                    haveSkip = true;
                }
                skipLast = ts;
                skipLastTick = s.vsyncTick;
            }
            const double tailSpan = std::chrono::duration<double>(skipLast - skipFirst).count();
            const double tailRate = tailSpan > 0.0
                                        ? static_cast<double>(skipLastTick - skipFirstTick) / tailSpan
                                        : 0.0;
            t.IsTrue(tailSpan > 0.5, "the tail measurement window should span ~1.3 s of wall");
            t.IsTrue(tailRate >= 50.0 && tailRate <= 75.0,
                     "after the replay drains, delivery returns to the ordinary wall beat (~60/s, "
                     "no overshoot plateau)");
        });
    });
}
