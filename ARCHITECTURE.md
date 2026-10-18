# HB STUDIO — FASE 0: Arquitetura (proposta para aprovação)

Status: **PROPOSTA — nenhuma linha de código foi escrita.**
Nome oficial do produto: **HB STUDIO**.

## DECISÕES APROVADAS (registro)

| # | Decisão | Consequência no projeto |
|---|---|---|
| 1 | C++20 + Qt 6 + FFmpeg (LGPL) + engine separado — **aprovado, com tudo configurável no sistema** | Nenhum parâmetro técnico (codec, buffers, transporte, perfil) é fixo no código; todos têm padrão e podem ser alterados em Settings/arquivo de projeto. |
| 2 | PoC antecipada de Virtual Camera + NDI Output como plano B — **aprovado, configurável** | Saída para OBS escolhida em Settings (Virtual Camera / NDI Output). |
| 3 | Sem acesso físico à FOMAKO NK20A | Documentação e protocolo serão pesquisados na internet; até lá desenvolvemos contra **simuladores** (mediamtx + simulador ONVIF próprio). Nada de PTZ será declarado "validado" sem câmera real. |
| 4 | Hardware: oferecer os **padrões mais usados** para escolha | Presets LOW/MEDIUM/HIGH/PRO + presets de saída comuns (720p30, 1080p30, 1080p60, 4K30) + modos de latência; detecção automática sugere, o usuário pode sobrescrever. |
| 5 | **Windows 11 apenas** | `MFCreateVirtualCamera` é o provider primário; DirectShow vira provider opcional/secundário (compatibilidade com OBS a validar na PoC). |
| 6 | Uso interno, distribuição **gratuita e open source** | Escolher licença do projeto compatível com Qt (LGPL), FFmpeg (LGPL) e libsrt (MPL). NDI SDK **não** é redistribuído no repositório; o usuário instala o NDI Runtime. Ver `LICENSES.md`. Licença do HB Studio ainda **pendente de escolha** (sugestão: Apache-2.0 ou MPL-2.0). |
| 7 | Apple Developer Program existe, mas **não é necessário agora** | macOS fica após o MVP Windows. |
| 8 | Isolamento híbrido (threads no MVP; processos separados antes do 1.0) | `IInputWorker` desenhado para os dois modos. |

> Honestidade técnica: itens marcados **[VALIDAR]** são hipóteses que dependem de prova de conceito (PoC) ou de hardware real (por exemplo, a FOMAKO NK20A) e **não** devem ser tratados como fatos. Não tenho acesso ao manual/firmware da NK20A; o que afirmo sobre ela é inferência baseada em padrões do mercado.

---

## 1. Arquitetura recomendada

**UI fina + Engine nativo de vídeo + Providers plugáveis.** A UI nunca toca em protocolo, decoder ou frame de vídeo bruto; ela envia comandos e recebe eventos e texturas.

```
┌───────────────────────── UI (Qt 6 / QML) ─────────────────────────┐
│ Switcher · Multiview · PTZ · Mixer · Cenas · Settings · Janelas    │
└───────────────▲───────────────────────────────┬───────────────────┘
        eventos / estado                    comandos
┌───────────────┴───────────────────────────────▼───────────────────┐
│ Application Layer  (Event Bus, Projeto, Perfis, Atalhos, API)     │
├───────────────────────────────────────────────────────────────────┤
│ Engines:  Camera · PTZ · Switcher · Audio · Compositor · Output   │
│           Resource Manager · Failsafe · Logging                   │
├───────────────────────────────────────────────────────────────────┤
│ Media Abstraction Layer (Frame GPU, FramePool, Clock, Decoders)   │
├───────────────────────────────────────────────────────────────────┤
│ Providers (plugins): RTSP · NDI · SRT · UVC · HTTP · ONVIF-PTZ ·  │
│   Fomako-PTZ · VirtualCamera(Win/Mac) · Recorder · Streamer       │
├───────────────────────────────────────────────────────────────────┤
│ SO / Hardware: D3D11 · Metal · MF · AVFoundation · NVDEC/QSV/AMF  │
│                VideoToolbox · Rede · Keychain/DPAPI               │
└───────────────────────────────────────────────────────────────────┘
```

Princípios inegociáveis:

1. **Program Output é um objeto do engine** (textura offscreen em resolução fixa, ex. 1920×1080@30/60), independente de qualquer janela.
2. **Cada input é isolado** (thread própria, fila limitada, exceções capturadas, watchdog). Falha de input → estado `ERROR/RECONNECTING`, nunca propaga.
3. **Clock mestre** do Program (VSync-independente) puxa o último frame válido de cada fonte; o Program nunca espera uma fonte.
4. **Preview ≠ Program ≠ Background ≠ Inactive** como classes de recurso (seção 17).

---

## 2. Stack tecnológico recomendado

