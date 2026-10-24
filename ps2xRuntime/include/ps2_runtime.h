#ifndef PS2_RUNTIME_H
#define PS2_RUNTIME_H

#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <string>
#include <string_view>
#include <functional>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX instructions
#include <smmintrin.h> // For SSE4.1 instructions
#endif
#include <atomic>
#include <array>
#include <mutex>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>

#include "ps2_log.h"
#include "runtime/ps2_address.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_audio.h"
#include "runtime/ps2_pad.h"
#include "runtime/ps2_rom_device.h"
#include "runtime/ps2_vfs.h"
#include "ps2x/iop/iop_types.h"

namespace ps2x::iop
{
    class IopSubsystem;
}

class PS2IopHostAdapter;
class PS2IopTransport;
class EeScheduler;
struct EeEvent;

enum PS2Exception
{
    EXCEPTION_TLB_REFILL = 0x02,          // TLB refill/load exception
    EXCEPTION_ADDRESS_ERROR_LOAD = 0x04,  // Address error on load
    EXCEPTION_ADDRESS_ERROR_STORE = 0x05, // Address error on store
    EXCEPTION_SYSCALL = 0x08,             // SYSCALL instruction
    EXCEPTION_BREAKPOINT = 0x09,          // BREAK instruction
    EXCEPTION_RESERVED_INSTRUCTION = 0x0A,
    EXCEPTION_INTEGER_OVERFLOW = 0x0C, // From MIPS spec
    EXCEPTION_TRAP = 0x0D,             // Trap instruction condition met
};

// PS2 CPU context (R5900)
struct alignas(16) R5900Context
{
    // General Purpose Registers (128-bit)
    __m128i r[32]; // Main registers

    // Control registers
    uint32_t pc;         // Program counter
    uint64_t insn_count; // Instruction counter
    uint64_t hi, lo;     // HI/LO registers for mult/div results
    uint64_t hi1, lo1;   // Secondary HI/LO registers for MULT1/DIV1
    uint32_t sa;         // Shift amount register

    // VU0 registers (when used in macro mode)
    __m128 vu0_vf[32];        // VU0 vector float registers
    uint16_t vi[16];          // VU0 vector integer registers
    float vu0_q;              // VU0 Q register (quotient)
    float vu0_p;              // VU0 P register (EFU result)
    float vu0_i;              // VU0 I register (integer value)
    __m128 vu0_r;             // VU0 R register
    __m128 vu0_acc;           // VU0 ACC accumulator register
    uint16_t vu0_status;      // VU0 status register
    uint32_t vu0_mac_flags;   // VU0 MAC flags
    uint32_t vu0_clip_flags;  // VU0 clipping flags
    uint32_t vu0_clip_flags2; // VU0 clipping flags
    uint32_t vu0_cmsar0;      // VU0 microprogram start address
    uint32_t vu0_cmsar1;      // VU0 microprogram start address
    uint32_t vu0_cmsar2;      // VU0 microprogram start address
    uint32_t vu0_cmsar3;      // VU0 microprogram start address
    uint32_t vu0_vpu_stat;
    uint32_t vu0_vpu_stat2; // extra VPU status (used by CR_VPU_STAT2)
    uint32_t vu0_vpu_stat3; // extra VPU status 3
    uint32_t vu0_vpu_stat4; // extra VPU status 4
    uint32_t vu0_tpc;       // TPC (VU0 PC)
    uint32_t vu0_tpc2;      // second TPC
    uint32_t vu0_fbrst;     // VIF/VU reset register
    uint32_t vu0_fbrst2;    // FBRST2
    uint32_t vu0_fbrst3;    // FBRST3
    uint32_t vu0_fbrst4;    // FBRST4
    uint32_t vu0_itop;
    uint32_t vu0_top;
    uint32_t vu0_info;
    uint32_t vu0_xitop; // VU0 XITOP - input ITOP for VIF/VU sync
    uint32_t vu0_pc;

    float vu0_cf[4]; // VU0 FMAC control floating-point registers

    // COP0 System control registers
    uint32_t cop0_index;
    uint32_t cop0_random;
    uint32_t cop0_entrylo0;
    uint32_t cop0_entrylo1;
    uint32_t cop0_context;
    uint32_t cop0_pagemask;
    uint32_t cop0_wired;
    uint32_t cop0_badvaddr;
    uint32_t cop0_count;
    uint32_t cop0_entryhi;
    uint32_t cop0_compare;
    uint32_t cop0_status;
    uint32_t cop0_cause;
    uint32_t cop0_epc;
    uint32_t cop0_prid;
    uint32_t cop0_config;
    uint32_t cop0_badpaddr;
    uint32_t cop0_debug;
    uint32_t cop0_perf;
    uint32_t cop0_taglo;
    uint32_t cop0_taghi;
    uint32_t cop0_errorepc;

    // LL/SC reservation state (not part of COP0 Status bits).
    uint32_t llbit;
    uint32_t lladdr;

    // Delay slot state tracking
    bool in_delay_slot;
    uint32_t branch_pc;

    // COP2 control registers (VU0 integer + control)
    uint32_t cop2_ccr[32];

    // FPU registers (COP1)
    float f[32];
    float f_acc;    // FPU accumulator
    uint32_t fcr31; // Control/status register

