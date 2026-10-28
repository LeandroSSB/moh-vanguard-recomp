# Medal of Honor: Vanguard — Recompilação Estática PS2

[![CI](https://github.com/LeandroSSB/moh-vanguard-recomp/actions/workflows/ci.yml/badge.svg)](https://github.com/LeandroSSB/moh-vanguard-recomp/actions/workflows/ci.yml)

Port nativo de PC de **Medal of Honor: Vanguard (PS2, NTSC-U SLUS-215.97)** via recompilação estática com [PS2Recomp](https://github.com/ran-j/PS2Recomp). Controles de FPS com mouse + teclado, clock NTSC 59.94 fiel, e o diário completo da pesquisa de bring-up.

[![status](https://img.shields.io/badge/status-not_playable_end--to--end_yet-orange)](docs/research-frontier.md)
[![bring-up](https://img.shields.io/badge/bring--up-7_of_9_stages_measured-green)](#onde-o-port-est%C3%A1-medido)
[![clock](https://img.shields.io/badge/clock-59.94_NTSC_faithful-green)](#onde-o-port-est%C3%A1-medido)
[![soak](https://img.shields.io/badge/soak-0_deaths_in_5%2B_h_runs-green)](#onde-o-port-est%C3%A1-medido)

> ⚠️ **Status: em progresso.** O jogo boota, roda o filme de logo da EA completo com áudio, e roda estável pós-filme (a main viva além de todas as paredes anteriores — o primeiro avanço da máquina de estágios pousou em 26/10/2026). O front-end/menu **ainda não abre**: o jogo entra num circular idle pós-filme decodado ao nível de instrução (veja [docs/research-frontier.md](docs/research-frontier.md)). Ainda não jogável de ponta a ponta.

**[→ Mapa de progresso ao vivo](https://lesandrossb.github.io/moh-vanguard-recomp/progress.html)**

## Onde o port está (medido)

| estágio | estado | medido |
|---|---|---|
| Boot | ✅ | ELF retail boota pela runtime; SIF/IOP vivos |
| ELF → código recompilado | ✅ | 18.8k+ funções, 0 instruções não decodadas |
| Streaming CDVD (arquivos + setores) | ✅ | 199+ leituras servidas, cdvdfsv/IOP |
| Filme de logo da EA (EAGMLOGO.MPC) | ✅ | playback completo com áudio (decode FFmpeg); 128 uploads @ 639×448 |
| Estabilidade pós-filme | ✅ | main viva, 0 mortes em 5+ runs de horas |
| Clock | ✅ | 59.94 NTSC fiel — vsync no terminal 60,03/s ±0,15% |
| Input (camada MKB) | ✅ | mouse-look + canal de input scriptado pousados ([PR #405](https://github.com/ran-j/PS2Recomp/pull/405)); interação no menu pendente no gate |
| Front-end / menu | ❌ | fronteira de pesquisa — circular idle pós-filme, decodado elo-a-elo ([docs/research-frontier.md](docs/research-frontier.md)) |
| Gameplay @ 60 fps | ⏳ | acorrentado ao gate do front-end |

<sub>✅ = medido nas runs deste repo (cada medição em [docs/bring-up.md](docs/bring-up.md)). ❌ = a fronteira aberta, decodada. ⏳ = gateado na linha acima. "7 de 9" conta linhas da tabela, não jogabilidade: o port para no gate do front-end.</sub>

## As correções (12+, causa raiz até a instrução)

Cada parede da tabela acima foi decodada ao nível de instrução, corrigida com TDD na runtime, e verificada em runs longos. Oito PRs upstream:

| PR | correção |
|---|---|
| [#398](https://github.com/ran-j/PS2Recomp/pull/398) | pilha automática da main do SetupThread ancorada no topo da RDRAM |
| [#399](https://github.com/ran-j/PS2Recomp/pull/399) | analyzer: ponteiros de função referenciados por dados, instruções e tabelas como candidatos a entry |
| [#400](https://github.com/ran-j/PS2Recomp/pull/400) | LQ/SQ do EE forçam alinhamento de 16 bytes, como o hardware |
| [#401](https://github.com/ran-j/PS2Recomp/pull/401) | GS: writes GIF A+D 0x58 PMODE roteados ao arquivo de registradores privilegiado |
| [#402](https://github.com/ran-j/PS2Recomp/pull/402) | DMAC: cadeias do GIF drenam por tags de IRQ até o readback do CHCR no tag END |
| [#403](https://github.com/ran-j/PS2Recomp/pull/403) | IOP: família de RPC do cdvdfsv servida de um serviço HLE residente na ROM |
| [#404](https://github.com/ran-j/PS2Recomp/pull/404) | IOP: sids temporários (bit31) aceitos; o SifSendCmd cid-0 da fila do loader ganha reply de completion |
| [#405](https://github.com/ran-j/PS2Recomp/pull/405) | input: canal scriptado opt-in (PS2X_INPUT_SCRIPT) + camada de mouse-look MKB |

Mais as correções que vivem na branch `vanguard-bringup`: entrega de clock só por wall-deadline com replay limitado, timers de hardware do EE avançando no wall clock, o carve de invocações de callbacks async, o wake do need-data do MPEG, shaping da entrega de timers/alarms. Cada uma com causa raiz → fix → teste em [docs/bring-up.md](docs/bring-up.md).

## Próximo passo — o gate do front-end

A única parede que resta: após o filme da EA, a main dorme num message-wait de kernel saudável e o jogo cai num **circular idle** — o worker segura zero tokens ← zero leituras de CD outstanding ← emissor de demanda worker-only; o armer no-opa em gate ≠ 6; a máquina de estágios tem zero entradas ([docs/research-frontier.md](docs/research-frontier.md), decodado elo a elo). Inventar comportamento do jogo está fora de questão.

**O instrumento nomeado: A/B contra o PCSX2.** O PCSX2 atravessa essa fronteira naturalmente, então o protocolo é: pinar os dois lados no mesmo ponto lógico (pós-filme), capturar a RAM dos dois, e deixar o `abdiff` nomear o primeiro valor divergente — um contrato da runtime servido errado, ou estado anterior do jogo divergindo por timing. Já exonerado por medida (não gaste de novo): a maquinaria de alarm/vsync da runtime, a entrega de invocações, a ponte EE→IOP, o caminho do disco.

## Estrutura do repo

```
├── ps2xRecomp/ ps2xRuntime/ ps2xAnalyzer/ ps2xIOP/ ps2xTest/   # toolchain PS2Recomp + nossos fixes
├── config/recompiler.toml      # config do recompilador (caminhos do usuário)
├── config/mkb.ini              # tuning do mouse-look (sens/deadzone/invert_y)
├── scripts/                    # pipeline setup / build / play (PowerShell)
├── docs/                       # história do bring-up + fronteira de pesquisa
└── AGENTS.md CLAUDE.md         # guia de desenvolvimento agent-driven
```

**Nenhum dado do jogo vive neste repo.** Traga sua própria cópia legal: seu próprio disco dumpado em ISO, e (para comparações A/B no PCSX2) a BIOS do seu próprio console.

## Requisitos

- Windows 10/11, CMake 3.20+, MSVC (C++20), ~10 GB livres
- Seu próprio disco de Medal of Honor: Vanguard (USA) → ISO

## Build & play

```powershell
# 1. builda o toolchain (analyzer + recompiler + runtime)
.\scripts\build.ps1

# 2. do seu ISO: extrai o ELF, analisa, recompila (gera o código do engine)
.\scripts\setup.ps1 -IsoPath "D:\caminho\para\seu.iso"

# 3. builda o runner do jogo (linka o código gerado)
.\scripts\build.ps1 -Runner

# 4. joga (mouse-look ligado, config de config\mkb.ini)
.\scripts\play.ps1
```

Detalhes do pipeline: [docs/bring-up.md](docs/bring-up.md).

## A história do bring-up

Este port existe porque a maquinaria de loader do jogo é uma cadeia de handshakes — callbacks de streaming de CD, round-trips de RPC do IOP, um loop de demanda dirigido por timer, e uma tabela de mensagens que o loader povoa um record por vez. Emular as instruções não bastava: **cada handshake precisava que a runtime servisse o mesmo contrato que o hardware do PS2 servia**, ou a lógica do próprio jogo travava. Cada um desses contratos foi decodado ao nível de instrução, corrigido com testes, e documentado:

- [docs/bring-up.md](docs/bring-up.md) — as 12+ correções, cada uma com causa raiz → fix → teste
- [docs/research-frontier.md](docs/research-frontier.md) — a fronteira aberta, decodada ao nível de instrução

## Contribuir

Veja [AGENTS.md](AGENTS.md) (também o guia do contribuidor humano): onde cada mudança vai, a disciplina de probes, o loop de bring-up, e as minas conhecidas.

## Legal

GPL-3.0 (herdada do PS2Recomp). Sem afiliação com Sony ou EA. **Traga sua cópia legal** — nenhum dado do jogo, ISO ou BIOS vive neste repo, e nenhum deve ser commitado.
