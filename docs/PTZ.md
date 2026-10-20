# Módulo PTZ (`src/ptz/`, `hb::ptz`)

Controle PTZ independente do vídeo: nenhuma falha de PTZ pode afetar o Program Output. Todo código é testável sem câmera
(transporte falso, relógio injetável). **Nada aqui foi validado em câmera real** — veja "Estado de validação".
Pesquisa sobre as câmeras FoMaKo: [FOMAKO.md](FOMAKO.md).

## Camadas

```
UI (10+ botões de preset, joystick)          PresetManager ─ PresetStore (JSON) ─ CameraPtzConfig
        │                                          │
        ▼                                          ▼
GuardedController  (rate limit ≤20 Hz, watchdog de Stop, fila não bloqueante, cache de status/presets)
        │  tick() chamado pela thread do PTZ Engine (~100 Hz)
        ▼
PTZProvider  ── ViscaProvider (IP/serial) ── ITransport (UDP real | Fake | serial futuro)
             ├─ PelcoProvider (D/P)       ── ITransport
             └─ OnvifPtzProvider          ── IOnvifPtzBackend (implementado pelo módulo onvif)
```

- `PTZController` (interface genérica): `connect, disconnect, panLeft, panRight, tiltUp, tiltDown, stop, zoomIn, zoomOut,
  setPanSpeed, setTiltSpeed, setZoomSpeed, gotoPreset, savePreset, deletePreset, getPresets, focusAuto, focusManual,
  focusNear, focusFar, irisOpen, irisClose, getCameraStatus` (+ `capabilities()` e `moveAbsolute()` opcional).
- Todo método devolve `PtzResult {Ok | NotSupported | Timeout | Error, detail}` (ou `PtzReply<T>`); exceções são
  capturadas nas fronteiras dos providers e do `GuardedController`.
- `PTZCapabilities` diz o que o provider suporta; a UI desabilita o resto. `selectProvider()` escolhe o primeiro candidato
  (ordem de preferência) que cobre as capacidades exigidas; senão o que cobre mais; se nenhum move a câmera, PTZ fica desabilitado.
  Ordem sugerida (ARCHITECTURE.md §6): ONVIF → VISCA IP → VISCA serial → Pelco-D → Pelco-P.

## Segurança operacional (`GuardedController`)

| Regra | Comportamento | Teste |
|---|---|---|
| Taxa | no máximo 20 comandos/s (o parâmetro é limitado a 20 Hz) | `Guard.RateLimitedTo20Hz` |
| Não bloqueante | métodos públicos só registram intenção/enfileiram; o I/O acontece em `tick()` | `Guard.CallsAreNonBlocking...` |
| Coalescência | vários comandos de movimento no mesmo intervalo viram **um** vetor (último vence) | `MovementCoalesces...` |
| **Watchdog de Stop** | movimento (pan/tilt/zoom/foco contínuo) precisa ser **renovado** em `watchdog` (padrão 500 ms) — a UI deve repetir `panLeft()` etc. (~10 Hz) enquanto o controle está pressionado; senão `Stop` é enviado | `WatchdogStops...` |
| Stop com prioridade | `stop()` ignora o limitador de taxa e cancela movimento pendente; se falhar, é repetido a cada intervalo do limitador até dar certo | `StopBypasses...`, `FailedStop...` |
| Desconexão | `disconnect()` envia Stop antes de fechar (e o provider também) | `DisconnectSendsStopFirst` |
| Fila | no máximo `maxQueue` (32) comandos de preset/íris/foco-modo; cheia → `Error`, nunca bloqueia | `UnsupportedAndQueueFull` |
| Timeout | por comando no provider (`ViscaConfig::ackTimeout`, padrão 500 ms), resultado `Timeout` | `TimeoutWhenCameraSilent` |
| Erros | só via callback `onError`; exceção no callback é engolida | `ErrorCallback...` |
| Relógio | `IPtzClock` injetável (`ManualClock` nos testes) | todos |

Observação: `getPresets()`/`getCameraStatus()` do `GuardedController` devolvem **cache** atualizado pelo `tick()`
(status a cada `statusPoll`, padrão 1 s). `connect()`/`disconnect()` chamam o provider diretamente e podem bloquear até o timeout:
chame-os da thread do PTZ Engine, não da UI.

Conversão de velocidade (magnitude 0..1; 0 → velocidade mínima válida quando há movimento):

| Protocolo | Pan | Tilt | Zoom |
|---|---|---|---|
| VISCA | 0x01–0x18 | 0x01–0x14 | variável 0–7 (`0x2p` tele / `0x3p` wide) |
| Pelco-D/P | 0x00–0x3F | 0x00–0x3F | sem velocidade variável (liga/desliga) |
| ONVIF | [-1,1] direto (`ContinuousMove`) | idem | idem |

## Providers

### VISCA (`visca.hpp`, `visca_provider.hpp`)
Comandos: Pan-tiltDrive (`8x 01 06 01 VV WW 0Y 0Y FF`), Zoom (`81 01 04 07 0p/2p/3p FF`), Preset Reset/Set/Recall
(`81 01 04 3F 00/01/02 pp FF`), Focus Auto/Manual (`04 38 02/03`), Focus Near/Far/Stop (`04 08 ..`), Iris Up/Down (`04 0B 02/03`),
Pan-tilt/Zoom absolutos, inquiries de posição (`09 06 12`, `09 04 47`). Parser de **ACK / Completion / Error**
(com códigos 01,02,03,04,05,41), reassemblagem de fluxo serial por terminador `FF`.

