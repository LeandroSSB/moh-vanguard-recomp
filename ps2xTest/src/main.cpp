#include "MiniTest.h"
#include <cstring>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string.h>

void register_code_generator_tests();
void register_r5900_decoder_tests();
void register_elf_analyzer_tests();
void register_pad_input_tests();
void register_ps2_input_script_tests();
void register_ps2_mkb_tests();
void register_ps2_runtime_io_tests();
void register_ps2_runtime_kernel_tests();
void register_ps2_runtime_interrupt_tests();
void register_ps2_memory_tests();
void register_ps2_vu1_tests();
void register_ps2_vu_tests();
void register_ps2_gs_tests();
void register_ps2_iop_tests();
void register_ps2_sif_rpc_tests();
void register_ps2_sif_dma_tests();
void register_ps2_recompiler_tests();
void register_ps2_runtime_expansion_tests();
void register_ps2_float_semantics_tests();
void reset_ps2_test_function_table();

int main()
{
    // PS2X_TEST_SUITE=<substring> registers only the matching suites
    // (diagnostic: shaves a full-suite run down to one suite).
    const char *suiteFilter = std::getenv("PS2X_TEST_SUITE");
    const auto wants = [suiteFilter](const char *registration)
    { return suiteFilter == nullptr || std::strstr(registration, suiteFilter) != nullptr; };
    MiniTest::BeforeEach(reset_ps2_test_function_table);

    if (wants("register_code_generator_tests"))
        register_code_generator_tests();
    if (wants("register_r5900_decoder_tests"))
        register_r5900_decoder_tests();
    register_elf_analyzer_tests();
    if (wants("register_pad_input_tests"))
        register_pad_input_tests();
    if (wants("register_ps2_input_script_tests"))
        register_ps2_input_script_tests();
    if (wants("register_ps2_mkb_tests"))
        register_ps2_mkb_tests();
    if (wants("register_ps2_runtime_io_tests"))
        register_ps2_runtime_io_tests();
    if (wants("register_ps2_runtime_kernel_tests"))
        register_ps2_runtime_kernel_tests();
    if (wants("register_ps2_runtime_interrupt_tests"))
        register_ps2_runtime_interrupt_tests();
    if (wants("register_ps2_memory_tests"))
        register_ps2_memory_tests();
    if (wants("register_ps2_vu1_tests"))
        register_ps2_vu1_tests();
    if (wants("register_ps2_vu_tests"))
        register_ps2_vu_tests();
    if (wants("register_ps2_gs_tests"))
        register_ps2_gs_tests();
    if (wants("register_ps2_iop_tests"))
        register_ps2_iop_tests();
    if (wants("register_ps2_sif_rpc_tests"))
        register_ps2_sif_rpc_tests();
    if (wants("register_ps2_sif_dma_tests"))
        register_ps2_sif_dma_tests();
    if (wants("register_ps2_recompiler_tests"))
        register_ps2_recompiler_tests();
    if (wants("register_ps2_runtime_expansion_tests"))
        register_ps2_runtime_expansion_tests();
    if (wants("register_ps2_float_semantics_tests"))
        register_ps2_float_semantics_tests();
    int res = MiniTest::Run();
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(res);
}
