#include "runtime/ee_scheduler.h"

#include "ps2_log.h"
#include "ps2_runtime_macros.h"

#include "ps2_runtime.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace
{
    // M5-T12: opt-in trace (PS2X_ENABLE_M5T12=1) for the main-stack callback
    // lifecycle - binds, pops, and the freeze - so a poisoned return can be
    // matched against the callback that owned the stack region.
    bool ps2xM5T12Enabled()
    {
        static const bool s_enabled = []()
        {
            const char *env = std::getenv("PS2X_ENABLE_M5T12");
            return env != nullptr && env[0] == '1';
        }();
        return s_enabled;
    }

    constexpr int KE_OK = 0;
    constexpr int KE_ERROR = -1;
    constexpr int KE_ILLEGAL_PRIORITY = -403;
    constexpr int KE_ILLEGAL_THID = -406;
    constexpr int KE_UNKNOWN_THID = -407;
    constexpr int KE_UNKNOWN_SEMID = -408;
    constexpr int KE_UNKNOWN_EVFID = -409;
    constexpr int KE_DORMANT = -413;
    constexpr int KE_NOT_DORMANT = -414;
    constexpr int KE_NOT_SUSPEND = -415;
    constexpr int KE_NOT_WAIT = -416;
    constexpr int KE_RELEASE_WAIT = -418;
    constexpr int KE_SEMA_ZERO = -419;
    constexpr int KE_SEMA_OVF = -420;
    constexpr int KE_EVF_COND = -421;
    constexpr int KE_WAIT_DELETE = -425;

    constexpr uint32_t WEF_OR = 0x01u;
    constexpr uint32_t WEF_CLEAR = 0x10u;
    constexpr uint32_t WEF_CLEAR_ALL = 0x20u;
    constexpr auto kVBlankPeriod = std::chrono::microseconds(16667);
    constexpr auto kVBlankDuration = std::chrono::microseconds(500);
    constexpr uint64_t kAlarmTickMicroseconds = 64u;
    constexpr uint32_t kDebugPublishDispatchInterval = 4096u;

    constexpr uint64_t microsecondsToEeCycles(uint64_t microseconds)
    {
        return (microseconds * EeScheduler::kEeClockHz + 999999ull) / 1000000ull;
    }

    std::chrono::nanoseconds eeCyclesToHostDuration(uint64_t cycles)
    {
        constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
        const uint64_t wholeSeconds = cycles / EeScheduler::kEeClockHz;
        const uint64_t remainingCycles = cycles % EeScheduler::kEeClockHz;
        const uint64_t remainingNanoseconds = (remainingCycles * kNanosecondsPerSecond + EeScheduler::kEeClockHz - 1u) / EeScheduler::kEeClockHz;
        return std::chrono::seconds(wholeSeconds) + std::chrono::nanoseconds(remainingNanoseconds);
    }

    constexpr uint64_t kVBlankPeriodCycles = microsecondsToEeCycles(16667u);
    constexpr uint64_t kVBlankDurationCycles = microsecondsToEeCycles(500u);
    constexpr uint64_t kAlarmTickCycles = microsecondsToEeCycles(kAlarmTickMicroseconds);

    template <typename Map>
    int allocatePositiveId(int &nextId, const Map &objects)
    {
        const int first = std::max(1, nextId);
        int candidate = first;
        do
        {
            if (!objects.contains(candidate))
            {
                nextId = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
                return candidate;
            }
            candidate = (candidate == std::numeric_limits<int>::max()) ? 1 : candidate + 1;
        } while (candidate != first);
        return 0;
    }

    // M3-T10 probe (runtime-only diagnostic): watch ONLY the 8 SDK dispatcher
    // handler slot words at 0x5109F0 (walked by sub_00355300). The v1 probe
    // also watched the adjacent tick counters 0x510A10/A14/A18, which tick
    // every frame and ate the log cap before the slots ever changed. Sampled
    // once per guest function dispatch; on any change, log the new words with
    // the writing thread's identity and the ring of recently dispatched PCs.
    // Exact per-store PCs come from [t10-write] (PS2X_T10_SLOT_WATCH).
    constexpr uint32_t kT10SlotWatchBase = 0x5109F0u;
    constexpr uint32_t kT10SlotWatchWords = 8u; // 0x5109F0..0x510A0F
    struct T10SlotWatchState
    {
        std::array<uint32_t, kT10SlotWatchWords> prev{};
        bool primed = false;
        uint32_t logged = 0u;
        std::array<uint32_t, 16> recentPcs{};
        uint32_t recentNext = 0u; // index of the OLDEST entry
        uint64_t dispatches = 0u;
    };
    T10SlotWatchState g_t10SlotWatch;

    void t10RecordDispatchPc(uint32_t pc)
    {
        g_t10SlotWatch.recentPcs[g_t10SlotWatch.recentNext] = pc;
        g_t10SlotWatch.recentNext = (g_t10SlotWatch.recentNext + 1u) % static_cast<uint32_t>(g_t10SlotWatch.recentPcs.size());
        ++g_t10SlotWatch.dispatches;
    }


    // M3-T10 probe: the host-side guest-RAM writers in this scheduler are the
    // only writers the gen store watch ([t10-write]) cannot see — and the slot
    // changes land exactly between guest branches, i.e. inside event
    // processing. Tag and log every one of them that touches the dispatcher
    // slot window 0x5109F0..0x510A10.
    void t10TagWrite(const char *tag, uint32_t address, uint32_t valueLo, uint32_t valueHi)
    {
        constexpr uint32_t kSlotBase = 0x5109F0u;
        constexpr uint32_t kSlotEnd = kSlotBase + 0x20u;
        const uint64_t writeEnd = static_cast<uint64_t>(address) + 8u;
        if (address < kSlotEnd && writeEnd > kSlotBase)
        {
            static std::atomic<uint32_t> s_logged{0};
            if (s_logged.fetch_add(1, std::memory_order_relaxed) < 256u)
            {
                std::ostringstream line;
                line << "[t10-hostwrite] " << tag
                     << " addr=0x" << std::hex << address
                     << " val=0x" << valueLo << ":" << valueHi
                     << std::dec << std::endl;
                RUNTIME_LOG(line.str());
            }
        }
    }

    void t10CheckSlotWrites(uint32_t pc, uint32_t ra, uint32_t sp, int tid, const uint8_t *rdram, uint64_t eeCycle)
    {
        if (rdram == nullptr || g_t10SlotWatch.logged >= 512u)
        {
            return;
        }
        std::array<uint32_t, kT10SlotWatchWords> cur{};
        std::memcpy(cur.data(), rdram + kT10SlotWatchBase, sizeof(cur));
        if (!g_t10SlotWatch.primed)
        {
            g_t10SlotWatch.prev = cur;
            g_t10SlotWatch.primed = true;
            return;
        }
        if (std::memcmp(g_t10SlotWatch.prev.data(), cur.data(), sizeof(cur)) == 0)
        {
            return;
        }
        ++g_t10SlotWatch.logged;
        std::ostringstream line;
        line << "[t10-slotwatch] dispatch=" << g_t10SlotWatch.dispatches
             << " eeCycle=0x" << std::hex << eeCycle
             << " tid=" << std::dec << tid
             << " pc=0x" << std::hex << pc
             << " ra=0x" << ra
             << " sp=0x" << sp
             << std::dec << " changed:";
        for (uint32_t i = 0; i < kT10SlotWatchWords; ++i)
        {
            if (cur[i] != g_t10SlotWatch.prev[i])
            {
                line << " [0x" << std::hex << (kT10SlotWatchBase + i * 4u) << "]"
                     << "0x" << cur[i] << "(was 0x" << g_t10SlotWatch.prev[i] << ")";
            }
        }
        line << std::dec << " recentPcs(oldest->newest):";
        for (const uint32_t recent : g_t10SlotWatch.recentPcs)
        {
            line << " 0x" << std::hex << recent;
        }
        line << std::dec << " execRing(oldest->newest):";
        {
            const auto &ring = ps2_log::t10_exec_ring();
            const unsigned pos = ps2_log::t10_exec_ring_pos();
            for (size_t i = 0; i < ps2_log::kT10ExecRingSize; ++i)
            {
                const char *name = ring[(pos + i) % ps2_log::kT10ExecRingSize];
                if (name != nullptr)
                {
                    line << ' ' << name;
                }
            }
        }
        line << std::endl;
        RUNTIME_LOG(line.str());
        g_t10SlotWatch.prev = cur;
    }
}

