#pragma once

#include "ps2_runtime.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <variant>
#include <vector>

// This exception is the EE equivalent of a longjmp to the dispatcher.  It is
// not an error and must only be caught at EeScheduler::run().
struct EeDispatcherTransfer final
{
};

enum class EeThreadStatus : uint8_t
{
    Running,
    Ready,
    Waiting,
    WaitingSuspended,
    Suspended,
    Dormant,
};

enum class EeWaitReason : uint8_t
{
    None,
    Sleep,
    Semaphore,
    EventFlag,
    VSync,
    External,
    Mpeg,
};

struct EeSemaphoreWait
{
    int id = 0;
};

struct EeEventFlagWait
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t mode = 0;
    uint32_t resultAddress = 0;
};

struct EeVSyncWait
{
    uint64_t afterTick = 0;
    int fixedResult = -1;
};

struct EeExternalWait
{
    uint32_t type = 0;
    uint64_t token = 0;
};

using EeWaitPayload = std::variant<std::monostate,
                                   EeSemaphoreWait,
                                   EeEventFlagWait,
                                   EeVSyncWait,
                                   EeExternalWait>;

struct EeWaitState
{
    EeWaitReason reason = EeWaitReason::None;
    EeWaitPayload payload{};
    std::function<void(R5900Context &)> completion;
};

enum class GuestInvocationKind : uint8_t
{
    Interrupt,
    Alarm,
    GsCallback,
    RpcCallback,
    SyscallOverride,
    ExitHandler,
    HleCall,
    SifCommand,
};

struct GuestInvocation
{
    GuestInvocationKind kind = GuestInvocationKind::Interrupt;
    uint64_t sequence = 0;
    uint64_t tag = 0;
    R5900Context context{};
    std::function<void(const R5900Context &, R5900Context &)> onComplete;
    // M5-T11: this invocation was bound below the MAIN thread's suspended
    // $sp (stackless internal dispatcher thread). While it is live the main
    // thread must stay unscheduled - its pump would descend straight into
    // the callback's frames (the script-path pc=0x100 death).
    bool usesMainStack = false;
};

struct GuestThread
{
    int id = 0;
    R5900Context context{};
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t arg = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    bool ownsStack = false;
    uint32_t tlsBase = 0;
    EeWaitState wait{};
    std::function<void(R5900Context &)> resumeCompletion;
    std::vector<GuestInvocation> invocations;

    // Vanguard M5-T4 bring-up diagnostics: the last dispatch of this thread
    // that HAD a recompiled function (updated by the executor right before
    // the call). When the thread later parks on an unmapped pc, the dump can
    // name the last good frame instead of leaving a silent DORMANT.
    uint32_t lastGoodPc = 0;
    uint32_t lastGoodRa = 0;
    uint32_t lastGoodSp = 0;
    bool invalidJumpLogged = false;

    [[nodiscard]] R5900Context &activeContext()
    {
        return invocations.empty() ? context : invocations.back().context;
    }

    [[nodiscard]] const R5900Context &activeContext() const
    {
        return invocations.empty() ? context : invocations.back().context;
    }
};

struct EeSemaphore
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    int initCount = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    std::deque<int> waiters;
};

struct EeEventFlag
{
    int id = 0;
    uint32_t attr = 0;
    uint32_t option = 0;
    uint32_t initBits = 0;
    uint32_t bits = 0;
    std::deque<int> waiters;
};

struct EeAlarm
{
    int id = 0;
    uint16_t ticks = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
};

struct EeIrqHandler
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    uint32_t gp = 0;
    uint32_t sp = 0;
    bool enabled = true;
    int order = 0;
};