| Camada | Escolha | Observação |
|---|---|---|
| Linguagem do engine | **C++20** | Acesso direto a D3D11/Metal/FFmpeg/NDI/MF/CMIO. (Rust é alternativa viável; C++ reduz atrito com SDKs C/C++/ObjC e com o Qt.) |
| UI | **Qt 6 (QML/Qt Quick) + QRhi** | LGPL v3 com **linkagem dinâmica**. Multi-janela/multi-monitor nativo, QRhi sobre D3D11 e Metal. |
| Mídia (rede/decode/mux) | **FFmpeg (libav\*)**, build **LGPL**, dinâmico | RTSP, SRT (via libsrt), RTMP(S), MP4, decode HW (D3D11VA/NVDEC/QSV/VideoToolbox). |
| Encode | **Encoders de hardware** (NVENC, QSV, AMF, VideoToolbox) via FFmpeg | Evita x264 (GPL). Fallback de software só se a licença for resolvida (ver seção 22). |
| GPU compositor | Interface `IGpuBackend` com **D3D11 (Win)** e **Metal (macOS)** | Shaders HLSL/MSL. Vulkan fica fora do MVP. |
| NDI | NDI SDK, **carregado dinamicamente** (`LoadLibrary`/`dlopen`) | App funciona sem NDI instalado. |
| ONVIF | **Implementação própria** (SOAP/XML mínimo + WS-Discovery UDP) | Evita gSOAP (GPL/comercial). XML: pugixml (MIT). HTTP: Qt Network ou libcurl. |
| UVC | Media Foundation (Win) / AVFoundation (macOS) | |
| Áudio | WASAPI (Win) / CoreAudio (macOS) + mixer próprio (float32) | miniaudio (MIT/domínio público) como camada de dispositivo. |
| Segredos | Windows Credential Manager/DPAPI · macOS Keychain | |
| Build | CMake + vcpkg/Conan | |
| Testes | GoogleTest + Catch2 opcional; câmeras RTSP/ONVIF simuladas (mediamtx, FFmpeg) | |
| Logs | spdlog (MIT) com filtro de segredos | |
| Instalador | WiX/MSIX (Win), pkg/notarização (macOS) | |

### Decisão técnica: framework de UI/aplicação

**PROBLEMA:** escolher a base que melhor sirva a vídeo ao vivo, GPU, multi-janela, câmera virtual, NDI, FFmpeg.

**OPÇÕES (resumo):**

| Opção | Vídeo/GPU | Multi-window | Acesso nativo (NDI/FFmpeg/CMIO/MF) | Estabilidade p/ live | Veredito |
|---|---|---|---|---|---|
| **C++ + Qt 6** | Excelente (QRhi, texturas nativas) | Excelente | Direto | Alta (OBS usa Qt) | **Escolhido** |
| Electron | Fraco para N streams; frames cruzam processos/GPU com custo | Bom | Via addon nativo | Média; RAM alta | Não |
| Tauri | WebView ≠ pipeline de vídeo GPU; texturas compartilhadas entre webview e engine são difíceis; WebView difere por SO | Razoável | Via Rust | Boa p/ UI, ruim p/ vídeo | Não |
| Flutter | Texturas externas possíveis, multi-janela ainda imaturo no desktop | Fraco | FFI | Média | Não |
| .NET MAUI | Sem macOS desktop nativo (Mac Catalyst), sem Windows-first sólido | Fraco | P/Invoke | Média | Não |
| React + backend nativo | Mesmo problema do Electron/Tauri na ponte de frames | Bom | Sim | Média | Só se UI web for requisito |
| Qt Widgets puro | Bom | Bom | Direto | Alta | Possível, mas QML dá UI mais moderna |

**DECISÃO:** C++20 + Qt 6 Quick, com engine desacoplado da UI (biblioteca `hb_engine` sem dependência do Qt, exceto onde conveniente) para permitir UI web remota depois (Web Control Panel via HTTP API).

**MOTIVO:** é o único conjunto em que renderização de vídeo GPU, múltiplas janelas/monitores e SDKs nativos convivem sem ponte de processos. O OBS Studio comprova a viabilidade da combinação.

**TRADE-OFFS:** C++ é mais lento para desenvolver e exige disciplina de memória/threads; QML tem curva própria; LGPL exige linkagem dinâmica e atribuição (ou licença comercial Qt).

**COMO TESTAR:** PoC de 1 semana: 8 janelas Qt exibindo 8 texturas D3D11/Metal a 30 fps com CPU < 15% (Fase 1).

---

## 3. Diagrama completo da arquitetura

```
Câmeras (FOMAKO/RTSP/ONVIF/NDI/NDI HX/SRT/UVC/HTTP)
        │
┌───────▼────────┐   eventos   ┌──────────────────────────────┐
│ Discovery      │────────────▶│ Event Bus (camera.*, ptz.*,  │
│ (WS-Disc, mDNS,│             │ switcher.*, recording.* ...) │
│ NDI finder,UVC)│             └───────▲───────────┬──────────┘
└───────┬────────┘                     │           │
┌───────▼────────┐  1 por câmera ┌─────┴─────┐ ┌───▼──────────┐
│ Camera Engine  │──────────────▶│ Input     │ │ PTZ Engine   │
│ (perfis, estado│               │ Worker    │ │ (ONVIF/Fomako│
│  reconexão)    │               │ net→decode│ │  providers)  │
└────────────────┘               └─────┬─────┘ └──────────────┘
                                       │ Frames GPU (FramePool, ref-count)
                              ┌────────▼─────────┐
                              │ Normalização:    │
                              │ scale/colorspace │
                              │ /fps → formato   │
                              │ interno          │
                              └───┬─────────┬────┘
                    Preview-res   │         │ Full-res (só PGM/PVW)
                     ┌────────────▼──┐   ┌──▼────────────────────┐
                     │ Multiview     │   │ Switcher (PVW/PGM,    │
                     │ renderer      │   │ transições)           │
                     └───────────────┘   └──┬────────────────────┘
 Áudio: inputs → Audio Mixer ───────┐       │
                                    │   ┌───▼──────────────┐
 Graphics (logo, lower 3rd) ────────┼──▶│ Compositor (GPU) │
 Cenas ─────────────────────────────┘   └───┬──────────────┘
                                            │
                                 ┌──────────▼───────────┐
                                 │ PROGRAM OUTPUT (tex) │  + PROGRAM AUDIO
                                 └─┬──────┬───────┬─────┘
                       VirtualCam  │ Rec  │Stream │ (NDI/SRT out futuros)
                           │
                        OBS / Zoom / Teams / Meet
```