EeScheduler::EeScheduler(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

EeScheduler::~EeScheduler()
{
    requestStop();
}

void EeScheduler::reset(uint8_t *rdram, const R5900Context &mainContext)
{
    m_executorThread = std::this_thread::get_id();
    m_rdram = rdram;
    m_readyQueues = {};
    m_threads.clear();
    m_semaphores.clear();
    m_eventFlags.clear();
    m_alarms.clear();
    m_intcHandlers.clear();
    m_dmacHandlers.clear();
    m_nextThreadId = kFirstThreadId;
    m_nextInvocationThreadId = -1;
    m_nextSemaphoreId = 1;
    m_nextEventFlagId = 1;
    m_nextAlarmId = 1;
    m_nextIntcHandlerId = 1;
    m_nextDmacHandlerId = 1;
    m_intcHeadOrder = 0;
    m_intcTailOrder = 1000;
    m_dmacHeadOrder = 0;
    m_dmacTailOrder = 1000;
    m_enabledIntcMask = 0xFFFFFFFFu;
    m_enabledDmacMask = 0xFFFFFFFFu;
    m_currentThreadId = 0;
    // M5-T11 fix: the thread table (and every live main-stack invocation it
    // held) is being cleared - the freeze count dies with it.
    m_mainStackUsers = 0;
    m_lastMainFreezeLog = {};
    m_degenerateInvocationStackLogged = false;
    m_carveExhaustedLogged = false;
    m_lastCarveGuardLog = {};
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    m_insideInterrupt = false;
    m_pendingEeTimerInterrupts = 0u;
    m_eeCycle = 0u;
    m_sliceEndCycle = kDefaultTimeSliceCycles;
    m_stopRequested.store(false, std::memory_order_release);
    m_checkpointPending.store(false, std::memory_order_release);
    m_debugPublishCountdown = 0u;
    {
        std::lock_guard lock(m_eventMutex);
        m_events.clear();
        m_deadlines.clear();
        m_pendingInvocations.clear();
        m_deferredInvocations.clear();
    }
    m_eventSequence = 0;
    m_invocationSequence = 0;
    m_vsyncTick = 0;
    m_vsyncFlagAddress = 0;
    m_vsyncTickAddress = 0;
    m_gsVSyncCallback = 0;
    m_gsVSyncCallbackGp = 0;
    m_gsVSyncCallbackSp = 0;
    m_runtime.memory().gs().vsyncTick.store(0u, std::memory_order_release);
    m_runtime.memory().resetEeTimers();

    GuestThread main{};
    main.id = kMainThreadId;
    main.context = mainContext;
    main.entry = mainContext.pc;
    // $sp is live execution state, not the stable initial stack descriptor
    // returned by ReferThreadStatus. SetupThread records that metadata.
    main.stack = 0u;
    main.gp = getRegU32(&mainContext, 28);
    main.initialPriority = 0;
    main.currentPriority = 0;
    main.status = EeThreadStatus::Ready;
    m_threads.emplace(main.id, std::move(main));
    m_readyQueues[0].push_back(kMainThreadId);
    scheduleEvent(m_eeCycle + kVBlankPeriodCycles,
                  std::chrono::steady_clock::now() + kVBlankPeriod,
                  EeEvent{EeEventType::VBlankStart, 0, 0});
    publishSnapshot();
}

void EeScheduler::run()
{
    assertExecutor();
    m_running.store(true, std::memory_order_release);

    while (!m_stopRequested.load(std::memory_order_acquire))
    {
        processPendingEvents();
        if (m_stopRequested.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_currentThreadId == 0)
        {
            GuestThread *next = selectReady();
            if (!next && m_pendingInvocations.empty())
            {
                copyMainContextToRuntime();
                publishIdleDebugContext();
                publishSnapshot();
                waitForEvent();
                continue;
            }
            if (next)
            {
                makeRunning(*next);
            }
            else
            {
                GuestThread *owner = &acquireInvocationThread();
                GuestInvocation invocation = std::move(m_pendingInvocations.front());
                m_pendingInvocations.pop_front();
                owner->status = EeThreadStatus::Running;
                m_currentThreadId = owner->id;
                renewTimeSlice();
                // M5-T14: bindInvocationStack self-guards - explicit-sp
                // invocations get their main-stack residency marked there.
                bindInvocationStack(*owner, invocation);
                owner->invocations.push_back(std::move(invocation));
            }
        }

        GuestThread *running = currentThread();
        assert(running != nullptr);
        if (running->resumeCompletion)
        {
            auto completion = std::move(running->resumeCompletion);
            running->resumeCompletion = {};
            try
            {
                completion(running->activeContext());
            }
            catch (const EeDispatcherTransfer &)
            {
            }
            if (m_currentThreadId == 0)
            {
                continue;
            }
        }
        R5900Context &context = running->activeContext();
        if (m_debugPublishCountdown == 0u)
        {
            copyMainContextToRuntime();
            publishSnapshot();
            m_debugPublishCountdown = kDebugPublishDispatchInterval - 1u;
        }
        else
        {
            --m_debugPublishCountdown;
        }

        publishDebugContext(context);

        if (context.pc == 0u)
        {
            // M5-T10 probe (runtime-only, opt-in PS2X_ENABLE_M5T10=1): a
            // guest thread reached pc=0 — either it RETURNED from its entry
            // (created with ra=0) or it jalr'd a null pointer. Either way it
            // dies here (makeDormant). Dump the dying thread's registers and
            // the stack tail: if sp sits at the CreateThread stack top the
            // entry returned cleanly (shallow frames still readable below);
            // a deep sp with stale ra words names the null-call frame.
            static const bool s_m5t10Exit = []()
            {
                const char *env = std::getenv("PS2X_ENABLE_M5T10");
                return env != nullptr && env[0] == '1';
            }();
            if (s_m5t10Exit && running->id != 0)
            {
                std::ostringstream ex;
                ex << "[m5t10-exit] tid=" << running->id
                   << " entry=0x" << std::hex << running->entry
                   << " ra=0x" << getRegU32(&context, 31)
                   << " sp=0x" << getRegU32(&context, 29)
                   << " gp=0x" << getRegU32(&context, 28)
                   << " s0=0x" << getRegU32(&context, 16)
                   << " stackTop=0x" << ((running->stack != 0u) ? ((running->stack + running->stackSize) & ~0xFu) : 0u);
                const uint32_t spExit = getRegU32(&context, 29);
                if (spExit >= 0x00100000u && spExit + 0x100u <= 0x02000000u)
                {
                    for (uint32_t w = 0; w < 64u; ++w)
                    {
                        ex << " +" << std::dec << (w * 4u) << std::hex
                           << "=0x" << m_runtime.memory().read32(spExit + w * 4u);
                    }
                }
                ex << std::dec << std::endl;
                RUNTIME_LOG(ex.str());
            }
            if (!running->invocations.empty())
            {
                GuestInvocation completed = std::move(running->invocations.back());
                running->invocations.pop_back();
                if (completed.usesMainStack && m_mainStackUsers > 0)
                {
                    --m_mainStackUsers;
                }
                // M5-T12: the pop is the exact moment a main-stack callback's
                // frames stop being live - and the moment the pump may
                // descend back onto that stack region. The trace names every
                // pop so a poisoned return (pc=ra=0x100 in a frame below the
                // pump sp) can be matched against the callback that owned
                // the region.
                if (ps2xM5T12Enabled() && (completed.usesMainStack || running->id == kMainThreadId))
                {
                    RUNTIME_LOG("[m5t12-pop] tid=" << running->id
                               << " kind=" << static_cast<int>(completed.kind)
                               << " seq=" << std::dec << completed.sequence
                               << " pc=0x" << std::hex << completed.context.pc
                               << " sp=0x" << getRegU32(&completed.context, 29)
                               << " mainStack=" << completed.usesMainStack
                               << std::dec << std::endl);
                }
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, running->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
                continue;
            }
            makeDormant(*running);
            m_currentThreadId = 0;
            copyMainContextToRuntime();
            publishIdleDebugContext();
            publishSnapshot();
            continue;
        }

        if (!m_pendingInvocations.empty() && running->invocations.empty())
        {
            GuestInvocation invocation = std::move(m_pendingInvocations.front());
            m_pendingInvocations.pop_front();
            // M5-T14: bindInvocationStack self-guards - explicit-sp
            // invocations get their main-stack residency marked there.
            bindInvocationStack(*running, invocation);
            running->invocations.push_back(std::move(invocation));
            continue;
        }

        if (!m_runtime.hasFunction(context.pc))
        {
            RUNTIME_LOG("[ee-run] no-function pc=0x" << std::hex << context.pc << std::dec
                                                     << " tid=" << running->id
                                                     << " inv=" << running->invocations.size()
                                                     << "\n");
            if (!running->invocations.empty())
            {
                context.pc = 0u;
            }
            else
            {
                // Vanguard M5-T4: the park itself must be loud. A thread that
                // reaches an unmapped pc is a corruption victim (smashed
                // return address / callback pointer); dump the full register
                // window, a stack walk from sp (marking words that look like
                // code addresses = the would-be return chain) and the last
                // dispatch that still had a function, once per thread.
                if (!running->invalidJumpLogged)
                {
                    running->invalidJumpLogged = true;
                    std::ostringstream oss;
                    oss << "[ee-run:invalid-jump] tid=" << running->id
                        << " badPc=0x" << std::hex << context.pc
                        << " ra=0x" << static_cast<uint32_t>(_mm_extract_epi32(context.r[31], 0))
                        << " sp=0x" << static_cast<uint32_t>(_mm_extract_epi32(context.r[29], 0))
                        << " gp=0x" << static_cast<uint32_t>(_mm_extract_epi32(context.r[28], 0))
                        << " inv=" << std::dec << running->invocations.size()
                        << " eeCycle=0x" << std::hex << m_eeCycle
                        << " vsync=" << std::dec << m_vsyncTick
                        << " lastGood: pc=0x" << std::hex << running->lastGoodPc
                        << " ra=0x" << running->lastGoodRa
                        << " sp=0x" << running->lastGoodSp
                        << " status=" << std::dec << static_cast<int>(running->status)
                        << " waitReason=" << static_cast<int>(running->wait.reason)
                        << "\n";
                    static constexpr const char *kGprNames[32] = {
                        "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
                        "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
                        "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
                        "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
                    for (uint32_t i = 0; i < 32u; i += 4u)
                    {
                        oss << "[ee-run:invalid-jump] gpr";
                        for (uint32_t j = i; j < i + 4u; ++j)
                        {
                            oss << ' ' << kGprNames[j] << "=0x"
                                << std::hex << static_cast<uint32_t>(_mm_extract_epi32(context.r[j], 0));
                        }
                        oss << std::dec << "\n";
                    }
                    const uint32_t sp = static_cast<uint32_t>(_mm_extract_epi32(context.r[29], 0));
                    constexpr uint32_t kStackWalkWords = 48u;
                    if (m_rdram != nullptr && sp < PS2_RAM_SIZE &&
                        sp + kStackWalkWords * sizeof(uint32_t) <= PS2_RAM_SIZE)
                    {
                        for (uint32_t w = 0; w < kStackWalkWords; w += 4u)
                        {
                            oss << "[ee-run:invalid-jump] stack sp+0x" << std::hex << (w * 4u) << ":";
                            for (uint32_t k = w; k < w + 4u; ++k)
                            {
                                const uint32_t addr = sp + k * sizeof(uint32_t);
                                uint32_t word = 0u;
                                std::memcpy(&word, m_rdram + addr, sizeof(word));
                                oss << " [+" << std::hex << (k * 4u) << "]0x" << word;
                                if (m_runtime.hasFunction(word))
                                {
                                    oss << "<code>";
                                }
                            }
                            oss << std::dec << "\n";
                        }
                    }
                    else
                    {
                        oss << "[ee-run:invalid-jump] stack sp=0x" << std::hex << sp
                            << " unreadable" << std::dec << "\n";
                    }
                    // Vanguard M5-T11 probe: for a bogus badPc (0x100 class)
                    // also dump the inter-function edge ring and the async
                    // callback-stack block registry: the ring names the last
                    // real control-flow edges before the poisoned dispatch;
                    // the registry shows whether the poisoned window sits
                    // inside an async stack block (T7 allocator vs game data).
                    if (context.pc < 0x00100000u)
                    {
                        // M5-T11 fix: attribute any sp inside the host
                        // async-callback-stack band - a poisoned frame living
                        // in a reserved block is the T10 heap-trample
                        // signature (the degenerate fallback path).
                        uint32_t bandBase = 0u;
                        uint32_t bandTop = 0u;
                        if (m_runtime.asyncCallbackStackBand(bandBase, bandTop))
                        {
                            const uint32_t spNow = static_cast<uint32_t>(_mm_extract_epi32(context.r[29], 0));
                            const uint32_t lastGoodSp = running->lastGoodSp;
                            for (const uint32_t probe : {spNow, lastGoodSp})
                            {
                                if (probe != spNow && lastGoodSp == spNow)
                                {
                                    break; // duplicate of the already-attributed sp
                                }
                                if (probe >= bandBase && probe < bandTop)
                                {
                                    oss << "[ee-run:invalid-jump] sp=0x" << std::hex << probe
                                        << " inside host async-callback-stack band [0x" << bandBase
                                        << ",0x" << bandTop << ") - heap-trample risk, see M5-T10"
                                        << std::dec << "\n";
                                }
                            }
                        }
                        ps2xM5T11FormatRing(oss, 64);
                        ps2xM5T11FormatAsyncBlocks(oss);
                        oss << "\n";
                    }
                    RUNTIME_LOG(oss.str());
                }
                m_runtime.reportMissingFunction(m_rdram,
                                                &context,
                                                context.pc,
                                                context.pc,
                                                PS2Runtime::GuestBranchKind::DirectJump,
                                                "EE scheduler");
                makeDormant(*running);
                m_currentThreadId = 0;
            }
            continue;
        }
        PS2Runtime::RecompiledFunction function = m_runtime.lookupFunction(context.pc);

        t10RecordDispatchPc(context.pc);

        if (checkpointDue(kGuestDispatchCycles))
        {
            continue;
        }

        try
        {
            m_insideInterrupt = !running->invocations.empty() && running->invocations.back().kind == GuestInvocationKind::Interrupt;
            m_guestExecuting.store(true, std::memory_order_release);
            running->lastGoodPc = context.pc;
            running->lastGoodRa = static_cast<uint32_t>(_mm_extract_epi32(context.r[31], 0));
            running->lastGoodSp = static_cast<uint32_t>(_mm_extract_epi32(context.r[29], 0));
            function(m_rdram, &context, &m_runtime);
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
            t10CheckSlotWrites(context.pc,
                               getRegU32(&context, 31),
                               getRegU32(&context, 29),
                               running->id,
                               m_rdram,
                               m_eeCycle);
        }
        catch (const EeDispatcherTransfer &)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_insideInterrupt = false;
        }
        catch (...)
        {
            m_guestExecuting.store(false, std::memory_order_release);
            m_running.store(false, std::memory_order_release);
            publishSnapshot();
            throw;
        }

        // M5-T16: the pop is the INTERRUPT EPILOGUE - it belongs to this
        // dispatch, before the thread can lose the CPU. The preemption
        // below used to catch a thread whose handler had just completed
        // (active pc==0, invocation still stashed): main sat Ready with a
        // completed carve invocation on its record while the higher-priority
        // workers spun on without blocking - and the loader (main) they
        // wait for never ran again. The T16 verification run measured the
        // terminal stall (reads frozen at 226 from tick ~1680, main
        // pc=0 sp=0x1fdfff0 in 107 of 114 samples, uploads stopped at
        // idx 81 / vsync 261 of a 300 s run).
        if (m_currentThreadId != 0u)
        {
            GuestThread *finishing = currentThread();
            if (finishing != nullptr && !finishing->invocations.empty() &&
                finishing->activeContext().pc == 0u)
            {
                GuestInvocation completed = std::move(finishing->invocations.back());
                finishing->invocations.pop_back();
                if (completed.usesMainStack && m_mainStackUsers > 0)
                {
                    --m_mainStackUsers;
                }
                if (completed.onComplete)
                {
                    try
                    {
                        completed.onComplete(completed.context, finishing->activeContext());
                    }
                    catch (const EeDispatcherTransfer &)
                    {
                    }
                }
            }
        }

        processPendingEvents();
        if (m_rescheduleRequested && m_currentThreadId != 0)
        {
            GuestThread *preempted = currentThread();
            assert(preempted != nullptr);
            enqueueReady(*preempted, !m_timeSliceExpired);
            m_currentThreadId = 0;
            m_rescheduleRequested = false;
            m_timeSliceExpired = false;
        }
    }

    m_guestExecuting.store(false, std::memory_order_release);
    m_running.store(false, std::memory_order_release);
    copyMainContextToRuntime();
    publishSnapshot();
}

void EeScheduler::requestStop()
{
    m_stopRequested.store(true, std::memory_order_release);
    m_checkpointPending.store(true, std::memory_order_release);
    m_eventCv.notify_all();
}

void EeScheduler::postEvent(EeEvent event)
{
    if (event.type == EeEventType::Stop)
    {
        requestStop();
        return;
    }

    {
        std::lock_guard lock(m_eventMutex);
        m_events.push_back(event);
        m_checkpointPending.store(true, std::memory_order_release);
    }
    m_eventCv.notify_one();
}

bool EeScheduler::checkpointDue(uint32_t cycles) noexcept
{
    accountCycles(cycles);

    if (m_checkpointPending.load(std::memory_order_acquire) ||
        m_stopRequested.load(std::memory_order_acquire))
    {
        return true;
    }

    const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    if (nextEventCycle != 0u && m_eeCycle >= nextEventCycle)
    {
        m_checkpointPending.store(true, std::memory_order_release);
        return true;
    }

    if (m_eeCycle < m_sliceEndCycle)
    {
        return false;
    }

    const GuestThread *running = currentThread();
    if (running != nullptr && hasReadyAtOrAbovePriority(running->currentPriority))
    {
        m_rescheduleRequested = true;
        m_timeSliceExpired = true;
        return true;
    }

    renewTimeSlice();
    return false;
}

void EeScheduler::accountCycles(uint32_t cycles) noexcept
{
    const uint64_t elapsed = std::max<uint64_t>(1u, cycles);
    m_eeCycle += elapsed;
    const uint32_t t8Raised = m_runtime.memory().advanceEeTimers(elapsed);
    if (t8Raised != 0u)
    {
        static uint32_t s_t8RaiseCount = 0u;
        ++s_t8RaiseCount;
        if (s_t8RaiseCount <= 12u || (s_t8RaiseCount % 512u) == 0u)
        {
            RUNTIME_LOG("[t8-timer] raise #" << s_t8RaiseCount
                                             << " mask=0x" << std::hex << t8Raised
                                             << std::dec << std::endl);
        }
    }
    m_pendingEeTimerInterrupts |= t8Raised;
    m_runtime.advanceIopEeCycles(elapsed);
    if (m_pendingEeTimerInterrupts != 0u)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

bool EeScheduler::isExecutingGuest() const noexcept
{
    return m_guestExecuting.load(std::memory_order_acquire);
}

void EeScheduler::setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp)
{
    assertExecutor();
    GuestThread *target = currentThread();
    if (!target)
    {
        return;
    }

    target->stack = stack;
    target->stackSize = stackSize;
    target->gp = gp;
    publishSnapshot();
}

int EeScheduler::createThread(const EeThreadCreateParams &params)
{
    assertExecutor();
    if (params.priority < 1 || params.priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    const int id = allocateThreadId();
    if (id == 0)
    {
        return KE_ERROR;
    }

    GuestThread thread{};
    thread.id = id;
    thread.entry = params.entry;
    thread.stack = params.stack;
    thread.stackSize = params.stackSize;
    thread.gp = params.gp;
    thread.attr = params.attr;
    thread.option = params.option;
    thread.initialPriority = params.priority;
    thread.currentPriority = params.priority;
    thread.status = EeThreadStatus::Dormant;
    m_threads.emplace(id, std::move(thread));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteThread(int id, uint32_t &ownedStack)
{
    assertExecutor();
    ownedStack = 0;
    if (id <= kMainThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    auto it = m_threads.find(id);
    if (it == m_threads.end())
    {
        return KE_UNKNOWN_THID;
    }
    if (it->second.status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }
    if (it->second.ownsStack)
    {
        ownedStack = it->second.stack;
    }
    // M5-T11 fix: this path erases the record without makeDormant - release
    // any live main-stack invocation it still holds.
    releaseMainStackUsers(it->second);
    m_threads.erase(it);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe)
{
    assertExecutor();
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Dormant)
    {
        return KE_NOT_DORMANT;
    }

    target->context = R5900Context{};
    target->context.pc = target->entry;
    target->arg = arg;
    target->suspendCount = 0;
    target->wakeupCount = 0;
    target->wait = {};
    SET_GPR_U32(&target->context, 4, arg);
    SET_GPR_U32(&target->context, 28, target->gp != 0u ? target->gp : getRegU32(&caller, 28));
    const uint32_t stackTop = target->stack != 0u
                                  ? (target->stack + target->stackSize) & ~0xFu
                                  : getRegU32(&caller, 29);
    SET_GPR_U32(&target->context, 29, stackTop);
    SET_GPR_U32(&target->context, 31, 0u);
    enqueueReady(*target);
    requestPreemptionIfHigher(*target, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

[[noreturn]] void EeScheduler::exitCurrent(bool deleteThreadRecord)
{
    assertExecutor();
    GuestThread *exiting = currentThread();
    assert(exiting != nullptr);
    const int id = exiting->id;
    const uint32_t ownedStack = deleteThreadRecord && exiting->ownsStack ? exiting->stack : 0u;
    makeDormant(*exiting);
    m_currentThreadId = 0;
    if (deleteThreadRecord && id != kMainThreadId)
    {
        m_threads.erase(id);
    }
    if (ownedStack != 0u)
    {
        m_runtime.guestFree(ownedStack);
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::terminateThread(int id, uint32_t &ownedStack, bool interruptSafe)
{
    assertExecutor();
    ownedStack = 0;
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if (target->ownsStack)
    {
        ownedStack = target->stack;
        target->ownsStack = false;
    }
    makeDormant(*target);
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::suspendThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }

    ++target->suspendCount;
    switch (target->status)
    {
    case EeThreadStatus::Running:
        target->status = EeThreadStatus::Suspended;
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
        break;
    case EeThreadStatus::Ready:
        removeReady(*target);
        target->status = EeThreadStatus::Suspended;
        break;
    case EeThreadStatus::Waiting:
        target->status = EeThreadStatus::WaitingSuspended;
        break;
    case EeThreadStatus::WaitingSuspended:
    case EeThreadStatus::Suspended:
        break;
    case EeThreadStatus::Dormant:
        break;
    }
    if (interruptSafe && m_insideInterrupt)
    {
        m_rescheduleRequested = true;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::resumeThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->suspendCount == 0)
    {
        return KE_NOT_SUSPEND;
    }
    --target->suspendCount;
    if (target->suspendCount != 0)
    {
        return KE_OK;
    }
    if (target->status == EeThreadStatus::WaitingSuspended)
    {
        target->status = EeThreadStatus::Waiting;
    }
    else if (target->status == EeThreadStatus::Suspended)
    {
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::sleepCurrent()
{
    assertExecutor();
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (self->wakeupCount != 0u)
    {
        --self->wakeupCount;
        setReturnS32(&self->activeContext(), KE_OK);
        return;
    }
    blockCurrent(EeWaitState{EeWaitReason::Sleep, std::monostate{}});
}

int EeScheduler::wakeupThread(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0 || id == m_currentThreadId)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status == EeThreadStatus::Dormant)
    {
        return KE_DORMANT;
    }
    if ((target->status == EeThreadStatus::Waiting || target->status == EeThreadStatus::WaitingSuspended) &&
        target->wait.reason == EeWaitReason::Sleep)
    {
        static uint32_t s_t8WakeOk = 0u;
        ++s_t8WakeOk;
        if (s_t8WakeOk <= 16u || (s_t8WakeOk % 256u) == 0u)
        {
            RUNTIME_LOG("[t8-wake] ready tid=" << id << " #" << s_t8WakeOk << std::endl);
        }
        makeReady(*target, KE_OK, interruptSafe);
    }
    else
    {
        static uint32_t s_t8WakeCounted = 0u;
        ++s_t8WakeCounted;
        if (s_t8WakeCounted <= 16u || (s_t8WakeCounted % 256u) == 0u)
        {
            RUNTIME_LOG("[t8-wake] counted tid=" << id
                                                 << " status=" << static_cast<int>(target->status)
                                                 << " #" << s_t8WakeCounted << std::endl);
        }
        ++target->wakeupCount;
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::cancelWakeup(int id)
{
    assertExecutor();
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    const int old = static_cast<int>(target->wakeupCount);
    target->wakeupCount = 0;
    publishSnapshot();
    return old;
}

int EeScheduler::changePriority(int id, int priority, bool interruptSafe, int &oldPriority)
{
    assertExecutor();
    if (priority < 1 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }
    if (id == 0)
    {
        id = m_currentThreadId;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    oldPriority = target->currentPriority;
    if (target->status == EeThreadStatus::Ready)
    {
        removeReady(*target);
        target->currentPriority = priority;
        enqueueReady(*target);
        requestPreemptionIfHigher(*target, interruptSafe);
    }
    else
    {
        target->currentPriority = priority;
        if (target->status == EeThreadStatus::Running)
        {
            for (int p = 0; p < target->currentPriority; ++p)
            {
                if (!m_readyQueues[p].empty())
                {
                    m_rescheduleRequested = true;
                    break;
                }
            }
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::rotateReadyQueue(int priority, bool interruptSafe)
{
    assertExecutor();
    if (priority == 0)
    {
        const GuestThread *self = currentThread();
        priority = self ? self->currentPriority : 0;
    }
    if (priority < 0 || priority >= kPriorityCount)
    {
        return KE_ILLEGAL_PRIORITY;
    }

    GuestThread *self = currentThread();
    if (self && self->currentPriority == priority)
    {
        enqueueReady(*self);
        m_currentThreadId = 0;
        m_rescheduleRequested = true;
    }
    else
    {
        auto &queue = m_readyQueues[priority];
        if (queue.size() > 1u)
        {
            const int head = queue.front();
            queue.pop_front();
            queue.push_back(head);
        }
    }
    (void)interruptSafe;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::releaseWait(int id, bool interruptSafe)
{
    assertExecutor();
    if (id == 0)
    {
        return KE_ILLEGAL_THID;
    }
    GuestThread *target = thread(id);
    if (!target)
    {
        return KE_UNKNOWN_THID;
    }
    if (target->status != EeThreadStatus::Waiting && target->status != EeThreadStatus::WaitingSuspended)
    {
        return KE_NOT_WAIT;
    }
    removeFromWaitObject(*target);
    makeReady(*target, KE_RELEASE_WAIT, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::transferIfRequested(bool interruptSafe)
{
    assertExecutor();
    if (interruptSafe || m_insideInterrupt || !m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId != 0)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        enqueueReady(*self, true);
        m_currentThreadId = 0;
    }
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

int EeScheduler::createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option)
{
    assertExecutor();
    if (maxCount <= 0 || initCount < 0 || initCount > maxCount)
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextSemaphoreId, m_semaphores);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeSemaphore semaphore{};
    semaphore.id = id;
    semaphore.count = initCount;
    semaphore.maxCount = maxCount;
    semaphore.initCount = initCount;
    semaphore.attr = attr;
    semaphore.option = option;
    m_semaphores.emplace(id, std::move(semaphore));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_semaphores.find(id);
    if (it == m_semaphores.end())
    {
        return KE_UNKNOWN_SEMID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_semaphores.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return id;
}

int EeScheduler::signalSemaphore(int id, bool interruptSafe)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (!object->waiters.empty())
    {
        const int waiterId = object->waiters.front();
        object->waiters.pop_front();
        GuestThread *waiter = thread(waiterId);
        assert(waiter != nullptr);
        makeReady(*waiter, id, interruptSafe);
        publishSnapshot();
        return id;
    }
    if (object->count == object->maxCount)
    {
        return KE_SEMA_OVF;
    }
    ++object->count;
    publishSnapshot();
    return id;
}

int EeScheduler::pollSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        return KE_UNKNOWN_SEMID;
    }
    if (object->count == 0)
    {
        return KE_SEMA_ZERO;
    }
    --object->count;
    publishSnapshot();
    return id;
}

void EeScheduler::waitSemaphore(int id)
{
    assertExecutor();
    EeSemaphore *object = semaphore(id);
    if (!object)
    {
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), KE_UNKNOWN_SEMID);
        return;
    }
    if (object->count != 0)
    {
        --object->count;
        GuestThread *self = currentThread();
        assert(self != nullptr);
        setReturnS32(&self->activeContext(), id);
        publishSnapshot();
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    object->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::Semaphore, EeSemaphoreWait{id}});
}

int EeScheduler::createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option)
{
    assertExecutor();
    const int id = allocatePositiveId(m_nextEventFlagId, m_eventFlags);
    if (id == 0)
    {
        return KE_ERROR;
    }
    EeEventFlag flag{};
    flag.id = id;
    flag.attr = attr;
    flag.option = option;
    flag.initBits = initialBits;
    flag.bits = initialBits;
    m_eventFlags.emplace(id, std::move(flag));
    publishSnapshot();
    return id;
}

int EeScheduler::deleteEventFlag(int id, bool interruptSafe)
{
    assertExecutor();
    auto it = m_eventFlags.find(id);
    if (it == m_eventFlags.end())
    {
        return KE_UNKNOWN_EVFID;
    }
    std::deque<int> waiters = std::move(it->second.waiters);
    m_eventFlags.erase(it);
    for (const int threadId : waiters)
    {
        if (GuestThread *waiter = thread(threadId))
        {
            makeReady(*waiter, KE_WAIT_DELETE, interruptSafe);
        }
    }
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::setEventFlag(int id, uint32_t bits, bool interruptSafe)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits |= bits;
    finishEventWaiters(*flag, interruptSafe);
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::clearEventFlag(int id, uint32_t mask)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    flag->bits &= mask;
    publishSnapshot();
    return KE_OK;
}

int EeScheduler::pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    if (!flag)
    {
        return KE_UNKNOWN_EVFID;
    }
    if (!eventCondition(flag->bits, bits, mode))
    {
        return KE_EVF_COND;
    }
    observedBits = flag->bits;
    if ((mode & WEF_CLEAR_ALL) != 0u)
    {
        flag->bits = 0;
    }
    else if ((mode & WEF_CLEAR) != 0u)
    {
        flag->bits &= ~bits;
    }
    publishSnapshot();
    return KE_OK;
}

void EeScheduler::waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress)
{
    assertExecutor();
    EeEventFlag *flag = eventFlag(id);
    GuestThread *self = currentThread();
    assert(self != nullptr);
    if (!flag)
    {
        setReturnS32(&self->activeContext(), KE_UNKNOWN_EVFID);
        return;
    }
    if (eventCondition(flag->bits, bits, mode))
    {
        const uint32_t observed = flag->bits;
        t10TagWrite("evf-result", resultAddress, observed, 0u);
        writeGuestU32(resultAddress, observed);
        if ((mode & WEF_CLEAR_ALL) != 0u)
        {
            flag->bits = 0;
        }
        else if ((mode & WEF_CLEAR) != 0u)
        {
            flag->bits &= ~bits;
        }
        setReturnS32(&self->activeContext(), KE_OK);
        publishSnapshot();
        return;
    }
    flag->waiters.push_back(self->id);
    blockCurrent(EeWaitState{EeWaitReason::EventFlag,
                             EeEventFlagWait{id, bits, mode, resultAddress}});
}

int EeScheduler::setAlarm(uint16_t ticks,
                          uint32_t handler,
                          uint32_t argument,
                          uint32_t gp,
                          uint32_t sp)
{
    assertExecutor();
    if (handler == 0u || !m_runtime.hasFunction(handler))
    {
        return KE_ERROR;
    }
    const int id = allocatePositiveId(m_nextAlarmId, m_alarms);
    if (id == 0)
    {
        return KE_ERROR;
    }
    m_alarms.emplace(id, EeAlarm{id, ticks, handler, argument, gp, sp});
    const uint64_t tickCount = ticks == 0u ? 1u : static_cast<uint64_t>(ticks);
    scheduleEvent(m_eeCycle + tickCount * kAlarmTickCycles,
                  std::chrono::steady_clock::now() + std::chrono::microseconds(tickCount * kAlarmTickMicroseconds),
                  EeEvent{EeEventType::Alarm, static_cast<uint32_t>(id), 0});
    return id;
}

int EeScheduler::cancelAlarm(int id)
{
    assertExecutor();
    if (m_alarms.erase(id) == 0u)
    {
        return KE_ERROR;
    }
    {
        std::lock_guard lock(m_eventMutex);
        std::erase_if(m_deadlines, [id](const ScheduledEvent &scheduled)
                      { return scheduled.event.type == EeEventType::Alarm &&
                               scheduled.event.id == static_cast<uint32_t>(id); });
        updateNextDeadline();
    }
    return KE_OK;
}

void EeScheduler::queueInvocation(GuestInvocation invocation)
{
    assertExecutor();
    invocation.sequence = ++m_invocationSequence;
    m_pendingInvocations.push_back(std::move(invocation));
    m_checkpointPending.store(true, std::memory_order_release);
}

// M5-T16: the async-invocation hold-for-vsync queue. The executor posts
// here from the SIF-cmd synthesis (sceSifSendCmd); processEvent drains it
// on VBlankStart, so a deferred reply can never beat the next vsync
// boundary - the builder's fill window always closes first.
void EeScheduler::queueDeferredInvocation(GuestInvocation invocation, uint64_t vsyncsToWait)
{
    assertExecutor();
    if (vsyncsToWait == 0u)
    {
        vsyncsToWait = 1u;
    }
    std::lock_guard lock(m_eventMutex);
    m_deferredInvocations.push_back(
        DeferredInvocation{std::move(invocation), m_vsyncTick + vsyncsToWait});
}

[[noreturn]] void EeScheduler::invokeCurrent(GuestInvocation invocation)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    // M5-T14: bindInvocationStack self-guards - explicit-sp
    // invocations get their main-stack residency marked there.
    bindInvocationStack(*owner, invocation);
    invocation.sequence = ++m_invocationSequence;
    owner->invocations.push_back(std::move(invocation));
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

[[noreturn]] void EeScheduler::invokeCurrentSequence(std::vector<GuestInvocation> invocations)
{
    assertExecutor();
    GuestThread *owner = currentThread();
    assert(owner != nullptr);
    assert(!invocations.empty());
    for (auto it = invocations.rbegin(); it != invocations.rend(); ++it)
    {
        // M5-T14: bindInvocationStack self-guards (explicit-sp residency).
        {
            bindInvocationStack(*owner, *it);
        }
        it->sequence = ++m_invocationSequence;
        owner->invocations.push_back(std::move(*it));
    }
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

bool EeScheduler::hasInvocation(GuestInvocationKind kind, uint64_t tag) const
{
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        return false;
    }
    return std::any_of(owner->invocations.begin(), owner->invocations.end(),
                       [kind, tag](const GuestInvocation &invocation)
                       {
                           return invocation.kind == kind && invocation.tag == tag;
                       });
}

// M5-T11 fix: see the declaration in ee_scheduler.h. The owner's own
// suspended $sp is the base every interrupt callback would use on real
// hardware; the reserved-block path survives only for threads that have
// no guest stack at all (the internal dispatcher threads).
void EeScheduler::bindInvocationStack(GuestThread &owner, GuestInvocation &invocation)
{
    const uint32_t explicitSp = getRegU32(&invocation.context, 29);
    if (explicitSp != 0u)
    {
        // M5-T14: an explicit sp INSIDE main's stack reserve is a
        // MAIN-STACK-RESIDENT invocation - the measured shape is
        // sceGsSyncVCallback, which binds the game's vsync callback at
        // the caller's live $sp (GS.cpp records getRegU32(ctx,29) and the
        // invocation enters with it). Without the residency flag the
        // parked chain is invisible to the freeze and to every bind scan,
        // so main gets scheduled onto the same region mid-chain and the
        // two contexts overwrite each other's saved registers - the
        // movie-end poison, decoded from the 2026-10-25 captures: the
        // chain's filename-builder memcpy (0x47AB84/0x479F98) landed in
        // the cid-1 handler's saved-ra slot (dst=0x1fff260 = entry sp
        // 0x1fff280 - 0x20, the `sd ra,0x20(sp)` word of frame -0x40) and
        // the handler restored that word as $ra (pc=ra=0x1fff450, the
        // loader's level-record pointer; T13's capture held 0x100, the
        // same class of stale word).
        if (explicitSp >= PS2_RAM_SIZE - kAsyncCallbackStackMainReserve)
        {
            invocation.usesMainStack = true;
            ++m_mainStackUsers;
        }
        return; // the caller already bound an explicit stack
    }
    SET_GPR_U32(&invocation.context, 29, invocationEntrySp(owner, invocation.usesMainStack));
    // M5-T12: opt-in bind trace - which path bound which callback where.
    // The tid=1 usesMainStack=true record seen frozen at pc=0 must be
    // attributable to a bind site, not reconstructed.
    if (ps2xM5T12Enabled())
    {
        static const char *kBindKinds[] = {
            "Interrupt", "Alarm", "GsCallback", "RpcCallback",
            "SyscallOverride", "ExitHandler", "HleCall", "SifCommand",
        };
        const size_t kindIndex = static_cast<size_t>(invocation.kind);
        const uint32_t boundSp = getRegU32(&invocation.context, 29);
        uint32_t bandBase = 0u;
        uint32_t bandTop = 0u;
        const bool inBand = m_runtime.asyncCallbackStackBand(bandBase, bandTop);
        RUNTIME_LOG("[m5t12-bind] owner=" << owner.id
                   << " kind=" << (kindIndex < sizeof(kBindKinds) / sizeof(kBindKinds[0]) ? kBindKinds[kindIndex] : "?")
                   << " seq=" << std::dec << invocation.sequence
                   << " entry=0x" << std::hex << invocation.context.pc
                   << " sp=0x" << boundSp
                   << " ownSp=0x" << getRegU32(&owner.activeContext(), 29)
                   << " mainStack=" << invocation.usesMainStack
                   << " carve=" << (boundSp >= kMainInvocationCarveBase && boundSp < kMainInvocationCarveTop ? "1" : "0")
                   << " band=" << (inBand ? "1" : "0")
                   << std::dec << std::endl);
    }
}

uint32_t EeScheduler::invocationEntrySp(const GuestThread &owner, bool &usedMainStack)
{
    assertExecutor();
    // The ACTIVE context: the thread's own suspended $sp, or, while an
    // invocation chain is parked/aborted mid-frame, the innermost frame's
    // $sp - a nested callback must push below it, exactly like a second
    // interrupt taken inside a handler on real hardware.
    const uint32_t ownSp = getRegU32(&owner.activeContext(), 29);
    usedMainStack = false;
    if (ownSp != 0u)
    {
        // M5-T15: MAIN-OWNED invocations bind on the dedicated defended
        // carve - NOT below main's live $sp. The T12 rule above was
        // hardware-faithful (an interrupt frame descends below the
        // interrupted $sp on the same stack) and put every main-owned
        // cid-1 handler INSIDE the below-sp band the game's loader
        // filename builder (running on tid3) fills as scratch: the T13/T14
        // captures measured the builder's memcpy landing DIRECTLY in the
        // handler's saved-ra slot (dst=0x1fff260/0x1fff4a0 = entry sp
        // 0x1fff280/0x1fff4c0 - 0x20) - the intermittent pc=ra=0x100
        // movie-end death. The carve [kMainInvocationCarveBase,
        // kMainInvocationCarveTop) sits inside main's DECLARED stack
        // (SetupThread stack=-1, size=0x28000 -> [0x1FD7FF0,0x1FFFFF0]),
        // below main's observed live floor (0x1FFB840 across every m5 run)
        // and above the game's static arena top (0x1FD81C0): the game's
        // scratch never reaches (0 measured [t11-copy] dsts below
        // 0x1FFB900). The top keeps 0xF120 under main's DEEPEST measured
        // park - 0x1FEF120 at the movie-end boundary (the first carve top
        // 0x1FF8000 overlapped THAT park; the carve-guard caught main inside
        // the band and the constants were re-derived). Main is parked while a carve frame runs, so
        // the T11 freeze does NOT apply to this path (no starvation
        // concern); foreign-owner invocations keep the T11/T12
        // owner-stack/freeze semantics unchanged. selectReady's
        // carve-guard watches the known failure mode (main descending
        // into the band) loudly.
        if (owner.id == kMainThreadId)
        {
            return carveInvocationSp();
        }
        uint32_t floor = 0x00100000u;
        if (owner.stack != 0u && owner.stackSize != 0u)
        {
            floor = owner.stack; // CreateThread stack base (low address)
        }
        else
        {
            // main thread: kernel-default 32 KiB stack anchored at the top of RAM
            floor = PS2_RAM_SIZE - kAsyncCallbackStackMainReserve;
        }
        if (ownSp > floor && (ownSp - floor) >= 0x1000u)
        {
            return ownSp;
        }
    }
    // Stackless owner (internal dispatcher thread): the callback still
    // interrupted a real guest context on hardware - main's, per the
    // published snapshot. Run it below the MAIN thread's suspended $sp AND
    // below every other live main-stack callback frame, so no host block
    // is carved out of the band the game's malloc arena lives in. The
    // reserved block survives only as a degenerate last resort.
    if (const GuestThread *mainThread = thread(kMainThreadId))
    {
        uint32_t base = getRegU32(&mainThread->activeContext(), 29);
        for (auto &[id, candidate] : m_threads)
        {
            for (const GuestInvocation &live : candidate.invocations)
            {
                if (!live.usesMainStack)
                {
                    continue;
                }
                const uint32_t liveSp = getRegU32(&live.context, 29);
                if (liveSp != 0u && (base == 0u || liveSp < base))
                {
                    base = liveSp;
                }
            }
        }
        const uint32_t mainFloor = PS2_RAM_SIZE - kAsyncCallbackStackMainReserve;
        if (base != 0u && base > mainFloor && (base - mainFloor) >= 0x1000u)
        {
            usedMainStack = true;
            ++m_mainStackUsers;
            return base;
        }
    }
    // Degenerate last resort (M5-T10): neither the owner's stack nor main's
    // leaves the 4 KiB deep-callback headroom - the only binding left is a
    // 16 KiB host-reserved block marching DOWN from 0x1FF8000 through the
    // band the game's malloc arena fills (the exact T10 trample). Loud and
    // one-shot so a resurrected trample is attributable in minutes.
    const uint32_t degenerateTop = invocationStackTop();
    if (!m_degenerateInvocationStackLogged)
    {
        m_degenerateInvocationStackLogged = true;
        RUNTIME_LOG("[ee-sched:degenerate-invocation-stack] owner and main stack both exhausted: binding a 16 KiB async-stack block at [0x"
                    << std::hex << (degenerateTop - 0x4000u) << ",0x" << (degenerateTop + 0x10u)
                    << ") - this band is the game's live heap (heap-trample risk, see M5-T10)"
                    << std::dec << std::endl);
    }
    return degenerateTop;
}

// M5-T15: the dedicated defended carve - see the declaration in
// ee_scheduler.h and the constants in ps2_runtime.h.
uint32_t EeScheduler::carveInvocationSp()
{
    assertExecutor();
    // The next frame binds at the lowest live carve sp (the innermost live
    // frame's adjusted $sp - a nested callback pushes below it, exactly
    // like a second interrupt taken inside a handler), or at the carve top
    // minus the 0x10 word-slot convention when the carve is idle. Bind
    // order needs no separate cursor state: sequential deliveries reuse
    // the same sp safely (strict LIFO lifetimes), nested ones see the
    // innermost live frame's CURRENT sp in its invocation record.
    uint32_t cursor = kMainInvocationCarveTop - 0x10u;
    for (const auto &entry : m_threads)
    {
        for (const GuestInvocation &live : entry.second.invocations)
        {
            const uint32_t liveSp = getRegU32(&live.context, 29);
            if (liveSp >= kMainInvocationCarveBase && liveSp < kMainInvocationCarveTop &&
                liveSp < cursor)
            {
                cursor = liveSp;
            }
        }
    }
    if (cursor < kMainInvocationCarveBase + 0x400u)
    {
        // Exhausted: >90 KiB of simultaneous carve frames - never measured
        // at 4-6 KiB per callback. The design's named alternative
        // (hold-at-park) is unimplementable on the synchronous
        // invokeCurrent delivery sites without re-wedging the T12 flow
        // (the risk the T14 report already named), so the bind CLAMPS to
        // the carve base: the overflow frame descends into the dead band
        // below the carve base - still inside main's declared stack, still
        // below main's live floor, still above the game's arena top - and
        // the loud one-shot log makes the state attributable in minutes.
        cursor = kMainInvocationCarveBase;
        if (!m_carveExhaustedLogged)
        {
            m_carveExhaustedLogged = true;
            RUNTIME_LOG("[ee-sched:carve-exhausted] the dedicated invocation carve [0x"
                        << std::hex << kMainInvocationCarveBase << ",0x" << kMainInvocationCarveTop
                        << ") is full of live frames - clamping the bind to the carve base "
                        "(would need >90 KiB of simultaneous main-owned callback frames)"
                        << std::dec << std::endl);
        }
    }
    return cursor;
}

// M5-T15: the carve guard - see the declaration in ee_scheduler.h.
void EeScheduler::guardMainCarve(uint32_t mainSp)
{
    // Scope: main's DECLARED stack region below the carve top. Main's
    // measured parks: 0x1FFB840 steady-state across every m5 run, and
    // 0x1FEF120 at the movie-end boundary (measured by THIS guard in the
    // T15 verification run). A sp under the carve top is a real descent into
    // the carve band (or past it) - the design's known failure mode.
    if (mainSp == 0u || mainSp >= kMainInvocationCarveTop ||
        mainSp < kMainInvocationStackFloor)
    {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (m_lastCarveGuardLog != std::chrono::steady_clock::time_point{} &&
        now - m_lastCarveGuardLog < std::chrono::seconds(2))
    {
        return;
    }
    m_lastCarveGuardLog = now;
    RUNTIME_LOG("[ee-sched:carve-guard] main sp 0x" << std::hex << mainSp
                << " entered the dedicated invocation carve band [0x"
                << kMainInvocationCarveBase << ",0x" << kMainInvocationCarveTop
                << ") or below it - main descended into the carve (the T15 design's "
                   "known failure mode); live carve frames follow"
                << std::dec << std::endl);
    for (const auto &entry : m_threads)
    {
        for (const GuestInvocation &live : entry.second.invocations)
        {
            const uint32_t liveSp = getRegU32(&live.context, 29);
            if (liveSp >= kMainInvocationCarveBase && liveSp < kMainInvocationCarveTop)
            {
                RUNTIME_LOG("[ee-sched:carve-guard]   live carve frame tid="
                            << entry.first
                            << " kind=" << static_cast<int>(live.kind)
                            << " seq=" << std::dec << live.sequence
                            << " pc=0x" << std::hex << live.context.pc
                            << " sp=0x" << liveSp << std::dec << std::endl);
            }
        }
    }
}

uint32_t EeScheduler::invocationStackTop()
{
    assertExecutor();
    const GuestThread *owner = currentThread();
    if (!owner)
    {
        throw std::logic_error("EE invocation stack requested without a current guest context");
    }
    const size_t depth = owner ? owner->invocations.size() : 0u;
    const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(owner->id)) << 32u) |
                         static_cast<uint32_t>(depth);
    const auto existing = m_invocationStackTops.find(key);
    if (existing != m_invocationStackTops.end())
    {
        return existing->second;
    }
    constexpr uint32_t kInvocationStackSize = 0x4000u;
    const uint32_t top = m_runtime.reserveAsyncCallbackStack(kInvocationStackSize, 16u);
    if (top == 0u)
    {
        throw std::runtime_error("EE invocation stack space exhausted");
    }
    m_invocationStackTops.emplace(key, top);
    return top;
}

int EeScheduler::addIrqHandler(bool dmac,
                               uint32_t cause,
                               uint32_t handler,
                               bool append,
                               uint32_t argument,
                               uint32_t gp,
                               uint32_t sp)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    int &nextId = dmac ? m_nextDmacHandlerId : m_nextIntcHandlerId;
    const int id = allocatePositiveId(nextId, handlers);
    if (id == 0)
    {
        return KE_ERROR;
    }
    int &head = dmac ? m_dmacHeadOrder : m_intcHeadOrder;
    int &tail = dmac ? m_dmacTailOrder : m_intcTailOrder;
    handlers.emplace(id,
                     EeIrqHandler{id,
                                  cause,
                                  handler,
                                  argument,
                                  gp,
                                  sp,
                                  true,
                                  append ? ++tail : --head});
    return id;
}

int EeScheduler::removeIrqHandler(bool dmac, uint32_t cause, int id)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end() && it->second.cause == cause)
    {
        handlers.erase(it);
    }
    return KE_OK;
}

int EeScheduler::setIrqHandlerEnabled(bool dmac, int id, bool enabled)
{
    assertExecutor();
    auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    auto it = handlers.find(id);
    if (it != handlers.end())
    {
        it->second.enabled = enabled;
    }
    return KE_OK;
}

int EeScheduler::setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled)
{
    assertExecutor();
    if (cause < 32u)
    {
        uint32_t &mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
        if (enabled)
        {
            mask |= 1u << cause;
        }
        else
        {
            mask &= ~(1u << cause);
        }
    }
    return KE_OK;
}

