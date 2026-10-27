# Medal of Honor: Vanguard — Recompilação Estática PS2

Port nativo de PC de **Medal of Honor: Vanguard (PS2, NTSC-U SLUS-215.97)** via recompilação estática com [PS2Recomp](https://github.com/ran-j/PS2Recomp). Controles de FPS com mouse + teclado, clock NTSC 59.94 fiel, e o diário completo da pesquisa de bring-up.

> ⚠️ **Status: em progresso.** O jogo boota, roda o filme de logo da EA completo com áudio, e roda estável pós-filme (a main viva além de todas as paredes anteriores — o primeiro avanço da máquina de estágios pousou em 26/10/2026). O front-end/menu **ainda não abre**: o jogo entra num circular idle pós-filme decodado ao nível de instrução (veja [docs/research-frontier.md](docs/research-frontier.md)). Ainda não jogável de ponta a ponta.

## O que funciona (medido)

| estágio | estado |
|---|---|
| Boot → ELF → código recompilado | ✅ 18.8k+ funções, 0 instruções não decodadas |
| Streaming CDVD (arquivos + setores) | ✅ 199+ leituras, cdvdfsv/IOP servido |
| Filme de logo da EA (EAGMLOGO.MPC) | ✅ playback completo com áudio (decode FFmpeg) |
| Estabilidade pós-filme | ✅ main viva, 0 mortes em 5+ runs de horas |
| Clock | ✅ 59.94 NTSC fiel (terminal 60.03/s ±0,15%) |
| Front-end / menu | ❌ fronteira de pesquisa (circular idle pós-filme, decodado) |

**12+ correções de runtime upstream:** [PRs #398–#405](https://github.com/ran-j/PS2Recomp/pulls?q=is%3Apr+author%3ALeandroSSB) — âncora de pilha do SetupThread, ponte EE→IOP + endTimeslice do IOP, staging do SetupHeap, alinhamento LQ/SQ de 16 bytes, tiers de descoberta do analyzer, shaping da entrega de timers/alarms, drain do DMA do GIF até o END, callback need-data do MPEG, PMODE A+D, e mais. Cada uma: causa raiz até a instrução, TDD, revisada.

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