---

## 4. Fluxo de vídeo

1. **Ingest** (thread de rede) → pacotes codificados em fila limitada (descarta o mais antigo em LOW LATENCY).
2. **Decode** (HW preferencial) → `GpuFrame` (NV12/P010 em textura nativa) retirado de um `FramePool` (sem alocação por frame).
3. **Sincronização:** cada fonte guarda *apenas o frame mais recente* (mailbox). O compositor lê o mais recente a cada tick do clock mestre. Sem genlock entre câmeras IP; alinhamento é "melhor esforço" (limitação documentada).
4. **Normalização:** conversão YUV→RGB/BGRA no shader, escala (bilinear/Lanczos para PGM, bilinear para preview), correção de fps por repetição/descarte.
5. **Dois ramos por fonte:** *preview-res* (Multiview) e *full-res* (somente se PVW/PGM/AUX).
6. **Switcher** gera a composição `PGM` e `PVW` como texturas (transições em shader: mix, fade via preto, wipe futuro).
7. **Compositor** aplica camadas (background, logo, lower third, texto) sobre o PGM → **Program Output texture**.
8. **Distribuição:** a mesma textura é consumida por Virtual Camera, Recorder (encode HW), Streamer, janelas de monitoramento. Readback GPU→CPU **somente** onde inevitável (Virtual Camera no Windows, possivelmente; ver seção 13).

Clock: tick fixo (ex. 30/60 Hz) por `std::chrono::steady_clock` + espera de alta resolução; métrica `render_time` e `dropped_frames` por tick.

---

## 5. Fluxo de áudio

- Cada input com áudio → decode → resample (48 kHz, float32) → **ring buffer por canal**.
- Mixer: gain, fader, mute, solo, medidores (peak/RMS, depois LUFS), PAN futuro → **Program Audio bus**.
- **Sincronia A/V:** timestamp de apresentação (PTS) por fonte; o áudio é o relógio de referência do Program Audio; vídeo do Program usa compensação de atraso configurável por fonte ("lip-sync offset"). Drift tratado por resample adaptativo leve.
- Program Audio alimenta Recorder, Streamer e (opcional) saída de áudio do SO.
- **Limitação:** câmera virtual de **vídeo** não carrega áudio. Para o OBS receber o áudio do HB Studio é preciso um dispositivo de áudio virtual (driver — complexo e fora do MVP) ou NDI Output (carrega áudio) ou roteamento via cabo virtual de terceiros. No MVP o áudio do OBS vem de dispositivos do próprio OBS. Isto precisa ser comunicado ao usuário.

---

## 6. Fluxo PTZ

```
UI (joystick/botões/atalhos) → PTZ Engine → PTZController (genérico)
                                              ├─ ONVIFPTZProvider (ContinuousMove, Stop, GotoPreset, SetPreset, Imaging)
                                              └─ FomakoPTZProvider (protocolo nativo, se ONVIF insuficiente) [VALIDAR]
```

- Canal de controle **totalmente separado** do vídeo (conexão/thread própria, timeout curto). Falha de PTZ → evento `ptz.error`, vídeo intacto.
- Seleção automática de provider: `capabilities` ONVIF (PTZ service presente?) → senão provider do fabricante → senão PTZ desabilitado.
- Velocidade: ONVIF usa velocidade normalizada [-1..1]; mapeada a `setPanSpeed/TiltSpeed/ZoomSpeed`.
- Presets: armazenados **no perfil da câmera no HB Studio** (nome e id) e, quando suportado, também na câmera (sincronização opcional).
- Foco/íris: via ONVIF Imaging (`Move`/`SetImagingSettings`); suporte depende da câmera. Métodos não suportados retornam `NotSupported` e a UI desabilita o controle.
- Rate-limit de comandos (ex. ≤ 20 Hz) e "stop" garantido ao soltar o controle (e watchdog: se a UI sumir, enviar Stop).
- **[VALIDAR NK20A]:** assumo ONVIF Profile S/T com PTZ, mais VISCA over IP/serial ou CGI HTTP proprietário. Necessário: captura de rede (Wireshark) e manual do fabricante.

---

## 7. Discovery Engine

Execução em paralelo, cada descoberta isolada com timeout:

| Fonte | Mecanismo |
|---|---|
| ONVIF | WS-Discovery: UDP multicast `239.255.255.250:3702` (Probe `NetworkVideoTransmitter`), por interface de rede |
| NDI | NDI Find API (mDNS interno do SDK), grupos configuráveis |
| UVC | Enumeração Media Foundation / AVFoundation, com hot-plug |
| RTSP/SRT/HTTP | Sem descoberta padrão → lista manual + varredura opcional de porta 554 em sub-rede (opt-in, com aviso) |
| Salvas | Perfis do projeto reavaliados (ping + probe) |

Resultado normalizado em `DiscoveredDevice { id, nome, fabricante, ip, protocolos[], ptz, resolução, fps, estado }`. Observações: multicast pode ser bloqueado por Wi-Fi/VLAN/firewall → mensagem de diagnóstico + adicionar por IP manual (probe ONVIF direto). Descoberta ONVIF exige login para `GetProfiles` na maioria das câmeras: o item aparece como "requer credenciais".

---

## 8. Estratégia NDI

- `NDIInputProvider`: `discoverSources()`, `connect()`, `receiveFrame()` (vídeo/áudio/metadata), `getSourceInfo()`.
- Carga dinâmica da biblioteca NDI; **verificação de dependência** na inicialização: ausente → recurso NDI desabilitado com instrução de instalação do NDI Runtime; app continua.
- NDI HX (H.264/H.265) decodifica no SDK ou por callback comprimido; preferir decode HW quando o SDK permitir frames comprimidos **[VALIDAR conforme versão do SDK]**.
- Selecionar `NDIlib_recv_bandwidth_lowest` para preview e `highest` para PGM/PVW (troca dinâmica de largura de banda — recurso do SDK que ajuda nos 16+ fontes).
- **Licença:** NDI SDK é gratuito mas com termos próprios (uso comercial permitido mediante aceite, atribuição obrigatória "NDI® is a registered trademark of Vizrt Group", regras sobre redistribuição do runtime). Verificar texto atual antes de empacotar (Seção 22).
- NDI Output (futuro) também resolve OBS (via plugin NDI) + áudio juntos.

---

## 9. Estratégia ONVIF

Implementação própria, mínima, testável:

- WS-Discovery · Device Management (`GetDeviceInformation`, `GetCapabilities`, `GetServices`, `GetSystemDateAndTime`) · Media (`GetProfiles`, `GetStreamUri`, `GetSnapshotUri`) · PTZ (`ContinuousMove`, `Stop`, `GotoPreset`, `SetPreset`, `RemovePreset`, `GetPresets`, `GetStatus`) · Imaging.
- Autenticação **WS-Security UsernameToken Digest** (com correção de offset de relógio via `GetSystemDateAndTime`, causa clássica de falha) e HTTP Digest como fallback.
- Escolha de perfis: *main* para PGM/PVW, *sub* para Multiview.
- Testes contra simulador ONVIF e, principalmente, contra a câmera real (fabricantes divergem do padrão).

---

## 10. Estratégia RTSP

- FFmpeg (`avformat`) com opções de baixa latência: `rtsp_transport` (UDP ou TCP configurável; TCP default por robustez), `fflags=nobuffer`, `flags=low_delay`, `probesize`/`analyzeduration` reduzidos, `stimeout`/`timeout`, `max_delay`.
- Alternativa futura: stack RTSP/RTP própria (live555/rtsp-simple-like) se FFmpeg não atingir latência-alvo **[VALIDAR com benchmark]**.
- Modos: LOW LATENCY (buffer mínimo, descarta frames antigos, reconexão agressiva) · BALANCED · STABLE (jitter buffer maior).
- Health monitor por stream: último frame, bitrate, taxa de perda, backoff exponencial na reconexão (1 s → 2 → 4 … teto 15 s, com jitter).
- Credenciais montadas em memória; URL **mascarada** em qualquer log.
- Pedir H.264 (compatível com HW decode universal); H.265 quando decoder HW disponível.

---

## 11. Estratégia SRT

- Fora do MVP, mas com `SRTInput`/`SRTOutput` já na interface. Implementação via **libsrt** (MPL 2.0) integrada ao FFmpeg ou direta.
- Modos: Listener, Caller, Rendezvous; parâmetros: latency, passphrase/AES (128/192/256), maxbw/bitrate, streamid.
- Estatísticas SRT (RTT, perdas, retransmissões) alimentam o Network Monitor.

---

## 12. Decodificação / Encode / GPU (item 15)

- **Windows:** decode D3D11VA (genérico), NVDEC, Quick Sync, AMD (via D3D11VA/AMF); encode NVENC/QSV/AMF. Texturas D3D11 compartilhadas entre decoder, compositor e encoder (zero-copy quando possível; `ID3D11Texture2D` + `KeyedMutex`/fences).
- **macOS:** VideoToolbox (decode/encode), Metal (compositor), `CVPixelBuffer`/`IOSurface` zero-copy.
- Fallback para software com aviso de capacidade reduzida.
- Detecção de hardware (seção "Resource Manager"): CPU, RAM, GPU/VRAM, codecs HW suportados, NICs → perfil **LOW / MEDIUM / HIGH / PRO** → define número máximo recomendado de fontes, resolução de preview, FPS de preview, tamanho de buffers.

---

## 13. Estratégia Virtual Camera (componente de maior risco)

**PROBLEMA:** expor o Program Output como webcam do SO.

**Não existe forma multiplataforma única.**