    R5900Context()
    {
        std::memset(this, 0, sizeof(*this));

        // Initialize VU0 registers
        vu0_q = 1.0f; // Q register usually initialized to 1.0

        // Reset COP0 registers
        cop0_random = 47; // Start at maximum value
        // Status as the EE kernel leaves it at handoff. IE (bit 0) and EIE
        // (bit 16) are separate enables and guest code reads both; libkernel's
        // StartThread refuses to run while IE is clear.
        cop0_status = 0x00010001; // EIE | IE
        cop0_prid = 0x00002e20; // CPU ID for R5900

        in_delay_slot = false;
        branch_pc = 0;
    }

    void dump() const
    {
        std::ios_base::fmtflags flags = std::cout.flags();
        std::cout << std::hex << std::setfill('0');
        std::cout << "--- R5900 Context Dump ---\n";
        std::cout << "PC: 0x" << std::setw(8) << pc << "\n";
        std::cout << "HI: 0x" << std::setw(8) << hi << " LO: 0x" << std::setw(8) << lo << "\n";
        std::cout << "HI1:0x" << std::setw(8) << hi1 << " LO1:0x" << std::setw(8) << lo1 << "\n";
        std::cout << "SA: 0x" << std::setw(8) << sa << "\n";
        for (int i = 0; i < 32; ++i)
        {
            std::cout << "R" << std::setw(2) << std::dec << i << ": 0x" << std::hex
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 3))
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 2)) << "_"
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 1))
                      << std::setw(8) << static_cast<uint32_t>(_mm_extract_epi32(r[i], 0)) << "\n";
        }
        std::cout << "Status: 0x" << std::setw(8) << cop0_status
                  << " Cause: 0x" << std::setw(8) << cop0_cause
                  << " EPC: 0x" << std::setw(8) << cop0_epc << "\n";
        std::cout << "--- End Context Dump ---\n";
        std::cout.flags(flags); // Restore format flags
    }

    ~R5900Context() = default;
};

inline uint32_t getRegU32(const R5900Context *ctx, int reg)
{
    // Check if reg is valid (0-31)
    if (reg < 0 || reg > 31)
        return 0;
    if (reg == 0)
        return 0;
    return static_cast<uint32_t>(_mm_extract_epi32(ctx->r[reg], 0));
}

inline void setReturnU32(R5900Context *ctx, uint32_t value)
{
    // R5900 sign-extends 32-bit results into 64-bit GPR, even for unsigned values.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value))); // $v0
}

inline void setReturnS32(R5900Context *ctx, int32_t value)
{
    // Signed 32-bit return should be sign-extended when observed as 64-bit.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value)); // $v0
}

inline void setReturnU64(R5900Context *ctx, uint64_t value)
{
    // Keep both conventions: full 64-bit value in $v0 and high 32-bit in $v1.
    ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(value));
    ctx->r[3] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<uint32_t>(value >> 32)));
}

inline constexpr uint32_t PS2_PATH_WATCH_ADDR = 0x01EFFFA0u;
inline constexpr uint32_t PS2_PATH_WATCH_BYTES = 0x200u;

// The EE kernel anchors the main thread's stack at the top of RAM with the
// kernel-default 32 KiB stack (main sp = PS2_RAM_SIZE - 0x10). Host-managed
// allocations that must never collide with live guest frames — the async
// callback stacks used by interrupt/alarm/SIF invocations — have to start
// below this line.
inline constexpr uint32_t kAsyncCallbackStackMainReserve = 0x8000u;

inline uint32_t ps2PathWatchPhysAddr()
{
    return PS2_PATH_WATCH_ADDR & PS2_RAM_MASK;
}

inline uint8_t ps2PathWatchExtractByteFromWrite(uint32_t writeAddr, uint32_t watchAddr, uint64_t valueLo, uint64_t valueHi)
{
    const uint32_t byteIndex = watchAddr - writeAddr;
    if (byteIndex < 8u)
    {
        return static_cast<uint8_t>((valueLo >> (byteIndex * 8u)) & 0xFFu);
    }
    return static_cast<uint8_t>((valueHi >> ((byteIndex - 8u) * 8u)) & 0xFFu);
}

#ifndef PS2X_ENGINE_SYNC_WATCH
#define PS2X_ENGINE_SYNC_WATCH 0
#endif

#ifndef PS2X_T10_SLOT_WATCH
#define PS2X_T10_SLOT_WATCH 0
#endif

#if PS2X_T10_SLOT_WATCH
// Vanguard M3-T10 diagnostic: log every guest store into the SDK TIMER0
// dispatcher handler slots (8 x 4 B at 0x5109F0, walked by sub_00355300 with
// `lw/jalr`). Zero in the ELF; the run showed them filled with foreign code
// words / ASCII ("Read") late in the run — name the exact writing PC here.
inline bool ps2T10SlotWatchHit(uint32_t guestAddr)
{
    return guestAddr - 0x5109F0u < 0x20u;
}

#endif