struct EeThreadSnapshot
{
    int id = 0;
    uint32_t pc = 0;
    uint32_t ra = 0;
    uint32_t sp = 0;
    uint32_t contextGp = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int initialPriority = 0;
    int currentPriority = 0;
    EeThreadStatus status = EeThreadStatus::Dormant;
    EeWaitReason waitReason = EeWaitReason::None;
    int waitId = 0;
    // Bring-up diagnostics (Vanguard M3): payload details for External/Mpeg/
    // VSync waits, so the steady-state blocker names itself in the tick dump.
    uint32_t waitType = 0;
    uint64_t waitToken = 0;
    uint64_t waitAfterTick = 0;
    int suspendCount = 0;
    uint32_t wakeupCount = 0;
    uint32_t invocationDepth = 0;
};

struct EeIrqHandlerSnapshot
{
    int id = 0;
    uint32_t cause = 0;
    uint32_t handler = 0;
    uint32_t argument = 0;
    bool enabled = true;
};

struct EeSemaphoreSnapshot
{
    int id = 0;
    int count = 0;
    int maxCount = 0;
    uint32_t waiters = 0;
};

struct EeEventFlagSnapshot
{
    int id = 0;
    uint32_t bits = 0;
    uint32_t initBits = 0;
    uint32_t attr = 0;
    uint32_t waiters = 0;
};

struct EeKernelSnapshot
{
    uint64_t sequence = 0;
    uint64_t eeCycle = 0;
    uint64_t sliceEndCycle = 0;
    uint64_t nextEventCycle = 0;
    int runningThreadId = 0;
    std::vector<EeThreadSnapshot> threads;
    std::vector<EeSemaphoreSnapshot> semaphores;
    std::vector<EeEventFlagSnapshot> eventFlags;
    std::vector<EeIrqHandlerSnapshot> intcHandlers;
    std::vector<EeIrqHandlerSnapshot> dmacHandlers;
};

enum class EeEventType : uint8_t
{
    Stop,
    VBlankStart,
    VBlankEnd,
    Dmac,
    ExternalWake,
    Alarm,
};

struct EeEvent
{
    EeEventType type = EeEventType::ExternalWake;
    uint32_t id = 0;
    uint64_t value = 0;
};

struct EeThreadCreateParams
{
    uint32_t attr = 0;
    uint32_t entry = 0;
    uint32_t stack = 0;
    uint32_t stackSize = 0;
    uint32_t gp = 0;
    int priority = 0;
    uint32_t option = 0;
};

class EeScheduler
{
public:
    static constexpr int kMainThreadId = 1;
    static constexpr int kFirstThreadId = 2;
    static constexpr int kLastThreadId = 255;
    static constexpr int kPriorityCount = 128;
    static constexpr uint64_t kEeClockHz = 294912000ull;
    static constexpr uint32_t kGeneratedCheckpointCycles = 32u;
    static constexpr uint32_t kGuestDispatchCycles = 8u;
    static constexpr uint64_t kDefaultTimeSliceCycles = 65536ull;

    explicit EeScheduler(PS2Runtime &runtime);
    ~EeScheduler();

    EeScheduler(const EeScheduler &) = delete;
    EeScheduler &operator=(const EeScheduler &) = delete;

    void reset(uint8_t *rdram, const R5900Context &mainContext);
    void run();
    void requestStop();
    void postEvent(EeEvent event);
    [[nodiscard]] bool checkpointDue(uint32_t cycles = kGeneratedCheckpointCycles) noexcept;
    void accountCycles(uint32_t cycles) noexcept;
    [[nodiscard]] bool isExecutingGuest() const noexcept;

    // Kernel object API. All calls except postEvent/requestStop execute on the
    // EE executor and therefore need no host synchronization.
    void setupCurrentThread(uint32_t stack, uint32_t stackSize, uint32_t gp);
    int createThread(const EeThreadCreateParams &params);
    int deleteThread(int id, uint32_t &ownedStack);
    int startThread(int id, uint32_t arg, const R5900Context &caller, bool interruptSafe);
    [[noreturn]] void exitCurrent(bool deleteThread);
    int terminateThread(int id, uint32_t &ownedStack, bool interruptSafe);
    int suspendThread(int id, bool interruptSafe);
    int resumeThread(int id, bool interruptSafe);
    void sleepCurrent();
    int wakeupThread(int id, bool interruptSafe);
    int cancelWakeup(int id);
    int changePriority(int id, int priority, bool interruptSafe, int &oldPriority);
    int rotateReadyQueue(int priority, bool interruptSafe);
    int releaseWait(int id, bool interruptSafe);
    void transferIfRequested(bool interruptSafe);