### Windows
| Opção | Prós | Contras |
|---|---|---|
| **Media Foundation Virtual Camera** (`MFCreateVirtualCamera`) | API oficial, sem driver de kernel, apps modernos (Teams/Zoom/Meet) | Windows 11 apenas (build 22000+); mídia-source COM roda no processo **Frame Server** → exige IPC (memória compartilhada/handles) do HB Studio para esse processo; compatibilidade com apps DirectShow como OBS **[VALIDAR em PoC]** |
| **Filtro DirectShow** (como o do OBS) | Funciona em Win 10 e 11, amplo suporte de apps (inclui OBS) | Tecnologia legada; instalação com registro COM (admin); app 64-bit e 32-bit; o código do OBS é GPL (**não copiar**; escrever o nosso) |
| Driver de kernel | — | Fora de cogitação (assinatura WHQL, risco) |

**DECISÃO PROPOSTA:** implementar **dois providers atrás da mesma interface**: `WindowsDirectShowVCam` (cobertura Win 10/11, prioridade p/ OBS) e `WindowsMFVirtualCam` (Win 11, apps modernos). A PoC da Fase 10 decide qual é o primário. Transporte de frames: memória compartilhada (BGRA/NV12 + header + eventos de sincronização), produtor = HB Studio; consumidor = DLL do filtro/MF source. Readback GPU→CPU do Program é necessário (custo ~ 1080p30 BGRA ≈ 250 MB/s, aceitável; em 4K, custo maior).

### macOS
- Único caminho moderno: **CoreMediaIO Camera Extension** (system extension, macOS 12.3+). Plugins DAL antigos estão descontinuados.
- Exige **Apple Developer Program**, assinatura, *notarization*, *entitlements* e aprovação do usuário em Ajustes do Sistema; entrega de frames do app para a extensão via **sink stream** da própria extensão ou XPC/IOSurface **[VALIDAR]**.
- Apps com *hardened runtime* podem exigir `com.apple.security.cs.disable-library-validation` para carregar a câmera — incluindo o próprio OBS (o OBS recente já lida com isso; **verificar na PoC**).

### Alternativas que devem existir como plano B
- **NDI Output** (OBS recebe via plugin obs-ndi, com áudio) — implementação simples, **recomendo antecipar** antes da câmera virtual para destravar o OBS rápido.
- Captura de janela/tela cheia do Program Output (OBS Display Capture) — "gambiarra" funcional documentada como fallback.

**COMO TESTAR (critério da Fase 10):** OBS 30+, Zoom, Teams e Chrome (Meet) listam "HB Studio Virtual Camera", exibem o PGM por ≥ 30 min a 1080p30 sem queda e com latência adicional < 100 ms (medida com relógio na tela).

---

## 14. Integração com OBS

- OBS adiciona "Dispositivo de Captura de Vídeo" → **HB Studio Virtual Camera** (resolução/fps fixados em 1920×1080 @ 30 por padrão; configurável).
- OBS não recebe câmeras individuais; recebe só o PGM.
- Opcional futuro: NDI Output (vídeo + áudio), `obs-websocket` para o HB Studio iniciar/parar gravação do OBS (opcional).
- Documentação `OBS.md` com passo a passo e limitações (áudio, formato de pixel, FPS).

---

## 15. Multi-window

- Qt 6: `QWindow` por módulo (Switcher, Multiview, PTZ, Mixer, PGM fullscreen, PVW fullscreen). Estado do app vive no **Application Layer**, não nas janelas — fechar uma janela nunca para o engine.
- Vídeo exibido em janelas secundárias vem das **mesmas texturas** (um compositor, várias vistas); janelas fullscreen usam swapchain própria e escala GPU.
- Layout persistente por monitor (`QScreen`) com reposicionamento seguro quando um monitor some.
- Dockable panels (QML/ docking próprio ou lib como KDDockWidgets — licença a verificar) + Detached windows.

---

## 16. Estratégia para 10–16 câmeras (itens 17 e M/N)

Princípio: **nunca decodificar mais do que é exibido, e nunca em resolução maior do que o necessário.**

| Classe | Decode | Resolução | FPS | Prioridade |
|---|---|---|---|---|
| Program | HW, sempre | Full (main stream) | Nativo | Máxima |
| Preview | HW | Full (main stream) | Nativo | Alta |
| Multiview (background) | **Sub-stream** da câmera (ONVIF profile 2 / NDI lowest bandwidth) | 640×360 ou menor | 10–15 fps, adaptativo | Média |
| Inactive | Desconectado ou keep-alive | — | — | Baixa |

Táticas: dois streams por câmera (main + sub); troca de qualidade ao promover para PVW/PGM (pré-conectar o main ao selecionar a câmera como PVW para evitar tela preta — "warm-up" de ~0,5–1 s); HW decode obrigatório acima de N fontes; pool de frames fixo; escala em GPU; limitador de fontes baseado no perfil de hardware; alerta "PERFORMANCE WARNING" com sugestões.