// Range variant for host-side bulk writers (stubs/services that memcpy into
// guest RAM): log once per op when [address, address+size) overlaps the slot
// window. Compiles to a no-op when PS2X_T10_SLOT_WATCH is off so the call
// sites do not need their own guards.
inline void ps2T10RangeWatch(const char *op, uint32_t address, uint64_t size, const R5900Context *ctx)
{
#if PS2X_T10_SLOT_WATCH
    constexpr uint32_t kSlotBase = 0x5109F0u;
    constexpr uint32_t kSlotEnd = kSlotBase + 0x20u;
    const uint64_t writeEnd = static_cast<uint64_t>(address) + size;
    if (address < kSlotEnd && writeEnd > kSlotBase)
    {
        static std::atomic<uint32_t> s_logged{0};
        if (s_logged.fetch_add(1, std::memory_order_relaxed) < 256u)
        {
            const uint32_t pc = ctx ? ctx->pc : 0u;
            const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
            RUNTIME_LOG("[t10-any] " << (op ? op : "write")
                                     << " pc=0x" << std::hex << pc
                                     << " ra=0x" << ra
                                     << " addr=0x" << address
                                     << "+0x" << size << std::dec
                                     << std::endl);
        }
    }
#else
    (void)op;
    (void)address;
    (void)size;
    (void)ctx;
#endif
}

// Vanguard M5-T4 diagnostic: armed saved-ra-slot watch. The T3 decode showed
// a run dying at pc=0x20 because the saved $ra of sub_00462410 (frame 0x50,
// ra slot at sp+0x10, written by `sd ra,0x10(sp)` @0x462428) had been
// overwritten with 0x20 by an unknown writer while the frame was live. This
// watch arms on entry to the frame function, derives the slot address from
// the entry sp, and names every write that touches the slot while the frame
// is live — the legit prologue store is tagged "expected", anything else is
// FOREIGN. A verify at every guest branch catches writers that bypass the
// per-store hooks (host services writing rdram directly).
//
// Opt-in (default off, zero behavior change):
//   PS2X_RA_SLOT_WATCH=1       enable
//   PS2X_RA_SLOT_ENTRY=0x...   frame function entry (default 0x462410)
//   PS2X_RA_SLOT_FRAME=0x50    frame size (sp decrement at entry)
//   PS2X_RA_SLOT_RA_OFF=0x10   offset of the saved ra inside the frame
//   PS2X_RA_SLOT_SD_PC=0x...   pc of the prologue `sd ra` (default entry+0x18)
//   PS2X_RA_SLOT_RETURN=0x...  pc of the epilogue `jr ra` (disarm site)
struct Ps2RaSlotWatchState
{
    std::atomic<bool> armed{false};
    std::atomic<bool> validated{false};
    std::atomic<uint32_t> slotBase{0};
    std::atomic<uint32_t> expectedRa{0};
    std::atomic<uint32_t> armTid{0};
    std::atomic<uint32_t> logs{0};
};

inline Ps2RaSlotWatchState g_ps2RaSlotWatch;

inline bool ps2RaSlotWatchEnabled()
{
    static const bool s_enabled = []()
    {
        const char *env = std::getenv("PS2X_RA_SLOT_WATCH");
        return env != nullptr && env[0] == '1';
    }();
    return s_enabled;
}

struct Ps2RaSlotWatchConfig
{
    uint32_t entry = 0x462410u;
    uint32_t frameSize = 0x50u;
    uint32_t raOffset = 0x10u;
    uint32_t sdPc = 0x462428u;
    uint32_t returnPc = 0x462500u;
};

// Called per guest branch: parse the env block once (function-local static).
inline const Ps2RaSlotWatchConfig &ps2RaSlotWatchConfig()
{
    static const Ps2RaSlotWatchConfig s_config = []()
    {
        Ps2RaSlotWatchConfig c;
        const auto read = [](const char *name, uint32_t fallback)
        {
            const char *env = std::getenv(name);
            return env != nullptr ? static_cast<uint32_t>(std::strtoull(env, nullptr, 0)) : fallback;
        };
        c.entry = read("PS2X_RA_SLOT_ENTRY", c.entry);
        c.frameSize = read("PS2X_RA_SLOT_FRAME", c.frameSize);
        c.raOffset = read("PS2X_RA_SLOT_RA_OFF", c.raOffset);
        c.sdPc = read("PS2X_RA_SLOT_SD_PC", c.sdPc);
        c.returnPc = read("PS2X_RA_SLOT_RETURN", c.returnPc);
        return c;
    }();
    return s_config;
}

inline void ps2RaSlotWatchLogClobber(const char *tag,
                                     uint32_t writerPc,
                                     uint32_t writerRa,
                                     uint32_t writerSp,
                                     uint32_t addr,
                                     uint32_t size,
                                     uint64_t valueLo,
                                     const char *op)
{
    if (g_ps2RaSlotWatch.logs.fetch_add(1, std::memory_order_relaxed) >= 64u)
    {
        return;
    }
    RUNTIME_LOG("[ra-slot-watch] " << tag << ' ' << (op != nullptr ? op : "write")
                                   << " writerPc=0x" << std::hex << writerPc
                                   << " writerRa=0x" << writerRa
                                   << " writerSp=0x" << writerSp
                                   << " addr=0x" << addr
                                   << " size=0x" << size
                                   << " val=0x" << valueLo
                                   << " slot=0x" << g_ps2RaSlotWatch.slotBase.load(std::memory_order_relaxed)
                                   << " expectedRa=0x" << g_ps2RaSlotWatch.expectedRa.load(std::memory_order_relaxed)
                                   << std::dec << std::endl);
}