void EeScheduler::dispatchIrq(bool dmac, uint32_t cause)
{
    assertExecutor();
    const uint32_t mask = dmac ? m_enabledDmacMask : m_enabledIntcMask;
    if (cause < 32u && (mask & (1u << cause)) == 0u)
    {
        if (!dmac && cause >= 9u && cause <= 12u)
        {
            static uint32_t s_t8MaskedTimer = 0u;
            ++s_t8MaskedTimer;
            if (s_t8MaskedTimer <= 8u || (s_t8MaskedTimer % 512u) == 0u)
            {
                RUNTIME_LOG("[t8-irq] timer cause " << cause
                                                    << " SKIPPED by mask (enabled=0x"
                                                    << std::hex << mask << std::dec
                                                    << ") #" << s_t8MaskedTimer << std::endl);
            }
        }
        return;
    }
    if (!dmac && cause >= 9u && cause <= 12u)
    {
        static uint32_t s_t8DispatchedTimer = 0u;
        ++s_t8DispatchedTimer;
        if (s_t8DispatchedTimer <= 12u || (s_t8DispatchedTimer % 512u) == 0u)
        {
            RUNTIME_LOG("[t8-irq] timer cause " << cause
                                                << " dispatched #" << s_t8DispatchedTimer << std::endl);
        }
    }
    const auto &handlers = dmac ? m_dmacHandlers : m_intcHandlers;
    std::vector<EeIrqHandler> matching;
    for (const auto &[id, handler] : handlers)
    {
        (void)id;
        if (handler.enabled && handler.cause == cause && handler.handler != 0u &&
            m_runtime.hasFunction(handler.handler))
        {
            matching.push_back(handler);
        }
    }
    std::sort(matching.begin(), matching.end(), [](const EeIrqHandler &left, const EeIrqHandler &right)
              { return left.order < right.order; });
    for (const EeIrqHandler &handler : matching)
    {
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Interrupt;
        invocation.context.pc = handler.handler;
        SET_GPR_U32(&invocation.context, 4, cause);
        SET_GPR_U32(&invocation.context, 5, handler.argument);
        SET_GPR_U32(&invocation.context, 28, handler.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        // M5-T18: the EE timer-counter callbacks (causes 9-12, the game's
        // wheel dispatcher class - Vanguard's 0x355300 walks its timer
        // wheel 0x5109F0 from here) must not ride the checkpoint that
        // raised them. The T17 capture measured the unshaped delivery
        // dying mid-movie: the dispatcher executed at an arbitrary
        // checkpoint (mid-fill), and the game's own dispatcher runs due
        // wheel continuations on the INTERRUPTED stack - the callback
        // window interleaved the movie pipeline's fill window (main
        // 0x6a0310 out of lastGood 0x355300 at vsync 197). Hardware gives
        // these callbacks at least a frame of slack (the wheel's due
        // windows are ms-scale); deliver them on the next VBlank boundary
        // like the T16 SIF replies. The VBlankStart promote pre-binds the
        // carve, so the frame never lands in main's below-sp band via the
        // stackless fallback. VBlank causes (2/3) already fire ON the
        // boundary - delivered inline, unchanged.
        if (!dmac && cause >= 9u && cause <= 12u)
        {
            queueDeferredInvocation(std::move(invocation), 1u);
        }
        else
        {
            queueInvocation(std::move(invocation));
        }
    }
}

void EeScheduler::setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress)
{
    assertExecutor();
    m_vsyncFlagAddress = flagAddress;
    m_vsyncTickAddress = tickAddress;
    t10TagWrite("vsync-reg-flag", flagAddress, 0u, 0u);
    writeGuestU32(flagAddress, 0u);
    if (tickAddress != 0u)
    {
        const uint32_t physical = tickAddress & 0x1FFFFFFFu;
        if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
        {
            const uint64_t zero = 0u;
            t10TagWrite("vsync-reg-tick", physical, 0u, 0u);
            std::memcpy(m_rdram + physical, &zero, sizeof(zero));
        }
    }
}

