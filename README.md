# HB STUDIO

Central de produção de vídeo ao vivo (IP/PTZ/NDI, Multiview, Switcher, Program Output,
Virtual Camera) — Windows 11 primeiro, macOS depois. Projeto open source, em desenvolvimento.

Arquitetura e decisões aprovadas: [ARCHITECTURE.md](ARCHITECTURE.md).

## Status

| Fase | Estado |
|---|---|
| 0 Arquitetura | Concluída (aprovada) |
| 1 Projeto base | **Parcial**: núcleo portátil pronto e testado (Linux). Pendentes: janela Qt, backend D3D11, build/CI no Windows 11 (ainda não executados) |
| 2+ | Não iniciadas |

### Núcleo da Fase 1 (`src/`)
- `core/event_bus` — eventos `camera.*`, `switcher.*`…; handler com falha não afeta os demais.
- `core/redact` + `core/logger` — logs com redação obrigatória de senhas/tokens/chaves; exportáveis.
- `core/clock` — relógio mestre do Program sem drift; pula ticks atrasados em vez de acumular.
- `video/program_output` — **Program Output headless** (resolução fixa, independente de janela), consumidores isolados, pool de frames.
- `core/hardware_profile` — detecção, tiers LOW/MEDIUM/HIGH/PRO (automático ou escolhido), presets de saída comuns (720p30…4K30), modos de latência, orçamento de fontes com alerta/bloqueio e override de especialista.

## Rodar
Veja [BUILD.md](BUILD.md). Smoke test sem janela: `hb_probe 3 1080p30`.

## Licença
Licença do projeto **ainda não escolhida** (ver LICENSES.md).