    int createSemaphore(int initCount, int maxCount, uint32_t attr, uint32_t option);
    int deleteSemaphore(int id, bool interruptSafe);
    int signalSemaphore(int id, bool interruptSafe);
    int pollSemaphore(int id);
    void waitSemaphore(int id);

    int createEventFlag(uint32_t initialBits, uint32_t attr, uint32_t option);
    int deleteEventFlag(int id, bool interruptSafe);
    int setEventFlag(int id, uint32_t bits, bool interruptSafe);
    int clearEventFlag(int id, uint32_t mask);
    int pollEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t &observedBits);
    void waitEventFlag(int id, uint32_t bits, uint32_t mode, uint32_t resultAddress);

    int setAlarm(uint16_t ticks, uint32_t handler, uint32_t argument, uint32_t gp, uint32_t sp);
    int cancelAlarm(int id);
    void queueInvocation(GuestInvocation invocation);
    // M5-T16: hold an async invocation until a later vsync boundary. The
    // synthesized SIF-cmd completions (the T2 loader handshake) posted
    // INLINE at request time let main's loader/pump wake inside the
    // filename-builder's fill window - the movie-end pc=ra=0x100 ordering
    // death. Hardware's IOP turnaround puts the reply at least a frame
    // after the request; this queue rides the vsync tick.
    void queueDeferredInvocation(GuestInvocation invocation, uint64_t vsyncsToWait = 1u);
    [[noreturn]] void invokeCurrent(GuestInvocation invocation);
    [[noreturn]] void invokeCurrentSequence(std::vector<GuestInvocation> invocations);
    [[nodiscard]] bool hasInvocation(GuestInvocationKind kind, uint64_t tag) const;
    // M5-T11 fix: hardware runs interrupt/SIF/RPC callbacks on the interrupted
    // thread's OWN stack (frames below its live $sp). The T7 host-reserved
    // blocks carved out of the top-of-RAM band march DOWN as new
    // (thread, depth) keys appear - and Vanguard's malloc arena reaches that
    // band, so callback frames poisoned live game data (the deterministic
    // tid=1 pc=0x100 invalid-jump death). Bind guest-backed invocations to
    // the owner's suspended $sp; only threads without a guest stack (the
    // internal dispatcher threads) still use a reserved block.
    // M5-T11: binds invocation.context.$sp (owner stack / main-stack fallback);
    // sets invocation.usesMainStack when the fallback is taken.
    void bindInvocationStack(GuestThread &owner, GuestInvocation &invocation);
    // Deep-callback bound: a guest-stack anchor is only used when it leaves
    // >= 4 KiB (0x1000) of headroom below the anchor $sp; a callback nested
    // deeper than that falls through to the main-stack fallback (and, if
    // that too lacks headroom, the degenerate reserved block). HW-faithful
    // overflow behavior: real hardware just pushes below $sp, clobbering
    // whatever lives there.
    [[nodiscard]] uint32_t invocationEntrySp(const GuestThread &owner, bool &usedMainStack);
    // M5-T11: number of LIVE invocations bound to the main thread's stack.
    int m_mainStackUsers = 0;
    // M5-T11 fix: release the main-stack freeze count for every live
    // usesMainStack invocation a destruction path is about to drop
    // (makeDormant clears invocations; deleteThread erases the record) -
    // a leaked count wedges main forever, silent, no timeout.
    void releaseMainStackUsers(const GuestThread &item);
    // M5-T12: true only when a thread OTHER than main holds a live
    // main-stack invocation. The freeze skips main only for foreign owners -
    // a main-owned main-stack callback must never hold main out of the
    // schedule (main's dispatch runs and completes it; the completed pc=0
    // pop is what releases the freeze count).
    [[nodiscard]] bool mainStackHeldByForeignOwner() const;
    // M5-T11 fix: rate-limited (2 s) diagnostic while the freeze is actively
    // holding main out of schedule, so a wedged freeze is diagnosable.
    void logMainStackFreeze();
    std::chrono::steady_clock::time_point m_lastMainFreezeLog{};
    // M5-T11 fix: one-shot gate for the degenerate reserved-block fallback
    // log (see invocationEntrySp) - the T10 heap-trample risk must be loud.
    bool m_degenerateInvocationStackLogged = false;
    // M5-T15: the dedicated defended carve for MAIN-OWNED async invocations
    // (the T13/T14 architectural closure). The T12 rule bound main's own
    // callbacks at main's live boundary sp - INSIDE the below-sp band the
    // game's loader filename builder (on tid3) fills as scratch: the
    // captures measured the builder's memcpy landing directly in the
    // handler's saved-ra slot (pc=ra=0x100, the movie-end poison). Main
    // now binds its invocations on the fixed carve
    // [kMainInvocationCarveBase, kMainInvocationCarveTop) instead - inside
    // main's DECLARED stack, below its observed live floor, above the
    // game's arena top. Main is parked while a carve frame runs, so the
    // T11 freeze does not apply to this path; foreign owners keep the
    // owner-stack/freeze semantics unchanged.
    // The next carve bind goes at the lowest live carve sp (nested frames
    // push below the innermost live frame), or at the carve top - 0x10
    // when the carve is idle. Returns the bind sp.
    [[nodiscard]] uint32_t carveInvocationSp();
    // M5-T15 guard: main's own suspended sp sampled at every scheduling
    // decision - a sp anywhere inside main's declared stack BELOW the
    // carve top means main descended into the carve band (the design's
    // known failure mode): log loudly, rate-limited, naming the live
    // carve frames - diagnosable in minutes (M3-T4 rule).
    void guardMainCarve(uint32_t mainSp);
    std::chrono::steady_clock::time_point m_lastCarveGuardLog{};
    // M5-T15: one-shot gate for the carve-exhausted log (would need >90 KiB
    // of simultaneous carve frames - never measured at 4-6 KiB/callback).
    bool m_carveExhaustedLogged = false;
    [[nodiscard]] uint32_t invocationStackTop();

    int addIrqHandler(bool dmac,
                      uint32_t cause,
                      uint32_t handler,
                      bool append,
                      uint32_t argument,
                      uint32_t gp,
                      uint32_t sp);
    int removeIrqHandler(bool dmac, uint32_t cause, int id);
    int setIrqHandlerEnabled(bool dmac, int id, bool enabled);
    int setIrqCauseEnabled(bool dmac, uint32_t cause, bool enabled);
    void dispatchIrq(bool dmac, uint32_t cause);
    void setVSyncFlag(uint32_t flagAddress, uint32_t tickAddress);
    [[nodiscard]] uint64_t currentVSyncTick() const noexcept;
    // M6-T1: emulated-cycle position for clock-drift diagnostics (the
    // scheduled-event gate fires only when BOTH the wall deadline passed and
    // m_eeCycle reached the deadline cycle - a slow executor throttles vsync
    // delivery; this getter makes that visible to the tick dump).
    [[nodiscard]] uint64_t currentEeCycle() const noexcept;
    uint32_t setGsVSyncCallback(uint32_t callback, uint32_t gp, uint32_t sp);

    [[noreturn]] void waitVSync(uint64_t afterTick, int fixedResult = -1, std::function<void(R5900Context &)> completion = {});
    void completeVSync(uint64_t tick);
    void completeExternalWait(uint32_t type, uint64_t token, int result);
    [[noreturn]] void waitExternal(EeWaitReason reason, uint32_t type, uint64_t token, std::function<void(R5900Context &)> completion = {});

    [[nodiscard]] GuestThread *thread(int id);
    [[nodiscard]] const GuestThread *thread(int id) const;
    [[nodiscard]] EeSemaphore *semaphore(int id);
    [[nodiscard]] const EeSemaphore *semaphore(int id) const;
    [[nodiscard]] EeEventFlag *eventFlag(int id);
    [[nodiscard]] const EeEventFlag *eventFlag(int id) const;
    [[nodiscard]] GuestThread *currentThread();
    [[nodiscard]] const GuestThread *currentThread() const;
    [[nodiscard]] int currentThreadId() const noexcept;
    [[nodiscard]] R5900Context *currentContext();
    [[nodiscard]] uint8_t *rdram() const noexcept;

    // Direct syscall tests use the same main-thread record without starting a
    // second executor. Production execution calls reset() before run().
    void bindMainContextForSyscall(R5900Context &ctx, uint8_t *rdram);

    [[nodiscard]] EeKernelSnapshot snapshot() const;
    void publishSnapshot();