inline void ps2RaSlotWatchCheckWrite(uint32_t addr,
                                     uint32_t size,
                                     uint64_t valueLo,
                                     const char *op,
                                     const R5900Context *ctx)
{
    if (!ps2RaSlotWatchEnabled() || !g_ps2RaSlotWatch.armed.load(std::memory_order_relaxed))
    {
        return;
    }
    const uint32_t base = g_ps2RaSlotWatch.slotBase.load(std::memory_order_relaxed);
    const uint64_t writeEnd = static_cast<uint64_t>(addr) + size;
    if (addr >= base + 8u || writeEnd <= base)
    {
        return;
    }
    const uint32_t writerPc = ctx ? ctx->pc : 0u;
    const uint32_t sdPc = ps2RaSlotWatchConfig().sdPc;
    if (writerPc != 0u && writerPc == sdPc)
    {
        if (!g_ps2RaSlotWatch.validated.exchange(true, std::memory_order_relaxed))
        {
            ps2RaSlotWatchLogClobber("expected", writerPc,
                                     ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u,
                                     ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u,
                                     addr, size, valueLo, op);
        }
        return;
    }
    if (!g_ps2RaSlotWatch.validated.load(std::memory_order_relaxed))
    {
        return; // before the prologue store the slot holds stale data
    }
    ps2RaSlotWatchLogClobber("FOREIGN", writerPc,
                             ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u,
                             ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[29], 0)) : 0u,
                             addr, size, valueLo, op);
}

// Branch-site verify: catch writers that bypass ps2TraceGuestWrite (host
// services writing rdram directly). Called from dispatchGuestBranch so a
// change between two guest branches pins the window to that site.
inline void ps2RaSlotWatchVerify(uint8_t *rdram, uint32_t sourcePc, uint32_t targetPc, uint32_t kind)
{
    if (!ps2RaSlotWatchEnabled() || !g_ps2RaSlotWatch.armed.load(std::memory_order_relaxed) ||
        rdram == nullptr)
    {
        return;
    }
    if (!g_ps2RaSlotWatch.validated.load(std::memory_order_relaxed))
    {
        return;
    }
    const uint32_t base = g_ps2RaSlotWatch.slotBase.load(std::memory_order_relaxed);
    if (base + 8u > PS2_RAM_SIZE)
    {
        return;
    }
    uint32_t words[2];
    std::memcpy(words, rdram + base, sizeof(words));
    const uint32_t expected = g_ps2RaSlotWatch.expectedRa.load(std::memory_order_relaxed);
    if (words[0] != expected || words[1] != 0u)
    {
        if (g_ps2RaSlotWatch.logs.fetch_add(1, std::memory_order_relaxed) < 64u)
        {
            RUNTIME_LOG("[ra-slot-watch] CLOBBER at-branch src=0x" << std::hex << sourcePc
                                                                   << " target=0x" << targetPc
                                                                   << " kind=" << kind
                                                                   << " slot=0x" << base
                                                                   << " now=[0x" << words[0]
                                                                   << ",0x" << words[1]
                                                                   << "] expectedRa=0x" << expected
                                                                   << std::dec << std::endl);
        }
    }
}

#if PS2X_ENGINE_SYNC_WATCH
// M3-T11: trigger-gated capture of the CD driver's staging copy. The boot
// dir-parse floods a static watch window (8192 cap gone before the reload),
// so the window is only live once dispatchGuestBranch saw the reload's
// request enqueue (src 0x11E954) and for the next N window hits after that.
inline std::atomic<uint32_t> g_ps2T11StoreWindow{0};

inline bool ps2EngineSyncWatchHit(uint32_t guestAddr, const uint8_t *rdram)
{
    // Engine sync struct: stage gate 0x642DC0, DF70 repeat counter 0x642DD0,
    // stream ring state 0x642DE8..
    if (guestAddr - 0x642DC0u < 0x30u)
    {
        return true;
    }
    // M3-T11: staging buffer 0x642E40 (sceCdRead buf) plus the driver record
    // at 0x642DE8 in front of it — captured only while the T11 window is open.
    // Also the reload blob buffer 0x1F18E00 (0x850 B window): whoever writes
    // it with the +8-shifted BRGR bytes is the bug this task hunts.
    if (g_ps2T11StoreWindow.load(std::memory_order_relaxed) > 0u &&
        (guestAddr - 0x642DE8u < 0x1B8u || guestAddr - 0x1F18E00u < 0x850u))
    {
        if (g_ps2T11StoreWindow.fetch_sub(1, std::memory_order_relaxed) <= 1u)
        {
            g_ps2T11StoreWindow.store(0, std::memory_order_relaxed);
        }
        return true;
    }
    // Deferred file-I/O arm record: flag 0x6480B8, fd 0x6480BC, buf 0x6480C0, size 0x6480C8.
    if (guestAddr - 0x6480B8u < 0x18u)
    {
        return true;
    }
    // Pending-completion pointer consumed by the pump sub_00340320.
    if (guestAddr == 0x647F6Cu)
    {
        return true;
    }
    // Request records (48 bytes each) of the table whose base sits at 0x647F70.
    if (rdram != nullptr)
    {
        const uint32_t tableBase = *reinterpret_cast<const uint32_t *>(rdram + 0x647F70u);
        if (tableBase != 0u && guestAddr - tableBase < 64u * 48u)
        {
            return true;
        }
    }
    // M3-T5: stream-ring channel object (0x68F710, obj+0..0x40 — the head at
    // +0x24 aliases the inline sentinel's next at 0x68F734) and the pump
    // walk-start globals 0x4FEB80/0x4FEB84: name whoever zeroes the ring.
    if (guestAddr - 0x68F710u < 0x40u)
    {
        return true;
    }
    if (guestAddr - 0x4FEB80u < 8u)
    {
        return true;
    }
    return false;
}
#endif