**Números (ESTIMATIVAS a validar por benchmark, não promessas):**
- 10 × 1080p30 H.264 → HW decode cabe em GPU integrada moderna; ~ 1 Gbps de rede é suficiente se 4–8 Mbps/câmera.
- 16 × 1080p30 sub-stream no Multiview + 2 main (PVW/PGM) → viável em perfil HIGH/PRO.
- 16 × 4K main simultâneos → **não** é objetivo realista em hardware comum; o sistema deve impedir/avisar.
- Rede: 16 × 8 Mbps ≈ 128 Mbps; NDI full ≈ 100–150 Mbps **por fonte** (exige 1 GbE dedicada/10 GbE) → NDI HX ou sub-stream.

Limite imposto pelo `Resource Manager`: bloqueio suave (aviso) e rígido (recusa de conectar além do orçamento de decode/VRAM/rede).

---

## 17. Estratégia Windows

- Prioridade do MVP. D3D11 + Media Foundation + WASAPI; DirectShow filter e/ou MF Virtual Camera; Credential Manager; instalador MSI/MSIX assinado (certificado de code signing — custo recorrente).
- Windows 10 22H2 e 11 suportados; **MF Virtual Camera somente Win 11**.
- Firewall: regra para multicast/UDP/RTSP/NDI no instalador (com consentimento).
- MMCSS/prioridade de thread (`AvSetMmThreadCharacteristics`) e `timeBeginPeriod` para o clock.

## 18. Estratégia macOS

- Metal + VideoToolbox + AVFoundation + CoreAudio; Keychain; binário universal (arm64 + x86_64).
- Camera Extension (system extension) → Apple Developer Program (US$ 99/ano), notarização, entitlements.
- Permissões TCC: câmera, microfone, **rede local** (macOS 15 exige permissão para multicast/Bonjour — impacta ONVIF/NDI).
- Sem NDI Runtime nativo embutido: o usuário instala o NDI Runtime ou o app o distribui conforme licença.

---

## 19. Estrutura de diretórios inicial

```
hb-studio/
├─ CMakeLists.txt  ·  vcpkg.json  ·  LICENSES.md  ·  README.md
├─ docs/  (ARCHITECTURE, DEVELOPMENT, BUILD, WINDOWS, MACOS, CAMERAS, PTZ, NDI, ONVIF, RTSP, OBS, VIRTUAL_CAMERA, TROUBLESHOOTING)
├─ src/
│  ├─ core/            event bus, tipos, erros, threading, config
│  ├─ video/           Frame, FramePool, decoders, scalers, clock
│  ├─ gpu/             IGpuBackend (d3d11/, metal/), shaders/
│  ├─ audio/           dispositivos, resample, mixer
│  ├─ cameras/         CameraInput (interface), perfis, estado, reconexão
│  ├─ discovery/       ws-discovery, ndi-finder, uvc-enum, manual
│  ├─ rtsp/  ndi/  srt/  usb/   (providers)
│  ├─ onvif/           cliente SOAP, auth, serviços
│  ├─ ptz/             PTZController, providers
│  ├─ switcher/  mixer/  multiview/  compositor/  graphics/
│  ├─ recording/  streaming/  virtual-camera/ (win/, macos/)
│  ├─ windows/ (código específico Win)  macos/ (específico Mac)
│  ├─ ui/              QML, view models
│  ├─ settings/  logging/  plugins/  api/
├─ third_party/        apenas metadados; libs via gerenciador de pacotes
├─ tools/              simulador RTSP/ONVIF, benchmarks, scripts de PoC
└─ tests/  (unit/, integration/, perf/)
```
Obs.: `windows/` e `macos/` contêm apenas adaptações de plataforma; lógica de negócio fica nos módulos neutros.

---

## 20. Dependências

| Dependência | Uso | Obrigatória? |
|---|---|---|
| Qt 6 (Core, Gui, Quick, Network, Multimedia opcional) | UI | Sim |
| FFmpeg (LGPL, dinâmico) + libsrt | RTSP/SRT/RTMP/MP4/decode/encode | Sim |
| NDI SDK/Runtime | NDI | Opcional (detecção) |
| pugixml, spdlog, GoogleTest | XML, log, testes | Sim |
| miniaudio ou APIs nativas | Áudio | Sim |
| Drivers de GPU atualizados | HW decode/encode | Recomendado |
| Visual Studio 2022 / Xcode 15+, CMake, vcpkg | Build | Dev |
| Certificado code signing (Win) / Apple Developer ID | Distribuição | Para release |
| mediamtx / simuladores | Testes | Dev |

## 21. Segurança (item 24)

- Senhas **apenas** em Credential Manager/DPAPI (Win) e Keychain (macOS); arquivo de projeto guarda *referência* (`credentialRef`), nunca a senha. Exportar projeto → credenciais **não** vão junto.
- Filtro central de logs: redação de `rtsp://user:pass@…`, cabeçalhos `Authorization`, `wsse:Password`, tokens, passphrases SRT, stream keys RTMP.
- ONVIF/RTSP trafegam em claro em muitas câmeras → aviso de rede confiável; preferir VLAN dedicada. Suporte a HTTPS/RTSPS quando houver.
- Varredura de rede só sob ação do usuário.
- Plugins: carregamento só de diretório confiável, assinatura recomendada; API HTTP futura só em localhost por padrão com token.
- Atualização: instalador assinado; verificação de integridade.

## 22. Licenças (a ser formalizado em `LICENSES.md`)

