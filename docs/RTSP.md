# RTSP / RTP (módulo `src/rtsp`)

Pilha RTSP/RTP **própria**, sem FFmpeg, que recebe vídeo H.264/H.265 de câmeras IP e entrega
**Access Units em Annex B com PTS**. A decodificação fica atrás da interface `IVideoDecoder`
(apenas declarada; virá depois via FFmpeg/Media Foundation).

Estado: lógica 100 % testada sem câmera e sem rede (servidor RTSP roteirizado em memória, 140 testes
em `tests/rtsp_*`), mais testes de loopback com sockets TCP reais no Linux. **Nada foi validado contra
uma câmera real** (veja "Limites conhecidos" e "Como validar com a FOMAKO NK20A").

## 1. Arquitetura

```
 ITcpStream (socket real | fake de teste)
      │ bytes
      ▼
 RtspSession ── MessageParser ── Authenticator (Basic/Digest, MD5 próprio)
  OPTIONS→DESCRIBE→SETUP→PLAY→(keepalive)→TEARDOWN, SDP, redirecionamento, 401/404/454/461
      │ frames interleaved '$' (canal RTP / canal RTCP)
      ▼
 StreamReceiver
  parseRtp → JitterBuffer (reordena, duplicatas, perda, jitter, bitrate)
           → Depacketizer H.264 (single/STAP-A/FU-A) | H.265 (single/AP/FU)
           → AccessUnit (Annex B + SPS/PPS/VPS injetados antes de IDR/IRAP)
  RTCP: lê SR, gera Receiver Report (+SDES CNAME) a cada 2 s
      │
      ▼
 RtspInput (provider)
  estados STOPPED / CONNECTING / ONLINE / ERROR / RECONNECTING,
  backoff exponencial com jitter, watchdogs, health monitor, fila limitada de AUs
```

| Arquivo | Papel |
|---|---|
| `url`, `message`, `auth`, `md5`, `base64` | Parser/serializador RTSP (texto + frames `$`), URL (`rtsp://user:pass@host:port/path`), Basic/Digest |
| `sdp` | `m=video`, `a=rtpmap`, `a=fmtp` (`sprop-parameter-sets`; `sprop-vps/sps/pps`), `a=control`, framerate/resolução quando existirem |
| `rtp`, `depacketizer*`, `jitter_buffer`, `stream_receiver` | Caminho de mídia |
| `session` | Máquina de estados RTSP (sem I/O, relógio injetado) |
| `rtsp_input` | Provider com reconexão e saúde |
| `tcp_stream.hpp`, `tcp_stream_posix.cpp`, `tcp_stream_win.cpp` | Interface `ITcpStream` e implementações de socket |
| `decoder.hpp` | `IVideoDecoder` (somente declaração) |

Princípios de projeto (de `docs/CONTRIBUTING.md` e `ARCHITECTURE.md` §1, §10):

- **Tempo injetável**: `RtspInput::tick(now)`, `RtspSession::poll(now)`, `JitterBuffer::push(..., now)`. Nenhum teste usa `sleep`.
  `startThread()` apenas envolve `tick` com o relógio real.
- **Nenhuma entrada de rede derruba o processo**: tamanhos sempre validados (cabeçalho RTSP ≤ 16 KiB, corpo ≤ 256 KiB,
  SDP ≤ 256 KiB, AU ≤ 8 MiB, no máximo 32 `m=`), exceções capturadas nas fronteiras (`tick`, callbacks), fuzz nos
  parsers (RTSP, RTP, H.264, H.265) sob ASan/UBSan. Falha de um input vira `ERROR → RECONNECTING`, nunca propaga.
- **Segredos nunca em log**: URL sempre mascarada (`rtsp://***@host/...`), o cabeçalho `Authorization` nunca é registrado, e tudo
  passa por `hb::core::Logger` (que aplica `redactSecrets`). Há teste que procura a senha nos logs e no `InputHealth`.

## 2. Sessão RTSP

Sequência: `OPTIONS` → `DESCRIBE` → `SETUP` → `PLAY` → keepalive → `TEARDOWN`.