inline void ps2TraceGuestWrite(uint8_t *rdram,
                               uint32_t guestAddr,
                               uint32_t size,
                               uint64_t valueLo,
                               uint64_t valueHi,
                               const char *op,
                               const R5900Context *ctx)
{
    (void)valueHi;
    (void)op;
#if PS2X_T10_SLOT_WATCH
    if (size <= 16u && ps2T10SlotWatchHit(guestAddr))
    {
        static std::atomic<uint32_t> t10SlotWatchLogged{0};
        if (t10SlotWatchLogged.load(std::memory_order_relaxed) < 4096)
        {
            t10SlotWatchLogged.fetch_add(1, std::memory_order_relaxed);
            const uint32_t pc = ctx ? ctx->pc : 0u;
            const uint32_t ra = ctx ? static_cast<uint32_t>(_mm_extract_epi32(ctx->r[31], 0)) : 0u;
            RUNTIME_LOG("[t10-write] " << op << " pc=0x" << std::hex << pc
                                       << " ra=0x" << ra
                                       << " addr=0x" << guestAddr
                                       << " val=0x" << valueLo
                                       << ":" << valueHi << std::dec
                                       << std::endl);
        }
    }
#endif
    // Vanguard M5-T4: armed saved-ra-slot watch (env opt-in).
    ps2RaSlotWatchCheckWrite(guestAddr, size, valueLo, op, ctx);
#if PS2X_ENGINE_SYNC_WATCH
    // Vanguard bring-up diagnostic: log guest writes into the engine's
    // cooperative sync state (gate/counter/ring, deferred I/O record,
    // completion pointer, request records) with the writing PC, rate-limited.
    // M3-T7: also log 16-byte (sq) writes — a 128-bit node write was the one
    // blind spot when hunting the heap-sentinel clobber at 0x68F730/34.
    if (size <= 16u && rdram != nullptr)
    {
        static std::atomic<uint32_t> watchLogged{0};
        if (watchLogged.load(std::memory_order_relaxed) < 8192 &&
            ps2EngineSyncWatchHit(guestAddr, rdram))
        {
            watchLogged.fetch_add(1, std::memory_order_relaxed);
            const uint32_t pc = ctx ? ctx->pc : 0u;
            RUNTIME_LOG("[gsync-watch] " << op << " pc=0x" << std::hex << pc
                                         << " addr=0x" << guestAddr
                                         << " val=0x" << valueLo
                                         << ":" << valueHi << std::dec
                                         << std::endl);
        }
    }
#else
    (void)rdram;
    (void)guestAddr;
    (void)size;
    (void)valueLo;
    (void)ctx;
    // TODO we dont need this anymore so on next release it will be deleted
#endif
}

// Host bulk-write diagnostics (memcpy/memset/DMA services writing guest RAM).
// Long-standing callers, historically a no-op ("we dont need this anymore").
// Vanguard M3-T11 needed it back: hunting which service fills a guest buffer
// costs one full probe build per blind guess, so make the trace opt-in via
// environment instead of deleted:
//   PS2X_TRACE_GUEST_RANGE_WRITES=1      log every bulk write (op, addr, size, pc)
//   PS2X_TRACE_GUEST_RANGE_ADDR=0xADDR   only writes overlapping [addr, addr+SIZE)
//   PS2X_TRACE_GUEST_RANGE_SIZE=0xN      (with ADDR; default 0x1000)
// Rate-limited to the first 4096 hits per run. Default (env unset): silent,
// zero behavior change.
inline void ps2TraceGuestRangeWrite(uint8_t *rdram,
                                    uint32_t guestAddr,
                                    uint32_t size,
                                    const char *op,
                                    const R5900Context *ctx)
{
    static const bool s_enabled = []()
    {
        const char *env = std::getenv("PS2X_TRACE_GUEST_RANGE_WRITES");
        return env != nullptr && env[0] == '1';
    }();
    if (!s_enabled)
    {
        return;
    }
    static const uint32_t s_watchAddr = []()
    {
        const char *env = std::getenv("PS2X_TRACE_GUEST_RANGE_ADDR");
        return env != nullptr ? static_cast<uint32_t>(std::strtoull(env, nullptr, 0)) : 0u;
    }();
    static const uint32_t s_watchSize = []()
    {
        const char *env = std::getenv("PS2X_TRACE_GUEST_RANGE_SIZE");
        return env != nullptr ? static_cast<uint32_t>(std::strtoull(env, nullptr, 0)) : 0x1000u;
    }();

    static std::atomic<uint32_t> s_logged{0};
    if (s_logged.load(std::memory_order_relaxed) >= 4096u)
    {
        return;
    }
    // Vanguard M5-T4: the armed ra-slot watch must see host bulk writers too
    // (checked before the env range filter, which is a different feature).
    ps2RaSlotWatchCheckWrite(guestAddr, size, 0u, op, ctx);
    const uint64_t writeEnd = static_cast<uint64_t>(guestAddr) + size;
    const bool overlaps = s_watchAddr == 0u ||
                          (guestAddr < s_watchAddr + s_watchSize && writeEnd > s_watchAddr);
    if (!overlaps)
    {
        return;
    }
    s_logged.fetch_add(1, std::memory_order_relaxed);
    const uint32_t pc = ctx ? ctx->pc : 0u;
    RUNTIME_LOG("[guest-range-write] " << (op != nullptr ? op : "write")
                                       << " addr=0x" << std::hex << guestAddr
                                       << " size=0x" << size
                                       << " pc=0x" << pc << std::dec
                                       << std::endl);
}

