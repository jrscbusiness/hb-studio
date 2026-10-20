# HB STUDIO

Central de produção de vídeo ao vivo (IP/PTZ/NDI, Multiview, Switcher, Program Output,
Virtual Camera) — Windows 11 primeiro, macOS depois. Projeto open source, em desenvolvimento.

Arquitetura e decisões aprovadas: [ARCHITECTURE.md](ARCHITECTURE.md).

## Status (estado real — veja docs/TEST_REPORT.md e docs/BUGS.md)

**Download:** https://github.com/jrscbusiness/hb-studio/releases/tag/latest → `HBStudio-Setup.exe` (duplo clique; o Windows pode mostrar “Windows protegeu o computador”: o instalador ainda não é assinado digitalmente → *Mais informações → Executar assim mesmo*).

| Área | Estado |
|---|---|
| Aplicativo (janela, Preview/Program, CUT/DISSOLVE, 8 entradas **de teste**, PTZ com 10–64 presets configuráveis por câmera) | Implementado; **inicia e renderiza no Windows (CI)**; não testado em Windows 11 real |
| Instalador gráfico + desinstalador | Implementado; renderiza no CI; **instalação real não testada** |
| Câmera virtual → OBS | Implementada e autotestada no CI; **não validada no Windows 11 real / OBS** |
| ONVIF, WS-Discovery, RTSP/RTP/H.264, VISCA/Pelco, áudio (mixer/LUFS) | Módulos prontos e testados com simuladores; **ainda não ligados ao aplicativo**; nenhum validado em câmera real |
| NDI, UVC, SRT, gravação, streaming, multi-monitor, macOS | Não implementados |

## Rodar
Veja [BUILD.md](BUILD.md). Smoke test sem janela: `hb_probe 3 1080p30`.

## Licença
Licença do projeto **ainda não escolhida** (ver LICENSES.md).