uint64_t EeScheduler::currentVSyncTick() const noexcept
{
    return m_vsyncTick;
}

uint32_t EeScheduler::setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp)
{
    assertExecutor();
    (void)sp;
    const uint32_t previous = m_gsVSyncCallback;
    m_gsVSyncCallback = callback;
    m_gsVSyncCallbackGp = gp;
    m_gsVSyncCallbackSp = 0u;
    return previous;
}

[[noreturn]] void EeScheduler::waitVSync(uint64_t afterTick, int fixedResult, std::function<void(R5900Context &)> completion)
{
    blockCurrent(EeWaitState{
        EeWaitReason::VSync,
        EeVSyncWait{afterTick, fixedResult},
        std::move(completion)});
}

void EeScheduler::completeVSync(uint64_t tick)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status == EeThreadStatus::Waiting || candidate.status == EeThreadStatus::WaitingSuspended) &&
            candidate.wait.reason == EeWaitReason::VSync &&
            std::get<EeVSyncWait>(candidate.wait.payload).afterTick < tick)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        const EeVSyncWait wait = std::get<EeVSyncWait>(waiter->wait.payload);
        const int result = wait.fixedResult >= 0
                               ? wait.fixedResult
                               : static_cast<int>((tick - 1u) & 1u);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

void EeScheduler::completeExternalWait(uint32_t type, uint64_t token, int result)
{
    assertExecutor();
    std::vector<int> completed;
    for (const auto &[id, candidate] : m_threads)
    {
        if ((candidate.status != EeThreadStatus::Waiting && candidate.status != EeThreadStatus::WaitingSuspended) ||
            (candidate.wait.reason != EeWaitReason::External &&
             candidate.wait.reason != EeWaitReason::Mpeg))
        {
            continue;
        }
        const auto &external = std::get<EeExternalWait>(candidate.wait.payload);
        if (external.type == type && external.token == token)
        {
            completed.push_back(id);
        }
    }
    std::sort(completed.begin(), completed.end());
    for (const int id : completed)
    {
        GuestThread *waiter = thread(id);
        assert(waiter != nullptr);
        makeReady(*waiter, result, false);
    }
    publishSnapshot();
}