class PS2Runtime; // M5-T11 probe forward declaration

// -----------------------------------------------------------------------------
// Vanguard M5-T11 probe (runtime-only, opt-in PS2X_ENABLE_M5T11=1): pin who
// poisons the engine event-dispatcher's control flow with 0x100 (T10 decode:
// tid=1 invalid-jump badPc=0x100 ra=0x100, deterministic, after the MPEG ring
// dries). Instruments:
//   [m5t11-edge]  ring of the last 128 inter-function edges (src/dst/kind/sp)
//   [m5t11-bad]   one-shot catch of ANY branch targeting a non-code address
//                 (<0x100000, !=0): full GPRs + stack scan + ring tail
//   [m5t11-task]  dispatcher-entry capture (sub_00462508): manager words +
//                 task-block watch ([*(a2+4)]..+0x1C, incl. the [task+0xC]
//                 handler slot the jalr takes) with the writing branch site
//   [m5t11-async] every reserveAsyncCallbackStack block (base/top): the T7
//                 async stacks march DOWN from 0x1FF8000; if they touch live
//                 game data the registry names the overlap at death time
bool ps2xM5T11Enabled();
void ps2xM5T11RecordEdge(uint32_t src, uint32_t dst, uint32_t kind, uint32_t sp, uint32_t ra);
void ps2xM5T11CatchBogusTarget(const PS2Runtime *runtime, uint8_t *rdram, R5900Context *ctx, uint32_t src, uint32_t dst, uint32_t kind);
void ps2xM5T11NoteDispatcherEntry(uint8_t *rdram, uint32_t a0, uint32_t a2);
void ps2xM5T11VerifyDispatcherSlot(uint8_t *rdram, uint32_t srcPc);
void ps2xM5T11NoteAsyncBlock(uint32_t base, uint32_t top);
void ps2xM5T11FormatRing(std::ostringstream &oss, uint32_t tail);
void ps2xM5T11FormatAsyncBlocks(std::ostringstream &oss);

class PS2Runtime
{
public:
    struct IoPaths
    {
        std::filesystem::path elfPath;
        std::filesystem::path elfDirectory;
        std::filesystem::path hostRoot;
        std::filesystem::path cdRoot;
        std::filesystem::path mcRoot;
        std::filesystem::path cdImage;
    };

    PS2Runtime();
    ~PS2Runtime();

    bool initialize(const char *title = "PS2 Game");
    bool syncCoreSubsystems();
    bool loadELF(const std::string &elfPath);
    void run();

    [[nodiscard]] ps2x::iop::ModuleLoadResult loadIopModule(std::string_view path, const void *arguments = nullptr, uint32_t argumentSize = 0);
    [[nodiscard]] ps2x::iop::ModuleLoadResult loadIopModuleBuffer(uint32_t guestAddress, const void *arguments = nullptr, uint32_t argumentSize = 0);
    [[nodiscard]] bool stopIopModule(int32_t moduleId, int32_t *result = nullptr);
    [[nodiscard]] ps2x::iop::DebugSnapshot iopDebugSnapshot() const;
    uint32_t allocateIopMemory(uint32_t size, uint32_t alignment = 16u);
    bool freeIopMemory(uint32_t address);
    bool readIopMemory(uint32_t address, void *destination, size_t size) const;
    bool writeIopMemory(uint32_t address, const void *source, size_t size);
    bool zeroIopMemory(uint32_t address, size_t size);
    bool isIopMemoryRange(uint32_t address, size_t size) const;

    using DebugUiCallback = void (*)(PS2Runtime &runtime, void *userData);
    void setDebugUiCallbacks(DebugUiCallback initCallback,
                             DebugUiCallback drawCallback,
                             DebugUiCallback shutdownCallback,
                             void *userData);

    using RecompiledFunction = void (*)(uint8_t *, R5900Context *, PS2Runtime *);

    enum class GuestBranchKind
    {
        DirectJump,
        DirectCall,
        IndirectJump,
        IndirectCall,
        Return,
    };

    enum class MissingFunctionPolicy : uint32_t
    {
        // Strict mode for tests/CI: log the bad target and request the runtime to stop.
        Stop = 0,

        // Debug mode: log once, leave ctx->pc on the bad target, and let the caller unwind.
        ContinueToTarget = 1,

        // Debug mode: same as ContinueToTarget, but triggers a debugger break once on MSVC.
        BreakOnce = 2,

        // Escape hatch only: skip missing calls by returning to fallthrough (it can hide guest bugs)
        SkipCallDebug = 3,
    };

    bool replaceFunction(uint32_t address, RecompiledFunction func);
    // TODO remove this later need to update all tests
    bool registerFunction(uint32_t address, RecompiledFunction func);
    RecompiledFunction lookupFunction(uint32_t address);
    bool hasFunction(uint32_t address) const;
    bool dispatchGuestBranch(uint8_t *rdram,
                             R5900Context *ctx,
                             uint32_t targetPc,
                             uint32_t sourcePc,
                             uint32_t fallthroughPc,
                             GuestBranchKind kind,
                             const char *debugName);
    void reportMissingFunction(uint8_t *rdram,
                               R5900Context *ctx,
                               uint32_t targetPc,
                               uint32_t sourcePc,
                               GuestBranchKind kind,
                               const char *debugName);
    void setMissingFunctionPolicy(MissingFunctionPolicy policy);
    MissingFunctionPolicy missingFunctionPolicy() const;
    void resetMissingFunctionReportOnce();