- **Transporte padrão: RTP intercalado em TCP** (`RTP/AVP/TCP;unicast;interleaved=0-1`), o mais robusto contra firewall/NAT/perda.
  UDP está **preparado na sessão** (`TransportMode::Udp`: monta `client_port`, lê `server_port`), mas o **caminho de dados UDP
  não existe**; `RtspInput` com UDP falha de forma explícita (`Config: ... not implemented`).
- **Autenticação**: Basic e Digest (MD5 e MD5-sess, `qop=auth` ou RFC 2069 sem qop). Digest tem prioridade se a câmera oferecer
  os dois. `auth-int` e SHA-256 não são suportados (sem esquema utilizável => `AuthFailed`). `nc` incrementa por requisição;
  `stale=true` é tratado de forma transparente; uma segunda rejeição com a mesma credencial é `AuthFailed` (sem laço).
  MD5 implementado do zero e testado com os vetores do RFC 1321; Digest conferido com os exemplos do RFC 2617 e RFC 7616.
- **Códigos tratados**: 401 (autentica e repete, no máximo 2 vezes por requisição), 404 (`NotFound`), 454 (`SessionLost`, inclusive
  no keepalive), 461 (`UnsupportedTransport`), 3xx com `Location` (redirecionamento), demais 4xx/5xx (`ServerError`). Câmera sem
  `OPTIONS` (405/501) é tolerada.
- **Redirecionamento**: `RtspInput` reconecta no novo `Location` imediatamente e sem backoff, até `maxRedirects` (5).
- **Keepalive**: intervalo = metade do `timeout` da `Session`, limitado a 5–30 s; usa `GET_PARAMETER` se anunciado em `Public`,
  senão `OPTIONS`. Keepalive sem resposta **não** derruba a sessão (quem decide é o watchdog de dados). O servidor pode enviar
  requisições (ping `GET_PARAMETER`); elas são respondidas.
- **Resolução de `a=control`** segue o RFC 2326 C.1.1 (`Content-Base` > `Content-Location` > URL do pedido; controle absoluto vence).
- Só a primeira trilha de vídeo H.264/H.265 é configurada; áudio é ignorado. `RTP/SAVP` (SRTP) e `rtsps://` não são suportados.

## 3. Caminho de mídia

**Jitter buffer** (`JitterBuffer`): reordena por número de sequência (extensão a 32 bits, wraparound), descarta duplicatas e
pacotes tardios, libera pacotes contíguos **imediatamente** (só espera quando há buraco), detecta reinício do remetente
(salto de sequência confirmado por 2 pacotes, troca de SSRC confirmada por 3). Estatísticas conforme RFC 3550 apêndice A:
recebidos, esperados, perdidos (e %), duplicados, tardios, reordenados, jitter de chegada, maior seq. estendido. `BitrateMeter`
mede bitrate em janela deslizante de 1 s.

**Despacotador H.264** (RFC 6184): single NAL, STAP-A, FU-A. **H.265** (RFC 7798): single NAL, AP, FU (sem DONL; SDP com
`sprop-max-don-diff > 0` é recusado). STAP-B/MTAP/FU-B/PACI são contados em `unsupportedNals` e ignorados.

- Fim de Access Unit pelo bit marcador **ou** por mudança de timestamp RTP (marcador perdido).
- Saída em Annex B (`00 00 00 01`). **SPS/PPS (e VPS) do SDP, ou os últimos vistos no fluxo, são injetados antes de cada IDR/IRAP
  que não os traga** (depois de um AUD inicial, se houver).
- **Política de perda** (`dropUntilKeyframeAfterLoss`, padrão ligada): um AU com pacote perdido é descartado, e **tudo é
  descartado até o próximo keyframe íntegro**, porque a cadeia de referência foi quebrada. Com a opção desligada, AUs danificados
  saem com `damaged=true` (o fragmento NAL incompleto nunca é emitido). O início do fluxo também espera o primeiro keyframe.
- Resistência: pacote truncado, bit proibido, tamanhos inconsistentes em STAP/AP, FU sem início, FU com S+E, duplicatas e pacotes
  antigos são contados e descartados sem exceção. PTS = `(ts - ts_inicial) / clockRate` em µs, com tratamento do wrap de 32 bits;
  é **relativo ao início do fluxo**, não ao relógio de parede (sincronização via RTCP SR fica para depois).

