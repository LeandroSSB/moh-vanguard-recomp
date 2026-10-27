# Bring-up — as 12+ correções, cada uma com causa raiz → fix → teste

O Vanguard é um jogo retail (ELF sem símbolos, loader próprio, streaming de
CD, middleware de som e um filme de logo por MPEG). Cada parede abaixo foi
decodada ao nível de instrução, corrigida com TDD na runtime, e verificada
em runs longos. Commits: branch `vanguard-bringup` (referenciados por SHA);
PRs upstream: [#398–#405](https://github.com/ran-j/PS2Recomp/pulls?q=is%3Apr+author%3ALeandroSSB).

## M2 — a pilha automática da main

**Sintoma:** a main "retornava" e a thread dormia para sempre no meio do
boot (tick 1800). **Causa raiz:** `SetupThread(stack=-1)` — a runtime
ancorava a pilha automática em `RAM_TOP − 0x28000`, colidindo com a arena
estática do jogo; o `memset` do primeiro malloc esmagou os frames vivos.
**Fix:** âncora no topo da RDRAM (`RAM_TOP − 0x10 − stackSize`), teste de
regressão com a geometria exata do jogo. Resultado: 54 → 1000+ funções
executando, SIF/IOP vivos.

## M3/M9 — o clock

**Sintoma:** entrega de vsync 6,6–7,6/s (11% do NTSC), jogo em slow-motion
estrutural (tbase 8,3/100/s). **Causa raiz (2 camadas):** (1) a entrega era
gateada 1:1 pelo throughput do executor (o portão duplo
`deadlineCycle && hostDeadline`); (2) os timers de hardware avançavam por
ciclos EXECUTADOS — a HW conta o barramento independente da CPU. **Fix:**
entrega wall-deadline-only com replay limitado + `advanceEeTimersToWall`.
Resultado: terminal 60,031/s (±0,15%), boot restaurado, tbase monotônica.

## M3 — o veneno de memória intermitente

**Sintoma:** 1-in-3 runs morriam em saltos selvagens (pc=0x100, vítimas
variáveis). **Causa raiz:** os blocos de callback async (16 KiB, chave
(thread,profundidade)) marchavam de 0x1FF8000 para baixo por cima da arena
malloc do jogo (que nunca consulta EndOfHeap). **Fix:** invocações na
pilha da thread dona + carve dedicado defendido
([0x1FD9000,0x1FE0000)) com guard de descida da main. Resultado: morte
extinta, primeira transição de tela da expedição.

## M3 — o sync do DMA do GIF

**Sintoma:** main parkada eternamente pollando `((CHCR>>28)&7)==7` (o END
tag no readback de CHCR.TAG; 5M+ leituras, 0 hits). **Causa raiz:** o walk
do runtime suspendia no primeiro tag IRQ e o drain deixava o TAG preso no
CNT. **Fix:** cadeias drenam ATÉ O END (o runtime completa DMA síncrono).
Resultado: spin extinto, o logo "LOADING" do jogo na tela.

## M4 — a fome de IOP

**Sintoma:** o middleware de som travado (mutex liberado 821/s cujo waiter
nunca adquiria); o filme sem áudio. **Causa raiz (2 defeitos):** a ponte
EE→IOP descartava TODO SIF transfer EE→IOP (`onSifTransfer` no-op), e o
`endTimeslice` do kernel IOP transformava toda thread de IRX residente que
bloqueava em import em Dormant para sempre. **Fix:** bridge acorda
servidores SetRpcQueue + agenda a interrupção SIF1; estados bloqueados
sobrevivem à timeslice. Resultado: SNDDRV executando (~330M instr/10 s),
filme com áudio.

## M5 — o callback need-data do MPEG

**Sintoma:** GetPicture esperando para sempre (`sawInput=0`). **Causa
raiz:** o runtime armazenava o callback type-1 registrado via
`sceMpegAddCallback` mas só o caminho DemuxPssRing o disparava — um jogo
que alimenta via `sceMpegAddBs` starvava. **Fix:** o starve enfileira o
callback need-data registrado (padrão executor-legal do fix de CD). E o
wrapper de wake: acordar o GetPicture parkado em QUALQUER feed (o parser
bufferiza parciais), e o código de sequence-end (0x1B7) encerra o
playback.

## M5 — o muro de demanda pós-filme

**Estado:** após o filme, main entra num message-wait e o jogo entra num
circular idle decodado elo-a-elo (worker sem tokens ← sem leituras
outstanding ← emissor de demanda worker-only; armer no-op em gate≠6; stage
machine zero entradas). Corrigir inventaria comportamento do jogo — o A/B
contra PCSX2 (ground truth) é o instrumento nomeado para achar a primeira
divergência. **M9-T1 (timers no wall clock) avançou a máquina pela primeira
vez pós-fix** (main → RCMP band → player-init) — a ordem de entrega vs o
fill do builder é o gate vivo.
