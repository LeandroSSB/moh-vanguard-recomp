# Research frontier — o circular idle pós-filme

Estado em 2026-10-26, decodado elo-a-elo (o diário completo está no repo
de trabalho; este doc é o mapa para quem continuar).

## O estado terminal

Pós-filme: main dorme por tick num alarme de kernel saudável
(SetAlarm(0xFC) → SleepThread(0x32), ~22k fires/900 s, 0 falhas — a
maquinaria de alarm/vsync da runtime está exonerada por medida),
bombeando a message-wait `sub_0033F208` que polla `*(rec+8)` — escrito
somente pelo completion bomb 0x340320. Gate 0x642DC0 = 0, deferredFlag
0x6480B8 = 0, worker tid2 dormindo em WaitSema(60) com zero tokens (zero
leituras outstanding), demand issuer 0x33DE50 worker-only, armer 0x33E398
dispara e no-opa em gate≠6, stage machine 0x33E108 zero entradas.

## Por que é circular

Cada elo é trabalhável apenas por outro elo do mesmo círculo: o worker
precisa de tokens (completions de CD) para emitir leituras; as leituras
produzem os tokens; o armer precisa do gate≠6 que só o worker seta. Na HW
esse estado não ocorre — a questão aberta é QUE divergência de timing ou
de estado anterior nos colocou aqui.

## O instrumento nomeado: A/B contra PCSX2

PCSX2 roda este jogo além da fronteira (boot → movie → loading →
front-end naturalmente). O protocolo: capturar o estado de RAM dos dois
lados no mesmo ponto lógico (pós-filme) e `abdiff` nomeia o primeiro
valor divergente — que é o input servido errado (contrato da runtime) ou
o estado do jogo divergente (timing). Requisitos: PCSX2 instalado + BIOS
do próprio console (dump legal). Regiões a capturar: gate 0x642DC0,
deferredFlag 0x6480B8, tabela 0x647F70, worker sema/gate, e o campo de
count do descritor de level (divergência nomeada: 0x5902F3D5 lido no
nop-scan em 2/5 runs).

## O que JÁ foi exonerado (não gaste de novo)

- A maquinaria de alarm/vsync da runtime (22k fires/900 s, 0 falhas)
- A entrega de invocações (µs-fast no mediano; carve dedicado; o freeze
  da main extinto; o shaping timer/alarm landed)
- A ponte EE→IOP e o endTimeslice do IOP (SNDDRV executando)
- O shape da reply cid-1 sintetizada (o handler consome só +0xC/+0x10;
  os 27 bytes zerados nunca são lidos)
- O disco (todas as leituras completam; o re-arm do SDK saudável)