**RTCP**: lê Sender Report (guarda LSR/DLSR), BYE (provoca reconexão) e gera Receiver Report + SDES CNAME a cada 2 s pelo canal
intercalado ímpar, com fração/total de perda, jitter e maior seq. estendido.

## 4. Modos de latência

Parâmetros vêm de `hb::core::latencyParams()` (`src/core/hardware_profile.hpp`):

| Modo | Espera por pacote faltante (`networkBufferMs`) | Capacidade do reordenador | Estouro | Fila de AUs (4 × `decodeQueueFrames`) |
|---|---|---|---|---|
| LOW_LATENCY | 50 ms | 128 pacotes | desiste do buraco (descarta o mais antigo) | 8 AUs |
| BALANCED | 200 ms | 512 | idem | 16 AUs |
| STABLE | 1000 ms | 2048 | rejeita o pacote novo e continua esperando | 48 AUs |

- Em **TCP intercalado** a ordem é garantida pelo transporte, então um buraco significa que o remetente descartou o pacote:
  a espera é limitada a **20 ms** em qualquer modo (esperar mais só adicionaria latência).
- A **fila de AUs** é limitada. No estouro com `dropOldest`, ela salta para o keyframe mais novo que contém; se não houver
  keyframe, esvazia e espera o próximo. Com `!dropOldest`, rejeita o AU novo (exceto keyframes) e espera o próximo keyframe.
  Contagem em `InputHealth::queueDropped`. O fator 4 absorve rajadas; `decodeQueueFrames` é a profundidade em regime.
- O jitter buffer **não atrasa** fluxo contíguo; a suavização de apresentação pertence ao compositor (mailbox do último frame, ARCHITECTURE §4).
- **Reconexão**: usa **1 s → 2 → 4 → 8 → teto 15 s, com jitter de ±20 %** (ARCHITECTURE §10 e critério de aceitação do RTSP), e **não**
  os campos `reconnect*` de `latencyParams()`, que variam por modo (0,5–2 s / 5–15 s). Configurável em `RtspInputConfig`.
  O backoff só volta a 1 s depois de `stableAfter` (10 s) ONLINE com dados, para uma câmera que oscila não ser martelada.

## 5. `RtspInput`: estados, timeouts, saúde

Estados: `STOPPED`, `CONNECTING` (primeira tentativa), `ONLINE` (primeiro AU recebido), `ERROR`, `RECONNECTING`
(`ERROR` é visível brevemente entre uma queda e o início da espera do backoff).

| Situação | Comportamento |
|---|---|
| TCP não conecta em 5 s | `Timeout` → reconecta com backoff |
| Requisição RTSP sem resposta em 5 s | `Timeout` → reconecta |
| PLAY ok mas nenhum RTP em 5 s | `NoData` → reconecta |
| Sem RTP por 3 s durante a reprodução (watchdog) | `NoData` → reconecta |
| RTP chega mas nenhum AU decodificável em 15 s | `NoData` → reconecta |
| Servidor fecha o socket / RTCP BYE / protocolo corrompido / 454 | reconecta |
| 404, 5xx | reconecta com backoff (câmera pode estar iniciando) |
| 401 com credencial errada | **ERROR permanente** (sem tentar de novo: evita bloqueio de conta na câmera); `retryOnAuthFailure=true` muda isso |
| URL inválida / UDP pedido | **ERROR permanente** (`Config`) |

`health(now)` devolve `InputHealth`: estado, nível (`GOOD` / `DEGRADED` se perda ≥ 2 % ou último quadro > 1,5 s / `BAD`),
último erro e tipo, bitrate (kbps), perda %, jitter (ms), AUs e keyframes, descartes da fila, nº de reconexões, tempo ONLINE,
tempo desde o último quadro e as estatísticas RTP/despacotador completas.

Threading: `tick` é mono-thread; callbacks de estado e de AU rodam na thread do `tick` (não chame `start/stop/tick` dentro deles).
`popAccessUnit()` e `health()` são seguros de outras threads. `startThread()/stopThread()` rodam `tick` com o relógio real.

## 6. Testes

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

