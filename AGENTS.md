# AGENTS.md — guia para agentes de desenvolvimento (e humanos)

Este repo é um port nativo de PS2 via recompilação estática. O desenvolvimento é
agent-driven: agentes fazem o ciclo instrumentar → decodificar → corrigir (TDD) →
verificar → documentar, um gate por vez. Este guia ensina o terreno.

## Onde cada mudança vai

| mudança | arquivo/lugar |
|---|---|
| Semântica do scheduler/clock/threads | `ps2xRuntime/src/lib/Kernel/EeScheduler.cpp` |
| Semântica de memória/DMA/timers HW | `ps2xRuntime/src/lib/ps2_memory.cpp` |
| Stubs de syscalls (CD, SIF, RPC, MPEG...) | `ps2xRuntime/src/lib/Kernel/Stubs/` |
| Handlers de syscall | `ps2xRuntime/src/lib/Kernel/Syscalls/` |
| Serviços IOP (RPC bridge, IRX) | `ps2xIOP/src/` |
| Descoberta de funções (analyzer) | `ps2xAnalyzer/src/` |
| Testes | `ps2xTest/src/` (MiniTest: `MiniTest::Case` + `tc.Run`) |
| Config do jogo (por usuário) | `config/recompiler.toml` (template), `mkb.ini` |

## Comandos

```powershell
# configure (uma vez)
cmake -S . -B out/build -DPS2X_ENABLE_AGRESSIVE_LOGS=ON

# build de um alvo
cmake --build <caminho ABSOLUTO do out/build> --config Release --target <alvo>

# suíte (SEMPRE da raiz do repo)
out\build\ps2xTest\Release\ps2x_tests.exe
```

Baseline atual: veja `console/STATE.md` se existir, senão o último número
documentado no diário (`docs/`).

## As minas (leia antes de qualquer build)

1. **`ps2xRuntime/src/runner/register_functions.cpp` é GERADO.** Depois de rodar o
   recompilador com um jogo real, ele cresce para ~519k linhas (a function table
   do engine). O template no git tem ~6 linhas. **NUNCA commite o estado gerado** —
   e antes de qualquer rebuild FULL, verifique `wc -l` (se ~6, o runner sai com a
   function table vazia e o guest morre na primeira instrução; restaure o gerado
   do seu diretório de gen). Builds incrementais podem linkar um obj unity stale
   cheio — é por isso que às vezes "funciona".
2. **Sync de fonte é por DIRETÓRIO, nunca arquivo-a-arquivo** (árvores espelhadas
   divergem silenciosamente e testes velhos rodam contra runtime novo).
3. **`cmake --build` com CAMINHO ABSOLUTO** (o cwd do shell oscila).
4. Builds longos (>10 min) em background com monitoramento — nunca mate um build
   para "tentar de novo".

## A disciplina de probes

Diagnóstico é por probes **opt-in** (env `PS2X_ENABLE_<TAG>=1`), log-only
(`RUNTIME_LOG`), com contador limitado (cap + reset), e **removidos (ou
formalizados como contrato) depois que o gate fecha**. Nunca um probe default-on
com endereços de um jogo específico no hot path.

## O loop de bring-up

1. Instrumentar o gate (probe opt-in nomeando o estado — pc/ra/valores).
2. Decodificar o elo quebrado no código GERADO (ele é o disassembly legível).
3. Corrigir via TDD (RED medido no shape exato do defeito — a história tem
   exemplos de REDs que pegaram um SEGUNDO defeito durante o primeiro).
4. Verificar em run longo (não-regressão + o gate seguinte).
5. Documentar no diário (causa raiz → fix → teste → números).

## A fronteira de pesquisa

O jogo está estável pós-filme; o front-end/menu não abre — o jogo entra num
circular idle decodado elo-a-elo (worker sem tokens ← sem leituras outstanding ←
emissor de demanda worker-only; armer no-op em gate≠6). Toda via de entrega de
invocação agora é moldada (defer-to-vsync + carve dedicado) e a família de mortes
pc=0x100 está extinta. A fronteira: decidir se a divergência restante vs hardware
é de timing (a corrente de fixes mudou o interleaving) ou de estado — o A/B contra
PCSX2 (ground truth) é o instrumento nomeado. Veja `docs/research-frontier.md`.

## Não faça

- Não commite dados do jogo (ISO/BIOS/gen output/chutes de memory dump).
- Não "conserte" comportamento do jogo com hacks de runtime — decode o contrato
  que a HW servia; inventar comportamento cria estados que a HW nunca alcança.
- Não esconda regressões em known-failures — diário e correção.