    static const IoPaths &getIoPaths();
    static void setIoPaths(const IoPaths &paths);
    static void configureIoPathsFromElf(const std::string &elfPath);
    static void setCdImagePath(const std::string &imagePath);

    void SignalException(R5900Context *ctx, PS2Exception exception);

    void executeVU0Microprogram(uint8_t *rdram, R5900Context *ctx, uint32_t address);
    void vu0StartMicroProgram(uint8_t *rdram, R5900Context *ctx, uint32_t address);

public:
    void handleSyscall(uint8_t *rdram, R5900Context *ctx);
    void handleSyscall(uint8_t *rdram, R5900Context *ctx, uint32_t encodedSyscallId);
    void handleBreak(uint8_t *rdram, R5900Context *ctx);

    void handleTrap(uint8_t *rdram, R5900Context *ctx);
    void handleTLBR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWI(uint8_t *rdram, R5900Context *ctx);
    void handleTLBWR(uint8_t *rdram, R5900Context *ctx);
    void handleTLBP(uint8_t *rdram, R5900Context *ctx);
    void clearLLBit(R5900Context *ctx);
    void configureGuestHeap(uint32_t guestBase, uint32_t guestLimit = PS2_RAM_SIZE);
    uint32_t guestMalloc(uint32_t size, uint32_t alignment = 16u);
    uint32_t guestCalloc(uint32_t count, uint32_t size, uint32_t alignment = 16u);
    uint32_t guestRealloc(uint32_t guestAddr, uint32_t newSize, uint32_t alignment = 16u);
    void guestFree(uint32_t guestAddr);
    uint32_t guestHeapBase() const;
    uint32_t guestHeapEnd() const;
    uint32_t guestHeapLimit() const;
    uint32_t reserveAsyncCallbackStack(uint32_t size, uint32_t alignment = 16u);

    void drainCompletedDmacHandlers(uint8_t *rdram);

    void requestStop();
    bool isStopRequested() const;

    EeScheduler &eeScheduler();
    const EeScheduler &eeScheduler() const;
    void postEeEvent(EeEvent event);
    bool eeCheckpointDue(uint32_t cycles = 32u) noexcept;
    [[noreturn]] void eeWaitVSyncTicks(uint32_t ticks, uint32_t resumePc);

    struct EeExitHandlerRegistration
    {
        uint32_t function = 0;
        uint32_t argument = 0;
    };
    void addEeExitHandler(int threadId, uint32_t function, uint32_t argument);
    std::vector<EeExitHandlerRegistration> takeEeExitHandlers(int threadId);
    void removeEeExitHandlers(int threadId);
    bool findEeSyscallOverride(uint32_t syscallNumber, uint32_t &handler) const;
    void setEeSyscallOverride(uint8_t *rdram, uint32_t syscallNumber, uint32_t handler);
    void initializeEeKernelState(uint8_t *rdram);

    uint8_t Load8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint16_t Load16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint32_t Load32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    uint64_t Load64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);
    __m128i Load128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr);

    void Store8(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint8_t value);
    void Store16(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint16_t value);
    void Store32(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint32_t value);
    void Store64(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, uint64_t value);
    void Store128(uint8_t *rdram, R5900Context *ctx, uint32_t vaddr, __m128i value);
    void kickGifDmaChainFromMMIO(uint8_t *rdram,
                                 R5900Context *ctx,
                                 uint32_t dPcrValue,
                                 uint32_t dStatValue,
                                 uint32_t tadr,
                                 uint32_t chcr);

    static inline bool isSpecialAddress(uint32_t addr)
    {
        return Ps2IsSpecialAddress(addr);
    }

public:
    inline R5900Context &cpu() { return m_cpuContext; }
    inline const R5900Context &cpu() const { return m_cpuContext; }

    inline PS2Memory &memory() { return m_memory; }
    inline const PS2Memory &memory() const { return m_memory; }

    inline GS &gs() { return m_gs; }
    inline const GS &gs() const { return m_gs; }
    inline GifArbiter &gifArbiter() { return m_gifArbiter; }
    inline const GifArbiter &gifArbiter() const { return m_gifArbiter; }
    inline VU1Interpreter &vu0() { return m_vu0; }
    inline const VU1Interpreter &vu0() const { return m_vu0; }
    inline VU1Interpreter &vu1() { return m_vu1; }
    inline const VU1Interpreter &vu1() const { return m_vu1; }

    inline PS2AudioBackend &audioBackend() { return m_audioBackend; }
    inline const PS2AudioBackend &audioBackend() const { return m_audioBackend; }
    inline PSPadBackend &padBackend() { return m_padBackend; }
    inline const PSPadBackend &padBackend() const { return m_padBackend; }
    inline PS2RomDevice &romDevice() { return m_romDevice; }
    inline const PS2RomDevice &romDevice() const { return m_romDevice; }
    inline PS2Vfs &vfs() { return m_vfs; }
    inline const PS2Vfs &vfs() const { return m_vfs; }

