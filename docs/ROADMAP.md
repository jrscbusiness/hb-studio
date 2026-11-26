# Roteiro (roadmap)

Este documento é honesto sobre o estado do projeto e lista para onde ele pode ir. Não é promessa de
prazo: o HB Studio é mantido por voluntários e **qualquer item pode ser pego por quem quiser
contribuir** (veja [CONTRIBUTING.md](CONTRIBUTING.md); comente na issue antes de começar).

Legenda de status: **validado** (comprovado de verdade), **testado em CI** (teste automatizado, sem
hardware real), **pendente** (falta fazer/ligar), **planejado** (decidido, ainda não iniciado),
**ideia** (sugestão, sem compromisso).

## Estado atual: validado × pendente

"Validado" só vale para o que foi visto funcionando numa máquina real. Hoje isso é muito pouco: o
projeto foi construído e testado em Linux e no runner Windows do GitHub (Windows Server 2022, sem
câmera nem GPU). Os detalhes de cada módulo estão nos `docs/*.md` ("Estado de validação").

| Item | Estado |
|---|---|
| Mesa de corte, motor de vídeo, render | testado em CI (o app abre e renderiza no runner; 30 fps medidos) |
| Câmera virtual (Frame Server) | testado em CI (autoteste da DLL no próprio processo); **pendente**: ver no App Câmera/OBS/Zoom em Windows 11 real |
| Instalador | renderiza no CI; **pendente**: instalar/desinstalar em PC real |
| Webcams/placas de captura (DirectShow) | **pendente**: validar com câmeras reais de várias marcas |
| NDI entrada/saída | testado em CI (backend simulado); **pendente**: NDI Runtime e fontes reais |
| Áudio, efeitos, mixer | testado em CI; **pendente**: interfaces de áudio e fones reais |
| VST3 | testado em CI; **pendente**: plugins comerciais reais |
| Gravação MP4 e multipista (ISO) | testado em CI (escritores simulados); **pendente**: gravações longas em PC real, divisão de arquivos, queda de energia |
| Transmissão RTMP | protocolo testado em loopback; **pendente**: YouTube/Twitch reais, encoders NVENC/QuickSync/AMF, transmissões longas |
| Legendas (whisper.cpp) | lógica testada; **pendente**: precisão e custo de CPU em PCs reais |
| Chroma por IA (ONNX) | lógica testada; **pendente**: modelo real, custo de CPU, empacotamento da DLL no instalador |
| Chat, backup em nuvem, IA | testados com transporte simulado; **pendente**: serviços reais (YouTube/Facebook/Twitch, S3/Drive, OpenRouter) |
| Controle remoto | testado em CI (sockets reais em loopback); **pendente**: celular/tablet, roteador e firewall reais |
| Plugins e Marketplace | testado em CI (validação do catálogo); **pendente**: plugins de terceiros reais |
| **PTZ real (ONVIF/VISCA/Pelco)** | protocolos testados com transporte simulado; **pendente**: ligar à aba CÂMERAS e validar em câmera real |
| Assinatura de código do instalador | **pendente**: depende de um certificado (veja [BUILD.md](../BUILD.md)) |

**Como ajudar mais rápido:** testar numa máquina Windows 11 de verdade e abrir uma issue com o
registro (CONFIG → Registro → COPIAR REGISTRO) diz mais ao projeto do que qualquer linha de código.

## Próximas ideias

| Ideia | Descrição | Status |
|---|---|---|
| Ligar PTZ real | Câmeras ONVIF/VISCA/Pelco aparecem como fontes na aba CÂMERAS e respondem ao joystick/presets | planejado |
| RTMPS e SRT | Transmissão com TLS (RTMPS) e SRT, além do RTMP puro de hoje | planejado |
| Câmera do celular via WebRTC | Usar a câmera do celular/tablet como fonte, abrindo uma página no navegador do aparelho | ideia |
| API para Stream Deck / Companion | Controle por botões físicos (Bitfocus Companion, Elgato Stream Deck) via uma API local autenticada | ideia |
| Fonte "navegador" | Página web (alertas, widgets, placar online) como fonte/camada de vídeo | ideia |
| Letras e versículos | Camada de texto para letras de música e versículos, com bancos de texto e atalhos | ideia |
| Player de mídia e slides | Reproduzir vídeos, imagens e apresentações (PDF/imagens) como fonte | ideia |
| Replay | Gravar um buffer circular e reproduzir o último lance | ideia |
| Redução de ruído (RNNoise) | Supressão de ruído de voz local no mixer de áudio | ideia |
| Diretor automático por IA | Sugerir ou fazer cortes automáticos conforme quem fala/enquadramento | ideia |
| Pós-live com IA | Resumo, capítulos e cortes sugeridos a partir da gravação e das legendas | ideia |
| Legendas traduzidas | Tradução ao vivo das legendas para outros idiomas | ideia |
| Transições animadas | Transições além de CUT/DISSOLVE (wipe, slide, stinger) | ideia |
| Captura de tela/janela | Compartilhar tela ou uma janela como fonte | ideia |
| Rastreamento | Seguir pessoa automaticamente com câmera PTZ ou com recorte digital | ideia |
| macOS | Versão para macOS (hoje só Windows) | planejado |

## Participe

- Gostou de uma ideia? Abra (ou comente) uma [issue](https://github.com/jrscbusiness/hb-studio/issues/new/choose)
  de "Sugerir funcionalidade" descrevendo o seu caso de uso.
- Quer implementar? Leia o [CONTRIBUTING.md](CONTRIBUTING.md) e combine na issue antes.
- **GitHub Discussions** (opcional): se o repositório ativar a aba Discussions, ela é o lugar para
  conversas abertas, votar em ideias e mostrar como você usa o HB Studio; até lá, use as issues.