`hb_rtsp_tests` (140 testes): vetores RFC 1321 e 2617/7616, parsers (inclusive fuzz), SDP, despacotadores H.264/H.265
(round-trip exato com STAP/FU, perda, duplicata, marcador perdido, truncamento, estouro), jitter/estatísticas, handshake RTSP
completo com servidor roteirizado (Basic, Digest, `stale`, senha errada, 404, 3xx, 454, 461, timeouts, keepalive), `RtspInput`
(online, perda de pacotes, queda no meio do stream, reconexão e temporização exata do backoff, watchdogs, segredos fora do log) e
3 testes de loopback com sockets TCP reais (servidor em thread). Também executados sem erros sob ASan/UBSan e (loopback) TSan.

## 7. Limites conhecidos

- **Não validado em hardware**: nenhuma câmera real foi usada. Cameras reais costumam ter particularidades (ordem de cabeçalhos,
  `Session` com `;timeout`, `Content-Base` ausente, SDP sem `sprop`, RTCP irregular, keepalive só por RTCP) que só aparecem em teste de campo.
- **UDP**: apenas negociação preparada; sem soquetes UDP, sem reordenação real. `IUdpSocket` ainda não existe.
- **Winsock** (`tcp_stream_win.cpp`): escrito, **não compilado nem executado** (ambiente Linux). Validar em Windows 11/MSVC 2022.
  Teste de loopback roda o mesmo código nos dois sistemas.
- **POSIX**: validado só em Linux/GCC; macOS não testado. `getaddrinfo` é bloqueante (câmeras são endereçadas por IP; um resolvedor
  em thread é trabalho futuro).
- Sem RTSPS/TLS, sem SRTP, sem `auth-int`/SHA-256, sem RTSP sobre HTTP tunneling, sem NAT traversal.
- H.265 sem DONL (interleaving), sem PACI. H.264 sem STAP-B/MTAP/FU-B. Parâmetros in-band: guarda até 4 SPS/PPS por tipo, sem
  analisar `sps_id`/`pps_id` (um fluxo com vários PPS alternados pode receber PPS antigo extra na injeção, sem efeito para decoders conformes).
- Resolução/fps vêm só do SDP (`a=framesize`, `a=x-dimensions`, `a=cliprect`, `a=framerate`); não há parser de SPS.
- PTS é relativo ao início do fluxo; sem alinhamento entre câmeras (ARCHITECTURE §4: "melhor esforço").
- Sem decodificação: `IVideoDecoder` é só declaração. Latência glass-to-glass **não medida**.
- Modos de latência usam valores iniciais de `latencyParams()` que ainda precisam de calibração com benchmark.

## 8. Como validar com a FOMAKO NK20A

A URL RTSP exata **não é assumida** aqui. Ela deve ser descoberta, por ordem de preferência:

1. **ONVIF `GetStreamUri`** (Media service, perfil H.264/H.265): é a fonte confiável; o resultado é a URL (`rtsp://IP:554/...`)
   a usar. O módulo ONVIF do projeto fará isso; manualmente é possível com qualquer cliente ONVIF (ex.: ONVIF Device Manager).
2. Manual/firmware do fabricante (a confirmar; não consultado).
3. Interface web da câmera, se mostrar a URL de stream.

Roteiro de teste de campo (marcar cada item como **validado** só depois de executado):

1. Conectar câmera e PC na mesma rede (de preferência VLAN dedicada; RTSP/ONVIF trafegam em claro).
2. Obter a URL pelo `GetStreamUri`. Informar usuário/senha **no campo de credenciais** (não na URL, quando possível).
3. Rodar `RtspInput` em TCP intercalado com `LatencyMode::Balanced` e conferir: chega ao `ONLINE`, `keyframes > 0`, perda ≈ 0 %, bitrate coerente.
4. Salvar os AUs (Annex B) em arquivo e abrir em um player (`ffplay -f h264 arquivo.h264`) para confirmar o bitstream.
5. Desligar/religar cabo ou câmera: reconexão em < 15 s (critério de aceitação do RTSP no ARCHITECTURE.md), sem vazamento de memória (soak de 1 h).
6. Repetir com Digest e Basic se a câmera permitir configurar; com senha errada deve parar em `ERROR` (`AuthFailed`).
7. Medir latência glass-to-glass (relógio na tela) nos três modos e registrar aqui.
8. Registrar quirks (SDP, `Session`, keepalive, GOP) e abrir issue/teste de regressão com o SDP real (sem credenciais).