[[noreturn]] void EeScheduler::waitExternal(EeWaitReason reason,
                                            uint32_t type,
                                            uint64_t token,
                                            std::function<void(R5900Context &)> completion)
{
    EeWaitState wait{reason, EeExternalWait{type, token}, std::move(completion)};
    blockCurrent(std::move(wait));
}

GuestThread *EeScheduler::thread(int id)
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

const GuestThread *EeScheduler::thread(int id) const
{
    auto it = m_threads.find(id);
    return it == m_threads.end() ? nullptr : &it->second;
}

EeSemaphore *EeScheduler::semaphore(int id)
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

const EeSemaphore *EeScheduler::semaphore(int id) const
{
    auto it = m_semaphores.find(id);
    return it == m_semaphores.end() ? nullptr : &it->second;
}

EeEventFlag *EeScheduler::eventFlag(int id)
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

const EeEventFlag *EeScheduler::eventFlag(int id) const
{
    auto it = m_eventFlags.find(id);
    return it == m_eventFlags.end() ? nullptr : &it->second;
}

GuestThread *EeScheduler::currentThread()
{
    return thread(m_currentThreadId);
}

const GuestThread *EeScheduler::currentThread() const
{
    return thread(m_currentThreadId);
}

int EeScheduler::currentThreadId() const noexcept
{
    return m_currentThreadId;
}