| Componente | Licença | Cuidado |
|---|---|---|
| Qt 6 | LGPLv3 (módulos comuns) / comercial | Linkagem dinâmica, permitir troca da lib, texto da licença, não usar módulos GPL-only (ex.: alguns add-ons) |
| FFmpeg | LGPL 2.1+ **ou** GPL conforme build | Build **sem** `--enable-gpl`/`--enable-nonfree`; sem x264/x265; dinâmico |
| H.264/H.265 | Patentes (MPEG LA/Access Advance/Via) | Uso de encoder/decoder do SO/GPU transfere parte do risco, mas não elimina; **consultar jurídico** antes de distribuir comercialmente |
| NDI SDK | Proprietária (Vizrt) | Aceitar EULA; atribuição obrigatória; regras de redistribuição do runtime **[verificar texto atual]** |
| libsrt | MPL 2.0 | Divulgar modificações dos arquivos MPL |
| OBS (código) | GPL-2.0+ | **Não copiar código** (incl. do virtual camera); implementar do zero |
| pugixml/spdlog/miniaudio | MIT/Unlicense | Atribuição |
| ONVIF | Especificações públicas; marca "ONVIF" e conformidade formal exigem filiação | Dizer "compatível com ONVIF" sem alegar certificação |
| SDKs Blackmagic/AJA/Magewell | Termos próprios | Só em plugins, fora do núcleo |

Sem aconselhamento jurídico definitivo aqui — recomendo revisão antes do primeiro release.

---

## 23. Riscos técnicos (priorizados)

| # | Risco | Prob. | Impacto | Mitigação |
|---|---|---|---|---|
| 1 | Virtual Camera (Win DirectShow/MF, macOS CMIO) | Alta | Crítico | PoC antecipada (antes da Fase 7); NDI Output como plano B |
| 2 | FOMAKO NK20A diverge do ONVIF/precisa protocolo proprietário | Média | Alto | Obter câmera + manual; capturar tráfego; `FomakoPTZProvider` isolado |
| 3 | Latência RTSP via FFmpeg acima da meta | Média | Alto | Benchmark; tuning; stack RTP própria se necessário |
| 4 | Crash de FFmpeg/SDK com stream malformado derruba app | Média | Crítico | Inputs em **processo trabalhador separado** (ver Decisão abaixo) |
| 5 | 10–16 fontes além da capacidade de hardware | Alta | Alto | Sub-streams, HW decode, Resource Manager com limites |
| 6 | Sincronismo A/V e drift entre câmeras IP | Média | Médio | PTS + offsets manuais; documentar ausência de genlock |
| 7 | Licenças (NDI, FFmpeg/codecs, Qt) | Média | Alto | Auditoria + LICENSES.md + revisão jurídica |
| 8 | Assinatura/notarização/permissões macOS | Alta | Médio | Conta Apple cedo; CI de assinatura |
| 9 | Multicast bloqueado em redes reais | Alta | Médio | Adição manual por IP, diagnóstico |
| 10 | Áudio para OBS (virtual camera não leva áudio) | Certa | Médio | NDI Output / virtual audio device futuro |
| 11 | Escopo gigantesco | Certa | Alto | Fases com critérios objetivos; MVP enxuto |

### Decisão técnica: isolamento de falhas (requisitos 38, 59, 70)

**PROBLEMA:** garantir que câmera/stream/protocolo defeituoso não derrube o Program.
**OPÇÕES:** (a) threads no mesmo processo com try/catch; (b) processo trabalhador por input (ou por grupo); (c) híbrido.
**DECISÃO:** (c). Interface `IInputWorker` desenhada desde a Fase 1 para ser *in-process* ou *out-of-process*; MVP roda em threads com watchdog; antes do release 1.0 (Fase 16) inputs de rede/decode migram para processos filhos entregando frames via memória/texturas compartilhadas. Exceções C++ não protegem de segfault em código nativo de terceiros — por isso (a) sozinho **não** cumpre o requisito.
**TRADE-OFFS:** (b) aumenta complexidade (IPC, textura compartilhada, latência de reinício).
**COMO TESTAR:** fuzz de streams corrompidos + `kill -9` do worker durante PGM; o Program deve manter o último frame e reconectar.

---

## 24. MVP (Windows-first)

Escopo exato (itens 1–12 da seção 66): Discovery ONVIF · RTSP · Discovery NDI · 4–8 câmeras · PTZ · Preview · Program · CUT · Dissolve · Program Output · Virtual Camera · OBS recebendo "HB Studio Program".

**Fora do MVP:** áudio, gravação, streaming, cenas, gráficos, multi-monitor avançado, macOS, SRT, 10+ câmeras (todos permanecem no roadmap).

Teste principal: FOMAKO PTZ → RTSP/ONVIF → Multiview → PVW → CUT → PGM → HB Studio Virtual Camera → OBS.

## 25. Roadmap por fases (com modificação proposta)

Mantive as 18 fases do prompt. Proponho **duas mudanças de ordem**, por reduzirem risco — preciso da sua aprovação:

- **PoC de Virtual Camera (Windows)** executada em paralelo à Fase 1 (apenas prova de conceito: quadro de teste visível no OBS). Motivo: é o maior risco e invalida o MVP se falhar.
- **NDI Output** como alternativa de saída para OBS, adiantada se a PoC falhar.