private:
    struct GuestHeapBlock
    {
        uint32_t addr = 0;
        uint32_t size = 0;
        bool free = true;
    };

    static uint32_t alignGuestHeapValue(uint32_t value, uint32_t alignment);
    static bool isGuestHeapAlignmentValid(uint32_t alignment);
    static uint32_t normalizeGuestHeapAlignment(uint32_t alignment);
    uint32_t clampGuestHeapBase(uint32_t guestBase) const;
    uint32_t clampGuestHeapLimit(uint32_t guestLimit) const;
    void resetGuestHeapLocked(uint32_t guestBase, uint32_t guestLimit);
    void ensureGuestHeapInitializedLocked();
    int32_t findGuestHeapBlockIndexLocked(uint32_t guestAddr) const;
    uint32_t allocateGuestBlockLocked(uint32_t size, uint32_t alignment);
    void freeGuestBlockLocked(uint32_t guestAddr);
    void coalesceGuestHeapLocked();
    void HandleIntegerOverflow(R5900Context *ctx);

    [[nodiscard]] ps2x::iop::RpcAbi selectIopRpcAbi(const ps2x::iop::RpcAbiRequest &request) const;
    [[nodiscard]] bool canBindIopRpc(uint32_t sid) const noexcept;
    [[nodiscard]] ps2x::iop::RpcResult handleIopRpc(uint8_t *rdram, R5900Context *ctx, ps2x::iop::RpcRequest request);
    void notifyIopSifTransfer(uint8_t *rdram, const ps2x::iop::SifTransfer &transfer);
    void advanceIopEeCycles(uint64_t eeCycles) noexcept;
    void resetIop();

    friend class PS2IopTransport;
    friend class EeScheduler;

private:
    PS2Memory m_memory;
    GifArbiter m_gifArbiter;
    GS m_gs;
    std::unique_ptr<PS2IopHostAdapter> m_iopHost;
    std::unique_ptr<ps2x::iop::IopSubsystem> m_iopSubsystem;
    PS2AudioBackend m_audioBackend;
    PSPadBackend m_padBackend;
    PS2RomDevice m_romDevice;
    PS2Vfs m_vfs;
    VU1Interpreter m_vu0{VU1Interpreter::Unit::VU0};
    VU1Interpreter m_vu1{VU1Interpreter::Unit::VU1};
    R5900Context m_cpuContext;
    std::unique_ptr<EeScheduler> m_eeScheduler;
    mutable std::mutex m_eeKernelStateMutex;
    std::unordered_map<int, std::vector<EeExitHandlerRegistration>> m_eeExitHandlers;
    std::unordered_map<uint32_t, uint32_t> m_eeSyscallOverrides;
    std::unordered_set<uint32_t> m_eeSyscallMirrorAddresses;
    mutable std::mutex m_guestHeapMutex;
    mutable std::mutex m_asyncCallbackStackMutex;
    std::vector<GuestHeapBlock> m_guestHeapBlocks;
    uint32_t m_guestHeapBase = 0x00100000u;
    uint32_t m_guestHeapEnd = 0x00100000u;
    uint32_t m_guestHeapLimit = PS2_RAM_SIZE;
    uint32_t m_guestHeapSuggestedBase = 0x00100000u;
    bool m_guestHeapConfigured = false;
    uint32_t m_asyncCallbackStackFloor = 0x01F00000u;
    // Starts BELOW the EE main thread's stack: the kernel anchors the main
    // thread at the top of RAM (main sp = PS2_RAM_SIZE - 0x10) with the
    // kernel-default 32 KiB stack, so callback chunks carved from plain
    // PS2_RAM_SIZE would hand out stack pointers inside the main thread's
    // live frames (M5-T4: vblank/alarm/SIF invocations smashed saved $ra
    // words and parked threads silently DORMANT on garbage pcs).
    uint32_t m_asyncCallbackStackTop = PS2_RAM_SIZE - kAsyncCallbackStackMainReserve;

    std::atomic<uint32_t> m_missingFunctionPolicy{static_cast<uint32_t>(MissingFunctionPolicy::ContinueToTarget)};
    std::atomic<bool> m_missingFunctionReported{false};
    std::atomic<bool> m_stopRequested{false};
    DebugUiCallback m_debugUiInitCallback = nullptr;
    DebugUiCallback m_debugUiDrawCallback = nullptr;
    DebugUiCallback m_debugUiShutdownCallback = nullptr;
    void *m_debugUiUserData = nullptr;
    bool m_debugUiInitialized = false;

public:
    std::atomic<uint32_t> m_debugPc{0};
    std::atomic<uint32_t> m_debugRa{0};
    std::atomic<uint32_t> m_debugSp{0};
    std::atomic<uint32_t> m_debugGp{0};

private:
    struct LoadedModule
    {
        std::string name;
        uint32_t baseAddress;
        size_t size;
        bool active;
    };

    std::vector<LoadedModule> m_loadedModules;
    uint8_t *m_boundRdram = nullptr;
    uint8_t *m_boundGSVram = nullptr;
};

// Generated by ps2xRecomp in ps2xRuntime/src/runner/register_functions.cpp.
extern const uint32_t g_ps2RecompiledFunctionTableBase;
extern const uint32_t g_ps2RecompiledFunctionTableEnd;
extern const uint32_t g_ps2RecompiledFunctionTableSlotCount;
extern PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[];

#endif // PS2_RUNTIME_H