R5900Context *EeScheduler::currentContext()
{
    GuestThread *self = currentThread();
    return self ? &self->activeContext() : nullptr;
}

uint8_t *EeScheduler::rdram() const noexcept
{
    return m_rdram;
}

void EeScheduler::bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram)
{
    if (m_executorThread == std::thread::id{})
    {
        reset(rdram, ctx);
        GuestThread *main = selectReady();
        assert(main != nullptr);
        makeRunning(*main);
        return;
    }
    assertExecutor();
    m_rdram = rdram;
    if (m_currentThreadId == 0)
    {
        GuestThread *main = thread(kMainThreadId);
        assert(main != nullptr);
        assert(main->status == EeThreadStatus::Ready);
        removeReady(*main);
        makeRunning(*main);
    }
}

EeKernelSnapshot EeScheduler::snapshot() const
{
    std::lock_guard lock(m_snapshotMutex);
    return m_snapshot;
}

void EeScheduler::publishSnapshot()
{
    EeKernelSnapshot next{};
    next.sequence = ++m_snapshotSequence;
    next.eeCycle = m_eeCycle;
    next.sliceEndCycle = m_sliceEndCycle;
    next.nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
    next.runningThreadId = m_currentThreadId;
    next.threads.reserve(m_threads.size());
    for (const auto &[id, item] : m_threads)
    {
        if (id < 0)
        {
            continue;
        }
        EeThreadSnapshot snapshot{};
        snapshot.id = id;
        const R5900Context &context = item.activeContext();
        snapshot.pc = context.pc;
        snapshot.ra = getRegU32(&context, 31);
        snapshot.sp = getRegU32(&context, 29);
        snapshot.contextGp = getRegU32(&context, 28);
        snapshot.entry = item.entry;
        snapshot.stack = item.stack;
        snapshot.stackSize = item.stackSize;
        snapshot.gp = item.gp;
        snapshot.initialPriority = item.initialPriority;
        snapshot.currentPriority = item.currentPriority;
        snapshot.status = item.status;
        snapshot.waitReason = item.wait.reason;
        snapshot.waitId = waitObjectId(item.wait);
        if (const EeExternalWait *external = std::get_if<EeExternalWait>(&item.wait.payload))
        {
            snapshot.waitType = external->type;
            snapshot.waitToken = external->token;
        }
        else if (const EeVSyncWait *vsync = std::get_if<EeVSyncWait>(&item.wait.payload))
        {
            snapshot.waitAfterTick = vsync->afterTick;
        }
        snapshot.suspendCount = item.suspendCount;
        snapshot.wakeupCount = item.wakeupCount;
        snapshot.invocationDepth = static_cast<uint32_t>(item.invocations.size());
        next.threads.push_back(snapshot);
    }
    std::sort(next.threads.begin(), next.threads.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.semaphores.reserve(m_semaphores.size());
    for (const auto &[id, item] : m_semaphores)
    {
        next.semaphores.push_back(EeSemaphoreSnapshot{id,
                                                      item.count,
                                                      item.maxCount,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.semaphores.begin(), next.semaphores.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    next.eventFlags.reserve(m_eventFlags.size());
    for (const auto &[id, item] : m_eventFlags)
    {
        next.eventFlags.push_back(EeEventFlagSnapshot{id,
                                                      item.bits,
                                                      item.initBits,
                                                      item.attr,
                                                      static_cast<uint32_t>(item.waiters.size())});
    }
    std::sort(next.eventFlags.begin(), next.eventFlags.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    // Bring-up diagnostics (Vanguard M3): publish the IRQ handler tables so
    // the tick dump names which guest ISRs the steady state actually runs.
    for (const auto &[id, item] : m_intcHandlers)
    {
        next.intcHandlers.push_back(EeIrqHandlerSnapshot{id,
                                                         item.cause,
                                                         item.handler,
                                                         item.argument,
                                                         item.enabled});
    }
    std::sort(next.intcHandlers.begin(), next.intcHandlers.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    for (const auto &[id, item] : m_dmacHandlers)
    {
        next.dmacHandlers.push_back(EeIrqHandlerSnapshot{id,
                                                         item.cause,
                                                         item.handler,
                                                         item.argument,
                                                         item.enabled});
    }
    std::sort(next.dmacHandlers.begin(), next.dmacHandlers.end(), [](const auto &left, const auto &right)
              { return left.id < right.id; });
    {
        std::lock_guard lock(m_snapshotMutex);
        m_snapshot = std::move(next);
    }
}

void EeScheduler::assertExecutor() const
{
    assert(m_executorThread == std::this_thread::get_id());
}

int EeScheduler::allocateThreadId()
{
    for (int attempts = 0; attempts <= kLastThreadId - kFirstThreadId; ++attempts)
    {
        const int candidate = m_nextThreadId;
        m_nextThreadId = candidate == kLastThreadId ? kFirstThreadId : candidate + 1;
        if (!m_threads.contains(candidate))
        {
            return candidate;
        }
    }
    return 0;
}

GuestThread &EeScheduler::acquireInvocationThread()
{
    for (auto &[id, candidate] : m_threads)
    {
        if (id < 0 && candidate.status == EeThreadStatus::Dormant && candidate.invocations.empty())
        {
            return candidate;
        }
    }

    GuestThread dispatcher{};
    dispatcher.id = m_nextInvocationThreadId--;
    dispatcher.initialPriority = 0;
    dispatcher.currentPriority = 0;
    dispatcher.status = EeThreadStatus::Dormant;
    return m_threads.emplace(dispatcher.id, std::move(dispatcher)).first->second;
}

void EeScheduler::enqueueReady(GuestThread &item, bool front)
{
    assert(item.currentPriority >= 0 && item.currentPriority < kPriorityCount);
    item.status = EeThreadStatus::Ready;
    auto &queue = m_readyQueues[item.currentPriority];
    if (front)
    {
        queue.push_front(item.id);
    }
    else
    {
        queue.push_back(item.id);
    }
}

void EeScheduler::removeReady(GuestThread &item)
{
    if (item.status != EeThreadStatus::Ready)
    {
        return;
    }
    auto &queue = m_readyQueues[item.currentPriority];
    auto it = std::find(queue.begin(), queue.end(), item.id);
    assert(it != queue.end());
    queue.erase(it);
}

GuestThread *EeScheduler::selectReady()
{
    // M5-T15: the carve guard - main's own suspended sp is sampled at every
    // scheduling decision here (the freeze log samples the same thread
    // table); a sp under the carve top means main descended into the
    // dedicated carve band - loud, rate-limited (M3-T4 rule).
    if (const GuestThread *mainThread = thread(kMainThreadId))
    {
        guardMainCarve(getRegU32(&mainThread->context, 29));
    }
    for (auto &queue : m_readyQueues)
    {
        for (size_t i = 0; i < queue.size(); ++i)
        {
            const int id = queue[i];
            // M5-T11: while a callback is live on the main thread's stack, the
            // main pump must not run - it would descend into the callback's
            // frames (both contexts end up poisoned, pc=ra=0x100). Skip main
            // and KEEP SCANNING the same queue: the thread that drains the
            // callback (e.g. the loader consumer) may sit BEHIND main at the
            // same priority - freezing the whole queue would starve it and
            // wedge the freeze forever.
            // M5-T12: only a FOREIGN owner holds main out of the schedule. A
            // MAIN-owned main-stack invocation (bound on main's own stack) is
            // exactly the hardware interrupt frame: main must be scheduled to
            // run/complete it (activeContext() is the invocation, so the pump
            // is unreachable until it pops), and the completed pc=0 pop is
            // the only thing that releases the count - freezing main on its
            // own callback wedged the whole post-movie session.
            if (m_mainStackUsers > 0 && id == kMainThreadId && mainStackHeldByForeignOwner())
            {
                logMainStackFreeze();
                continue;
            }
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(i));
            GuestThread *selected = thread(id);
            assert(selected != nullptr);
            assert(selected->status == EeThreadStatus::Ready);
            return selected;
        }
    }
    return nullptr;
}

// M5-T12: the freeze must only hold main out of the schedule while a
// FOREIGN thread's invocation lives on main's stack (a stackless dispatcher
// thread's fallback bind - scheduling main would run the pump down into
// those foreign frames). Main-owned invocations never qualify: main's
// dispatch is what runs and completes them.
bool EeScheduler::mainStackHeldByForeignOwner() const
{
    for (const auto &entry : m_threads)
    {
        if (entry.first == kMainThreadId)
        {
            continue;
        }
        for (const GuestInvocation &invocation : entry.second.invocations)
        {
            if (invocation.usesMainStack)
            {
                return true;
            }
        }
    }
    return false;
}

// M5-T11 fix: a wedged freeze must be diagnosable in minutes, not a silent
// idle - rate-limited (once per 2 s) while the freeze actively skips main.
void EeScheduler::logMainStackFreeze()
{
    const auto now = std::chrono::steady_clock::now();
    if (m_lastMainFreezeLog != std::chrono::steady_clock::time_point{} &&
        now - m_lastMainFreezeLog < std::chrono::seconds(2))
    {
        return;
    }
    m_lastMainFreezeLog = now;
    RUNTIME_LOG("[ee-sched:main-frozen] " << m_mainStackUsers
                                           << " live main-stack callback(s): main thread held out of schedule"
                                           << std::endl);
    // M5-T12: name the parked callback(s) - kind, owning thread, and the
    // guest pc the invocation sits at - so one log names the exact function
    // holding main.
    for (const auto &entry : m_threads)
    {
        for (const GuestInvocation &invocation : entry.second.invocations)
        {
            if (!invocation.usesMainStack)
            {
                continue;
            }
            static const char *kKindNames[] = {
                "Interrupt", "Alarm", "GsCallback", "RpcCallback",
                "SyscallOverride", "ExitHandler", "HleCall", "SifCommand",
            };
            const size_t kindIndex = static_cast<size_t>(invocation.kind);
            const char *kindName = kindIndex < sizeof(kKindNames) / sizeof(kKindNames[0])
                                       ? kKindNames[kindIndex]
                                       : "?";
            RUNTIME_LOG("[ee-sched:main-frozen]   parked callback tid="
                        << entry.first << " kind=" << kindName
                        << " seq=" << std::dec << invocation.sequence
                        << " tag=0x" << std::hex << invocation.tag
                        << " pc=0x" << std::hex << invocation.context.pc
                        << " ra=0x" << getRegU32(&invocation.context, 31)
                        << " sp=0x" << getRegU32(&invocation.context, 29)
                        << " a0=0x" << getRegU32(&invocation.context, 4)
                        << std::dec << std::endl);
        }
    }
}

void EeScheduler::makeRunning(GuestThread &item)
{
    assert(m_currentThreadId == 0);
    assert(item.status == EeThreadStatus::Ready);
    item.status = EeThreadStatus::Running;
    m_currentThreadId = item.id;
    renewTimeSlice();
}

// M5-T11 fix: mirrors the guarded decrement of the completion pop - every
// live usesMainStack invocation this record still holds releases one freeze
// count. Called from every path that destroys the invocation records without
// completing them: makeDormant (terminateThread / exitCurrent / invalid-jump
// / pc==0 exit all land here) and deleteThread (direct record erase).
void EeScheduler::releaseMainStackUsers(const GuestThread &item)
{
    for (const GuestInvocation &invocation : item.invocations)
    {
        if (invocation.usesMainStack && m_mainStackUsers > 0)
        {
            --m_mainStackUsers;
        }
    }
}

void EeScheduler::makeDormant(GuestThread &item)
{
    // M5-T11 fix: the invocations being cleared may still be live on the
    // MAIN thread's stack - release their freeze count first or a thread
    // destroyed mid-callback wedges main forever.
    releaseMainStackUsers(item);
    removeReady(item);
    removeFromWaitObject(item);
    item.status = EeThreadStatus::Dormant;
    item.wait = {};
    item.resumeCompletion = {};
    item.suspendCount = 0;
    item.wakeupCount = 0;
    item.invocations.clear();
}

void EeScheduler::removeFromWaitObject(GuestThread &item)
{
    const int id = item.id;
    if (item.wait.reason == EeWaitReason::Semaphore)
    {
        const int objectId = std::get<EeSemaphoreWait>(item.wait.payload).id;
        if (EeSemaphore *object = semaphore(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    else if (item.wait.reason == EeWaitReason::EventFlag)
    {
        const int objectId = std::get<EeEventFlagWait>(item.wait.payload).id;
        if (EeEventFlag *object = eventFlag(objectId))
        {
            auto it = std::find(object->waiters.begin(), object->waiters.end(), id);
            if (it != object->waiters.end())
            {
                object->waiters.erase(it);
            }
        }
    }
    item.wait = {};
}

void EeScheduler::blockCurrent(EeWaitState wait)
{
    GuestThread *self = currentThread();
    assert(self != nullptr);
    self->wait = std::move(wait);
    self->status = self->suspendCount == 0 ? EeThreadStatus::Waiting : EeThreadStatus::WaitingSuspended;
    m_currentThreadId = 0;
    publishSnapshot();
    throw EeDispatcherTransfer{};
}

void EeScheduler::makeReady(GuestThread &item, int result, bool interruptSafe)
{
    auto completion = std::move(item.wait.completion);
    item.wait = {};
    setReturnS32(&item.activeContext(), result);
    item.resumeCompletion = std::move(completion);
    if (item.suspendCount != 0)
    {
        item.status = EeThreadStatus::Suspended;
        return;
    }
    enqueueReady(item);
    requestPreemptionIfHigher(item, interruptSafe);
}

void EeScheduler::requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe)
{
    const GuestThread *running = currentThread();
    if (!running || readyThread.currentPriority >= running->currentPriority)
    {
        return;
    }
    m_rescheduleRequested = true;
    if (interruptSafe || m_insideInterrupt)
    {
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::applyPendingPreemption()
{
    if (!m_rescheduleRequested)
    {
        return;
    }
    if (m_currentThreadId == 0)
    {
        m_rescheduleRequested = false;
        m_timeSliceExpired = false;
        return;
    }
    GuestThread *self = currentThread();
    assert(self != nullptr);
    enqueueReady(*self, !m_timeSliceExpired);
    m_currentThreadId = 0;
    m_rescheduleRequested = false;
    m_timeSliceExpired = false;
}

void EeScheduler::processPendingEvents()
{
    assertExecutor();
    processDueDeadlines();
    const uint32_t timerInterrupts = m_pendingEeTimerInterrupts;
    m_pendingEeTimerInterrupts = 0u;
    for (uint32_t timer = 0u; timer < 4u; ++timer)
    {
        if ((timerInterrupts & (1u << timer)) != 0u)
        {
            dispatchIrq(false, 9u + timer);
        }
    }
    std::deque<EeEvent> pending;
    {
        std::lock_guard lock(m_eventMutex);
        pending.swap(m_events);
    }
    for (const EeEvent &event : pending)
    {
        processEvent(event);
    }

    {
        std::lock_guard lock(m_eventMutex);
        const uint64_t nextEventCycle = m_nextDeadlineCycle.load(std::memory_order_acquire);
        const bool cycleEventDue = nextEventCycle != 0u && m_eeCycle >= nextEventCycle;
        const bool pendingWork = !m_events.empty() || cycleEventDue || m_stopRequested.load(std::memory_order_acquire);
        m_checkpointPending.store(pendingWork, std::memory_order_release);
    }
    applyPendingPreemption();
}

void EeScheduler::processDueDeadlines()
{
    for (;;)
    {
        std::vector<ScheduledEvent> due;
        std::chrono::steady_clock::time_point pacingDeadline{};
        {
            std::unique_lock lock(m_eventMutex);
            const auto now = std::chrono::steady_clock::now();
            for (const ScheduledEvent &item : m_deadlines)
            {
                if (item.deadlineCycle <= m_eeCycle &&
                    (pacingDeadline == std::chrono::steady_clock::time_point{} ||
                     item.hostDeadline < pacingDeadline))
                {
                    pacingDeadline = item.hostDeadline;
                }
            }

            if (pacingDeadline == std::chrono::steady_clock::time_point{})
            {
                updateNextDeadline();
                return;
            }

            if (now < pacingDeadline)
            {
                m_eventCv.wait_until(lock, pacingDeadline, [this]()
                                     { return !m_events.empty() ||
                                              m_stopRequested.load(std::memory_order_acquire); });
                if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
                {
                    updateNextDeadline();
                    return;
                }
            }

            const auto pacedNow = std::chrono::steady_clock::now();
            auto firstFuture = std::partition(m_deadlines.begin(), m_deadlines.end(),
                                              [this, pacedNow](const ScheduledEvent &item)
                                              { return item.deadlineCycle <= m_eeCycle &&
                                                       item.hostDeadline <= pacedNow; });
            due.insert(due.end(),
                       std::make_move_iterator(m_deadlines.begin()),
                       std::make_move_iterator(firstFuture));
            m_deadlines.erase(m_deadlines.begin(), firstFuture);
            updateNextDeadline();
        }

        std::sort(due.begin(), due.end(), [](const ScheduledEvent &left, const ScheduledEvent &right)
                  {
                      if (left.deadlineCycle != right.deadlineCycle)
                      {
                          return left.deadlineCycle < right.deadlineCycle;
                      }
                      if (left.event.type != right.event.type)
                      {
                          return left.event.type < right.event.type;
                      }
                      if (left.event.id != right.event.id)
                      {
                          return left.event.id < right.event.id;
                      }
                      return left.sequence < right.sequence; });

        if (due.empty())
        {
            return;
        }

        for (ScheduledEvent &scheduled : due)
        {
            if (scheduled.event.type == EeEventType::VBlankStart)
            {
                scheduleEvent(scheduled.deadlineCycle + kVBlankDurationCycles,
                              scheduled.hostDeadline + kVBlankDuration,
                              EeEvent{EeEventType::VBlankEnd, 0, m_vsyncTick + 1u});
                scheduleEvent(scheduled.deadlineCycle + kVBlankPeriodCycles,
                              scheduled.hostDeadline + kVBlankPeriod,
                              EeEvent{EeEventType::VBlankStart, 0, 0});
            }
            processEvent(scheduled.event);
        }
    }
}

void EeScheduler::processEvent(const EeEvent &event)
{
    switch (event.type)
    {
    case EeEventType::Stop:
        requestStop();
        break;
    case EeEventType::VBlankStart:
        ++m_vsyncTick;
        m_runtime.memory().gs().vsyncTick.store(m_vsyncTick, std::memory_order_release);
        if ((m_vsyncTick & 1u) != 0u)
        {
            m_runtime.memory().gs().csr.fetch_or(0x2000ull, std::memory_order_acq_rel);
        }
        else
        {
            m_runtime.memory().gs().csr.fetch_and(~0x2000ull, std::memory_order_acq_rel);
        }
        // M5-T16: promote the async invocations held for this boundary
        // (the synthesized SIF-cmd completions). Delivery now rides the
        // vsync tick - hardware's IOP turnaround guarantees the loader's
        // filename-builder fill completes before the completion wakes
        // main; the inline delivery was the movie-end ordering divergence.
        {
            std::deque<GuestInvocation> due;
            {
                std::lock_guard lock(m_eventMutex);
                while (!m_deferredInvocations.empty() &&
                       m_deferredInvocations.front().readyVsync <= m_vsyncTick)
                {
                    due.push_back(std::move(m_deferredInvocations.front().invocation));
                    m_deferredInvocations.pop_front();
                }
            }
            while (!due.empty())
            {
                GuestInvocation invocation = std::move(due.front());
                due.pop_front();
                // M5-T16: the reply is main-owned traffic (the loader
                // completion channel). Delivered while main is PARKED, the
                // idle path would hand it to a stackless dispatcher thread
                // and the T11/T12 fallback would bind it AT main's
                // suspended sp - the exact bind geometry the T15 carve
                // replaced (the handler's frames descend into the below-sp
                // band the filename builder fills; the freeze stops main,
                // never the builder). Pre-bind the explicit sp on the
                // carve; bindInvocationStack honors it byte-for-byte.
                if (getRegU32(&invocation.context, 29) == 0u)
                {
                    SET_GPR_U32(&invocation.context, 29, carveInvocationSp());
                }
                {
                    // M5-T16 diagnostic: the reply promotion beat - which
                    // vsync delivered which reply packet.
                    static uint32_t s_promoteLogs = 0;
                    if (s_promoteLogs < 400u)
                    {
                        ++s_promoteLogs;
                        std::ostringstream line;
                        line << "[m5t16-promote] vsync=" << std::dec << m_vsyncTick
                             << " pkt=0x" << std::hex << getRegU32(&invocation.context, 4)
                             << std::dec << std::endl;
                        RUNTIME_LOG(line.str());
                    }
                }
                queueInvocation(std::move(invocation));
            }
        }
        t10TagWrite("vsync-flag", m_vsyncFlagAddress, 1u, 0u);
        writeGuestU32(m_vsyncFlagAddress, 1u);
        if (m_vsyncTickAddress != 0u)
        {
            const uint32_t physical = m_vsyncTickAddress & 0x1FFFFFFFu;
            if (m_rdram && physical <= PS2_RAM_SIZE - sizeof(uint64_t))
            {
                t10TagWrite("vsync-tick", physical, static_cast<uint32_t>(m_vsyncTick), static_cast<uint32_t>(m_vsyncTick >> 32u));
                std::memcpy(m_rdram + physical, &m_vsyncTick, sizeof(m_vsyncTick));
            }
        }
        m_vsyncFlagAddress = 0u;
        m_vsyncTickAddress = 0u;
        completeVSync(m_vsyncTick);
        if (m_gsVSyncCallback != 0u && m_runtime.hasFunction(m_gsVSyncCallback))
        {
            GuestInvocation invocation{};
            invocation.kind = GuestInvocationKind::GsCallback;
            invocation.context.pc = m_gsVSyncCallback;
            SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(m_vsyncTick));
            SET_GPR_U32(&invocation.context, 28, m_gsVSyncCallbackGp);
            SET_GPR_U32(&invocation.context, 29, m_gsVSyncCallbackSp);
            SET_GPR_U32(&invocation.context, 31, 0u);
            queueInvocation(std::move(invocation));
        }
        dispatchIrq(false, 2u);
        break;
    case EeEventType::ExternalWake:
        completeExternalWait(event.id, event.value, KE_OK);
        break;
    case EeEventType::VBlankEnd:
        dispatchIrq(false, 3u);
        break;
    case EeEventType::Dmac:
        break;
    case EeEventType::Alarm:
    {
        auto it = m_alarms.find(static_cast<int>(event.id));
        if (it == m_alarms.end())
        {
            break;
        }
        const EeAlarm alarm = it->second;
        m_alarms.erase(it);
        {
            static uint32_t s_t8AlarmFires = 0u;
            ++s_t8AlarmFires;
            if (s_t8AlarmFires <= 8u || (s_t8AlarmFires % 512u) == 0u)
            {
                RUNTIME_LOG("[t8-alarm] fire #" << s_t8AlarmFires
                                                << " handler=0x" << std::hex << alarm.handler
                                                << " arg=0x" << alarm.argument
                                                << std::dec << " ticks=" << alarm.ticks
                                                << std::endl);
            }
        }
        GuestInvocation invocation{};
        invocation.kind = GuestInvocationKind::Alarm;
        invocation.context.pc = alarm.handler;
        SET_GPR_U32(&invocation.context, 4, static_cast<uint32_t>(alarm.id));
        SET_GPR_U32(&invocation.context, 5, static_cast<uint32_t>(alarm.ticks));
        SET_GPR_U32(&invocation.context, 6, alarm.argument);
        SET_GPR_U32(&invocation.context, 28, alarm.gp);
        SET_GPR_U32(&invocation.context, 29, 0u);
        SET_GPR_U32(&invocation.context, 31, 0u);
        // M5-T18: the alarm callback fires at an arbitrary checkpoint (a
        // 64 us alarm lands between vsyncs); deliver it on the next VBlank
        // boundary with the T16 carve pre-bind, not inline at the fire
        // checkpoint (the same T17 interleave class as the timer IRQs
        // above).
        queueDeferredInvocation(std::move(invocation), 1u);
        break;
    }
    }
}

void EeScheduler::finishEventWaiters(EeEventFlag &flag, bool interruptSafe)
{
    for (auto it = flag.waiters.begin(); it != flag.waiters.end();)
    {
        GuestThread *waiter = thread(*it);
        assert(waiter != nullptr);
        const EeEventFlagWait wait = std::get<EeEventFlagWait>(waiter->wait.payload);
        if (!eventCondition(flag.bits, wait.bits, wait.mode))
        {
            ++it;
            continue;
        }
        const uint32_t observed = flag.bits;
        t10TagWrite("evf-wake", wait.resultAddress, observed, 0u);
        writeGuestU32(wait.resultAddress, observed);
        if ((wait.mode & WEF_CLEAR_ALL) != 0u)
        {
            flag.bits = 0;
        }
        else if ((wait.mode & WEF_CLEAR) != 0u)
        {
            flag.bits &= ~wait.bits;
        }
        it = flag.waiters.erase(it);
        makeReady(*waiter, KE_OK, interruptSafe);
    }
}

bool EeScheduler::eventCondition(uint32_t current, uint32_t requested, uint32_t mode)
{
    return (mode & WEF_OR) != 0u ? (current & requested) != 0u
                                 : (current & requested) == requested;
}

int EeScheduler::waitObjectId(const EeWaitState &wait)
{
    switch (wait.reason)
    {
    case EeWaitReason::Semaphore:
        return std::get<EeSemaphoreWait>(wait.payload).id;
    case EeWaitReason::EventFlag:
        return std::get<EeEventFlagWait>(wait.payload).id;
    default:
        return 0;
    }
}

void EeScheduler::writeGuestU32(uint32_t address, uint32_t value)
{
    if (address == 0u)
    {
        return;
    }
    const uint32_t physical = address & 0x1FFFFFFFu;
    if (!m_rdram || physical > PS2_RAM_SIZE - sizeof(value))
    {
        return;
    }
    std::memcpy(m_rdram + physical, &value, sizeof(value));
}

void EeScheduler::waitForEvent()
{
    std::unique_lock lock(m_eventMutex);
    if (!m_events.empty() || m_stopRequested.load(std::memory_order_acquire))
    {
        return;
    }
    const uint64_t timerCycles = m_runtime.memory().cyclesUntilNextEeTimerInterrupt();
    const bool hasTimerDeadline = timerCycles != std::numeric_limits<uint64_t>::max();
    if (m_deadlines.empty() && !hasTimerDeadline)
    {
        m_eventCv.wait(lock, [this]()
                       { return !m_events.empty() || m_stopRequested.load(std::memory_order_acquire); });
        return;
    }

    uint64_t deadlineCycle = 0u;
    auto hostDeadline = std::chrono::steady_clock::time_point::max();
    if (!m_deadlines.empty())
    {
        const auto next = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                           [](const ScheduledEvent &left, const ScheduledEvent &right)
                                           {
                                               if (left.deadlineCycle != right.deadlineCycle)
                                               {
                                                   return left.deadlineCycle < right.deadlineCycle;
                                               }
                                               return left.sequence < right.sequence;
                                           });
        deadlineCycle = next->deadlineCycle;
        hostDeadline = next->hostDeadline;
    }
    if (hasTimerDeadline)
    {
        const auto timerHostDeadline = std::chrono::steady_clock::now() + eeCyclesToHostDuration(timerCycles);
        if (timerHostDeadline < hostDeadline)
        {
            deadlineCycle = m_eeCycle + timerCycles;
            hostDeadline = timerHostDeadline;
        }
    }

    const bool signaled = m_eventCv.wait_until(lock, hostDeadline, [this]()
                                               { return !m_events.empty() ||
                                                        m_stopRequested.load(std::memory_order_acquire); });
    if (!signaled)
    {
        const uint64_t elapsed = deadlineCycle > m_eeCycle ? deadlineCycle - m_eeCycle : 0u;
        lock.unlock();
        uint64_t remaining = elapsed;
        while (remaining > 0u)
        {
            const uint32_t step = static_cast<uint32_t>(std::min<uint64_t>(remaining, std::numeric_limits<uint32_t>::max()));
            accountCycles(step);
            remaining -= step;
        }
        m_checkpointPending.store(true, std::memory_order_release);
    }
}

void EeScheduler::scheduleEvent(uint64_t deadlineCycle,
                                std::chrono::steady_clock::time_point hostDeadline,
                                EeEvent event)
{
    {
        std::lock_guard lock(m_eventMutex);
        m_deadlines.push_back(ScheduledEvent{deadlineCycle, hostDeadline, event, ++m_eventSequence});
        updateNextDeadline();
    }
    m_eventCv.notify_one();
}

void EeScheduler::updateNextDeadline()
{
    if (m_deadlines.empty())
    {
        m_nextDeadlineCycle.store(0u, std::memory_order_release);
        return;
    }
    const auto it = std::min_element(m_deadlines.begin(), m_deadlines.end(),
                                     [](const ScheduledEvent &left, const ScheduledEvent &right)
                                     {
                                         if (left.deadlineCycle != right.deadlineCycle)
                                         {
                                             return left.deadlineCycle < right.deadlineCycle;
                                         }
                                         return left.sequence < right.sequence;
                                     });
    m_nextDeadlineCycle.store(it->deadlineCycle, std::memory_order_release);
}

bool EeScheduler::hasReadyAtOrAbovePriority(int priority) const
{
    const int last = std::clamp(priority, 0, kPriorityCount - 1);
    for (int p = 0; p <= last; ++p)
    {
        if (!m_readyQueues[static_cast<size_t>(p)].empty())
        {
            return true;
        }
    }
    return false;
}

void EeScheduler::renewTimeSlice()
{
    m_sliceEndCycle = m_eeCycle + kDefaultTimeSliceCycles;
    m_timeSliceExpired = false;
}

void EeScheduler::copyMainContextToRuntime()
{
    const GuestThread *main = thread(kMainThreadId);
    if (main)
    {
        m_runtime.m_cpuContext = main->context;
    }
}

void EeScheduler::publishDebugContext(const R5900Context &context)
{
    m_runtime.m_debugPc.store(context.pc, std::memory_order_relaxed);
    m_runtime.m_debugRa.store(getRegU32(&context, 31), std::memory_order_relaxed);
    m_runtime.m_debugSp.store(getRegU32(&context, 29), std::memory_order_relaxed);
    m_runtime.m_debugGp.store(getRegU32(&context, 28), std::memory_order_relaxed);
}

void EeScheduler::publishIdleDebugContext()
{
    // Temporary IRQ/RPC/alarm invocations deliberately return to PC=0. Once
    // the scheduler is idle, show a real EE thread context instead of leaving
    // the debugger pinned to that completed dispatcher frame.
    const GuestThread *selected = thread(kMainThreadId);
    if (!selected)
    {
        for (const auto &[id, candidate] : m_threads)
        {
            if (id > 0 && candidate.status != EeThreadStatus::Dormant)
            {
                selected = &candidate;
                break;
            }
        }
    }

    if (selected)
    {
        publishDebugContext(selected->activeContext());
    }
}