| Fase | Entregável | Critério de conclusão (objetivo) |
|---|---|---|
| 0 | Esta arquitetura | Aprovação por escrito |
| 1 | Projeto base: CMake, Qt, event bus, log com redação, CI, janela vazia, clock/Program Output (textura de teste) | Build Win; testes unitários verdes; 1 janela exibe textura a 60 fps estável 1 h |
| 2 | Discovery (ONVIF WS-Discovery, NDI finder, UVC) | Acha a NK20A e fonte NDI de teste; timeout/erros isolados; teste com rede sem multicast mostra diagnóstico |
| 3 | RTSP | 1 hora contínua, reconecta após desligar câmera/cabo (< 15 s), latência glass-to-glass medida e documentada, 0 vazamento de memória |
| 4 | ONVIF (media+device) | `GetProfiles/GetStreamUri` OK na NK20A; auth digest funcional |
| 5 | PTZ | Mover/parar/zoom/presets na NK20A; Stop garantido; falha de PTZ não afeta vídeo |
| 6 | NDI | Receber NDI e NDI HX; app inicia sem runtime NDI |
| 7 | Multiview | 8 fontes @ preview-res, CPU/GPU dentro do orçamento por perfil, tiles com status |
| 8 | Switcher | PVW/PGM, CUT, DISSOLVE (duração configurável) corretos frame a frame (teste automatizado de pixel) |
| 9 | Program Output | PGM independe do tamanho da janela; resolução fixa; consumidores múltiplos |
| 10 | Virtual Camera | Critério da seção 13 |
| 11 | OBS Integration | Teste principal (seção 67) com evidência (captura/vídeo) |
| 12 | Áudio | Mixer, meters, sync A/V ≤ ±40 ms |
| 13 | Gravação | MP4 H.264 HW, arquivo reproduzível, A/V sincronizado, 2 h sem queda de frames |
| 14 | Streaming | RTMP(S) para servidor de teste, reconexão |
| 15 | Multi-window | Mover módulos entre ≥ 2 monitores; persistência de layout |
| 16 | Otimização | 10 e 16 fontes em perfis alvo; inputs em processos isolados; `kill -9` não derruba PGM |
| 17 | Instalador | Instala/desinstala limpo em Win 10/11 VM; verifica dependências |
| 18 | Testes finais | Suíte completa, soak 8 h, relatório de performance |
| macOS | Entra após o MVP Windows validado (Camera Extension, VideoToolbox, Metal) | Mesmos critérios nos Macs alvo |

Funcionalidades futuras (replay, chroma key, NDI/SRT/WebRTC out, macros, Stream Deck, OSC/MIDI/HTTP API, tracking IA etc.) permanecem documentadas e a arquitetura (Event Bus, plugins, outputs plugáveis) as comporta.

## 26. Critérios de aceitação gerais

1. Nenhuma funcionalidade "pronta" sem: teste automatizado + teste manual documentado + métrica (CPU/GPU/latência) registrada.
2. Soak test: Program estável ≥ 1 h (MVP) / ≥ 8 h (release), sem vazamento de memória > 1%/h.
3. Falhas injetadas (câmera desconectada, stream corrompido, PTZ travado, rede caindo) não interrompem o Program.
4. Logs não contêm segredos (teste automatizado com grep de padrões).
5. LICENSES.md completo antes de qualquer distribuição.

---

## 27. Outros requisitos transversais (resumo de projeto)

- **Event Bus** (síncrono para estado, assíncrono para telemetria): `camera.*`, `ptz.*`, `switcher.*`, `transition.*`, `recording.*`, `stream.*`. Base futura da HTTP API/Web Control.
- **Perfis persistentes** (JSON versionado) por câmera; **Projeto de Transmissão** restaura tudo (câmeras, PTZ, presets, cenas, gráficos, áudio, saídas).
- **Failsafe** configurável por câmera/projeto: último frame · câmera reserva · preto · placeholder · outra cena.
- **Modos de latência** LOW LATENCY / BALANCED / STABLE controlam buffers de rede, filas, jitter buffer e política de descarte.
- **Developer Mode** e **Resource Manager** expõem CPU, GPU, RAM, VRAM, FPS, dropped frames, decode/render time, bitrate, latência.
- **Plugins** (Blackmagic/AJA/Magewell/DeckLink etc.) via ABI C estável, fora do núcleo.

---

## 28. Pontos que preciso que você decida/forneça para aprovar

1. Aprova **C++20 + Qt 6 + FFmpeg (LGPL) + engine separado**?
2. Aprova a **PoC antecipada de Virtual Camera** e **NDI Output** como plano B para o OBS?
3. Você tem acesso físico a uma **FOMAKO NK20A** (e manual/firmware) para validar ONVIF/PTZ?
4. Qual o **hardware-alvo** do MVP (CPU, GPU, RAM, NIC)? Isso define os perfis LOW…PRO.
5. **Windows 10 ou apenas 11?** (define se DirectShow é obrigatório).
6. Distribuição **comercial** ou uso interno? (impacta Qt LGPL vs comercial, NDI, patentes de codec.)
7. Você já possui/pode adquirir **Apple Developer Program** e certificado de code signing Windows?
8. Aprova isolamento híbrido (in-process no MVP, processos separados antes do 1.0)?

**Nenhuma implementação será iniciada até sua aprovação.**
