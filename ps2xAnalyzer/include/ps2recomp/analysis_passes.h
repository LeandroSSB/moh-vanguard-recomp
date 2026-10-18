#ifndef PS2RECOMP_ANALYSIS_PASSES_H
#define PS2RECOMP_ANALYSIS_PASSES_H

#include "ps2recomp/types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ps2recomp
{
    class AnalysisPasses
    {
    public:
        static bool hasHardwareIOSignal(const std::vector<Instruction> &instructions);
        static bool hasLargeComplexMMISignal(const std::vector<Instruction> &instructions,
                                             size_t largeInstructionThreshold = 500);
        static bool hasSelfModifyingSignal(const std::vector<Instruction> &instructions,
                                           const std::vector<Section> &sections);
        static std::vector<JumpTable> detectJumpTables(
            const std::vector<Instruction> &instructions,
            const std::vector<Section> &sections,
            const std::function<bool(uint32_t, uint32_t &)> &readWord);
        static std::unordered_set<std::string> findRecursiveFunctions(
            const std::unordered_map<std::string, std::vector<std::string>> &callGraph);

        // A word that compilers emit as the first instruction of a standard
        // function prologue (stack allocation or return-address save).
        static bool looksLikeFunctionPrologue(uint32_t instructionWord);

        // Scans every loadable section for 4-byte data words that point into
        // [textStart, textEnd) at an address that is not a known function
        // start and whose first instruction looks like a function prologue.
        // Such targets are callable guest entries registered through data
        // tables (callback chains); without an entry the runtime cannot
        // dispatch an indirect call to them.
        static std::vector<uint32_t> collectDataReferencedEntryCandidates(
            const std::vector<Section> &sections,
            uint32_t textStart,
            uint32_t textEnd,
            const std::unordered_set<uint32_t> &knownStarts);
    };
}

#endif // PS2RECOMP_ANALYSIS_PASSES_H