private:
    struct ScheduledEvent
    {
        uint64_t deadlineCycle = 0;
        std::chrono::steady_clock::time_point hostDeadline{};
        EeEvent event{};
        uint64_t sequence = 0;
    };

    void assertExecutor() const;
    [[nodiscard]] int allocateThreadId();
    GuestThread &acquireInvocationThread();
    void enqueueReady(GuestThread &thread, bool front = false);
    void removeReady(GuestThread &thread);
    [[nodiscard]] GuestThread *selectReady();
    void makeRunning(GuestThread &thread);
    void makeDormant(GuestThread &thread);
    void removeFromWaitObject(GuestThread &thread);
    [[noreturn]] void blockCurrent(EeWaitState wait);
    void makeReady(GuestThread &thread, int result, bool interruptSafe);
    void requestPreemptionIfHigher(const GuestThread &readyThread, bool interruptSafe);
    void applyPendingPreemption();
    void processPendingEvents();
    void processDueDeadlines();
    void processEvent(const EeEvent &event);
    void finishEventWaiters(EeEventFlag &flag, bool interruptSafe);
    [[nodiscard]] static bool eventCondition(uint32_t current, uint32_t requested, uint32_t mode);
    static int waitObjectId(const EeWaitState &wait);
    void writeGuestU32(uint32_t address, uint32_t value);
    void waitForEvent();
    void scheduleEvent(uint64_t deadlineCycle, std::chrono::steady_clock::time_point hostDeadline, EeEvent event);
    void updateNextDeadline();
    // M6-T2: publishes the earliest HOST deadline over m_deadlines to the
    // clock thread (m_nextHostDeadlineNs + a generation bump). Called with
    // m_eventMutex held (from updateNextDeadline).
    void publishHostScheduleLocked();
    // M6-T2: the clock beat thread. Discovers passed host deadlines while the
    // executor is busy executing guest code (no cycle deadline wakes it) and
    // asks for a scheduling pass via m_wallBeatPending; delivery itself always
    // happens on the executor thread.
    void clockThreadMain();
    [[nodiscard]] bool hasReadyAtOrAbovePriority(int priority) const;
    void renewTimeSlice();
    void copyMainContextToRuntime();
    void publishDebugContext(const R5900Context &context);
    void publishIdleDebugContext();

    PS2Runtime &m_runtime;
    uint8_t *m_rdram = nullptr;
    std::array<std::deque<int>, kPriorityCount> m_readyQueues{};
    std::unordered_map<int, GuestThread> m_threads;
    std::unordered_map<int, EeSemaphore> m_semaphores;
    std::unordered_map<int, EeEventFlag> m_eventFlags;
    std::unordered_map<int, EeAlarm> m_alarms;
    std::unordered_map<int, EeIrqHandler> m_intcHandlers;
    std::unordered_map<int, EeIrqHandler> m_dmacHandlers;
    int m_nextThreadId = kFirstThreadId;
    int m_nextInvocationThreadId = -1;
    int m_nextSemaphoreId = 1;
    int m_nextEventFlagId = 1;
    int m_nextAlarmId = 1;
    int m_nextIntcHandlerId = 1;
    int m_nextDmacHandlerId = 1;
    int m_intcHeadOrder = 0;
    int m_intcTailOrder = 1000;
    int m_dmacHeadOrder = 0;
    int m_dmacTailOrder = 1000;
    uint32_t m_enabledIntcMask = 0xFFFFFFFFu;
    uint32_t m_enabledDmacMask = 0xFFFFFFFFu;
    int m_currentThreadId = 0;
    bool m_rescheduleRequested = false;
    bool m_timeSliceExpired = false;
    bool m_insideInterrupt = false;
    uint32_t m_pendingEeTimerInterrupts = 0;
    uint64_t m_eeCycle = 0;
    uint64_t m_sliceEndCycle = kDefaultTimeSliceCycles;
    std::thread::id m_executorThread{};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_guestExecuting{false};
    std::atomic<bool> m_stopRequested{false};
    std::atomic<bool> m_checkpointPending{false};
    uint32_t m_debugPublishCountdown = 0u;

    mutable std::mutex m_eventMutex;
    std::condition_variable m_eventCv;
    std::deque<EeEvent> m_events;
    std::vector<ScheduledEvent> m_deadlines;
    std::deque<GuestInvocation> m_pendingInvocations;

    // M5-T16: async invocations held until a vsync boundary (guarded by
    // m_eventMutex, drained on the executor from processEvent).
    struct DeferredInvocation
    {
        GuestInvocation invocation;
        uint64_t readyVsync = 0;
    };
    std::deque<DeferredInvocation> m_deferredInvocations;
    uint64_t m_eventSequence = 0;
    uint64_t m_invocationSequence = 0;
    uint64_t m_vsyncTick = 0;
    uint32_t m_vsyncFlagAddress = 0;
    uint32_t m_vsyncTickAddress = 0;
    uint32_t m_gsVSyncCallback = 0;
    uint32_t m_gsVSyncCallbackGp = 0;
    uint32_t m_gsVSyncCallbackSp = 0;
    std::unordered_map<uint64_t, uint32_t> m_invocationStackTops;
    std::atomic<uint64_t> m_nextDeadlineCycle{0};

    // M6-T2: wall-master clock pacing. The vsync/timer beat is a wall-clock
    // source (the NTSC crystal does not care how fast the guest executes):
    // delivery is gated on the HOST deadline alone. The executor's cycle
    // position may lag arbitrarily when the guest runs slower than realtime -
    // the beat then fires late in guest-time terms, never fewer-per-wall-
    // second. The clock thread below is the discovery mechanism for passed
    // host deadlines while the executor is busy; the pacing decision (a
    // guest AHEAD of its guest-time schedule sleeps until the beat) stays on
    // the executor path inside processDueDeadlines.
    std::mutex m_clockMutex;
    std::condition_variable m_clockCv;
    std::thread m_clockThread;
    std::atomic<bool> m_clockStop{false};
    // Earliest host deadline over m_deadlines, ns since the steady epoch
    // (0 = no scheduled events), plus a generation counter so the clock
    // thread detects re-arms.
    std::atomic<uint64_t> m_nextHostDeadlineNs{0};
    std::atomic<uint64_t> m_hostScheduleGeneration{0};
    // Set by the clock thread when a host deadline passes; consumed (and
    // cleared) by processDueDeadlines on the executor thread.
    std::atomic<bool> m_wallBeatPending{false};

    mutable std::mutex m_snapshotMutex;
    EeKernelSnapshot m_snapshot;
    uint64_t m_snapshotSequence = 0;
};