VISCA over IP: cabeçalho de 8 bytes `tipo[2] tamanho[2] sequência[4]` (big-endian) — Command `0x0100`, Inquiry `0x0110`,
Reply `0x0111`, DeviceSetting `0x0120`, Control `0x0200` (reset de sequência, payload `01`), ControlReply `0x0201`.
O comprimento declarado é validado contra o datagrama (nunca confia no tamanho vindo da rede).

Comportamentos a saber: Iris só tem efeito em modo AE manual na câmera; foco near/far contínuo é parado por `stop()`/watchdog;
VISCA não lista presets (`getPresets` → `NotSupported`; nomes ficam no `PresetStore`).

### Pelco (`pelco.hpp`, `pelco_provider.hpp`)
Pelco-D: `FF addr cmd1 cmd2 data1 data2 chk` (chk = soma dos bytes 1..5 mod 256). Pelco-P: `A0 addr c1 c2 d1 d2 AF chk`
(chk = XOR dos bytes 0..6). Protocolo sem resposta: `Ok` = bytes entregues ao transporte. Sem auto-foco, sem inquiry,
sem lista de presets. Íris é um pulso (comando + stop).

### ONVIF PTZ (`onvif_provider.hpp`)
Só usa `IOnvifPtzBackend` (SOAP vive no módulo onvif, de outro engenheiro). Capacidades vêm de `OnvifPtzBackendCaps`
(PTZ, presets, absolute, Imaging foco/auto-foco/íris, status). O que o backend não suporta devolve `NotSupported`.

## Presets, numeração e configuração por câmera

`CameraPtzConfig` (editada na UI de configuração, persistida no `PresetStore`):

| Campo | Valores |
|---|---|
| `protocol` | `onvif`, `visca-ip`, `visca-serial`, `pelco-d`, `pelco-p` |
| `host`, `port` | IP/porta (`port=0` → padrão: VISCA IP 52381, ONVIF 80); no serial, `host` guarda o nome da porta |
| `address` | VISCA 1–7 (em IP é fixo 1), Pelco-D 1–255, Pelco-P 0–31 |
| `presetCount` | **10–64** botões (fora do intervalo é ajustado) |
| `presetBase` | **0 ou 1**: número do primeiro preset enviado à câmera (botão *i* → `base + i`) |
| `storage` | `camera` (preset na câmera: Set/Recall/Reset) ou `software` (posição guardada no app, recall por movimento absoluto) |

`PresetManager` liga botão → câmera/store: `recall`, `save(nome)`, `clear`, `rename`, `label`, `isUsed`.
Armazenamento em software exige `absoluteMove` (VISCA: sim; ONVIF: se o backend suportar; Pelco: não) e posição
legível (`getCameraStatus`). Nomes ficam sempre no store, mesmo com preset na câmera.
`makeProvider(config, transport, onvifBackend)` cria o provider da configuração (ou `nullptr` → PTZ desabilitado).

### `PresetStore` (JSON)
Arquivo `{"schema":1,"cameras":{"<id>":{"config":{...},"presets":[{"id":1,"name":"Home","position":{...}}]}}}`.
Serializador/parser mínimo próprio (sem dependências), limite de profundidade 32 e de tamanho 8 MiB.
- Entrada corrompida → `LoadStatus::Corrupt`, store vazio, arquivo renomeado para `*.corrupt` (não é sobrescrito).
- Entradas individuais inválidas são ignoradas (`skippedEntries()`), as boas são mantidas.
- Schema maior que o suportado → `UnsupportedVersion` (arquivo intacto).
- Gravação atômica (arquivo temporário + rename).

## Transportes

`ITransport` (`open/close/send/receive(timeout)`), sem exceções. `FakeTransport` para testes.
`makeUdpTransport(host, port)`: **POSIX** (`udp_transport_posix.cpp`) — exercitado por teste de loopback;
**Winsock** (`udp_transport_win.cpp`) — **NÃO VALIDADO** (escrito sem build Windows). Serial e TCP: interface pronta, sem implementação real.

## Estado de validação

| Item | Estado |
|---|---|
| Codificação VISCA/Pelco, parsers, framing IP, watchdog, rate limit, store | testado em unidade (GCC 13, Linux) |
| UDP POSIX | testado em loopback (não contra câmera) |
| Winsock, MSVC | **não compilado/validado** |
| Layout dos comandos VISCA/Pelco | escrito a partir da especificação pública (Sony VISCA / Pelco-D/P), **não** conferido contra PDF nesta sessão nem contra câmera |
| VISCA over IP: valor inicial da sequência após reset (usamos 1), se a câmera responde o reset | **não validado** |
| FoMaKo NK20A / KN20A | **nada validado**; ver [FOMAKO.md](FOMAKO.md) para as perguntas em aberto (portas 52381/1259/5678, ONVIF 2000, endereçamento VISCA IP) |

## Roteiro de validação com câmera real (quando houver)

1. Wireshark no controle da câmera pelo controlador FoMaKo: identificar se "Sony VISCA 52381" usa o cabeçalho de 8 bytes e
   qual endereço/ID vai no byte `8x`; idem "IP VISCA" UDP 1259 / TCP 5678.
2. Testar `rtsp://<ip>:554/1` e `rtsp://<ip>:554/live/av0`.
3. ONVIF na porta 2000 (`GetCapabilities` → serviço PTZ/Imaging?).
4. Conferir faixa real de presets (spec fala em 255), numeração 0/1, comportamento de Iris/Focus.
