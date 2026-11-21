# TRANSMISSÃO (transmissão ao vivo multiplataforma via RTMP/RTMPS)

Módulo: `src/streaming/` (biblioteca `hb_streaming`, protocolo — portável) + `src/streaming/windows/`
(biblioteca `hb_streaming_win`, codificação — Windows/Media Foundation) + `src/app/stream_destinations.*`
(lista de destinos, persistida) + `src/app/windows/stream_sender.*` (orquestração por destino) +
aba **TRANSMISSÃO** na UI (primeira aba, antes de PRODUÇÃO).

> **Estado de validação:** o protocolo RTMP (handshake, AMF0, chunking, connect/createStream/publish,
> envio de mensagens de vídeo/áudio) foi testado de ponta a ponta contra um servidor RTMP de teste
> rodando em socket real (`tests/rtmp_client_test.cpp`, `tests/rtmp_test_support.hpp`) — não é mock,
> é um socket TCP de verdade em 127.0.0.1. A codificação H.264/AAC (`src/streaming/windows/
> mf_video_encoder.cpp` / `mf_audio_encoder.cpp`) **ainda não está implementada** (placeholder que
> falha de forma honesta, não finge codificar) — por isso, hoje, um destino nunca chega a transmitir
> de verdade, mesmo com credenciais corretas: ele nem chega a abrir a conexão de rede, porque
> `StreamSender` cria e inicia os codificadores *antes* de conectar (ver "Arquitetura" abaixo) e para
> cedo se eles falharem. Nada aqui foi testado contra uma plataforma real (YouTube/Twitch/etc.) nem
> contra hardware de aceleração (NVENC/QSV/AMF).

## Por que não há login/OAuth com nenhuma plataforma

YouTube, Facebook, Instagram, TikTok, X e LinkedIn exigem um aplicativo registrado e aprovado por
cada plataforma para fazer login OAuth em nome do usuário — isso não é algo que um app de código
aberto consiga obter unilateralmente (cada aprovação é um processo próprio, por plataforma, às vezes
pago, às vezes restrito a parceiros). A alternativa real e amplamente usada por ferramentas
profissionais de transmissão (é exatamente como o OBS Studio funciona) é: o usuário copia a própria
**chave de transmissão** da sua conta naquela plataforma (uma string opaca, gerada pela própria
plataforma) e cola no HB Studio; o "destino" nada mais é que um endereço de servidor RTMP(S) + essa
chave. Por isso `platform` (`src/app/stream_destinations.hpp`) é só um rótulo/preset de endereço de
servidor, nunca uma integração de conta.

**Instagram Live não está na lista de presets.** A Meta restringe a publicação ao vivo no Instagram a
parceiros aprovados via API (Graph API para contas comerciais, com aprovação caso a caso) — não há
um endereço RTMP público e estável que qualquer app possa usar, ao contrário de YouTube/Twitch/
Facebook/Kick/Vimeo. Oferecer um preset "Instagram" sem isso funcionar seria enganoso.

**Só YouTube e Twitch vêm com o endereço do servidor pré-preenchido.** São os dois endereços mais
estáveis e documentados publicamente; para as demais plataformas (Facebook, Kick, Vimeo, LinkedIn, X)
o campo fica em branco com uma dica — um endereço chutado errado é pior que nenhum, porque falha
silenciosamente só na hora de transmitir. "Personalizado" cobre qualquer servidor RTMP/RTMPS próprio
(ex.: um servidor `nginx-rtmp` ou `MediaMTX` da própria igreja/produtora).

## Arquitetura

1. **`src/streaming/amf0.*`** — codificador/decodificador AMF0 (o formato dos comandos RTMP:
   `connect`, `createStream`, `publish`, `onStatus`, `_result`). Só o subconjunto de tipos que RTMP
   usa nesses comandos (Number, Boolean, String, Object, Null, Undefined) — sem arrays ECMA, Date,
   XML, referências ou AMF3, que nenhum servidor real manda nessa troca.
2. **`src/streaming/flv.*`** — monta os *bodies* de tag de vídeo/áudio no formato FLV (RTMP reaproveita
   esse layout exato para suas mensagens de vídeo tipo 9 e áudio tipo 8, só sem o enquadramento de
   arquivo FLV). Puro empacotamento de bytes, sem codec nenhum.
3. **`src/streaming/rtmp_client.*`** (`RtmpClient`) — cliente RTMP(S) do zero: handshake simples,
   multiplexação de chunk stream, comandos `connect`/`createStream`/`publish`, envio de vídeo/áudio.
   Construído sobre `hb::rtsp::ITcpStream` (a abstração de socket já existente e multiplataforma do
   módulo RTSP — POSIX/Winsock), em vez de uma nova camada de socket. Deliberadamente síncrono/
   bloqueante (`start()`/`sendVideo()`/`sendAudio()` bloqueiam a thread chamadora): correto aqui
   porque cada destino roda na própria thread dedicada (`StreamSender`), nunca na thread de
   áudio/vídeo em tempo real — um destino travado trava só a própria thread.
4. **`src/streaming/windows/mf_encoder.hpp`** — contrato (`IH264Encoder`/`IAacEncoder`) para
   codificação H.264/AAC via `IMFTransform` do Media Foundation **diretamente** (não via
   `IMFSinkWriter`, que é o que `src/recording/windows/mf_writers.cpp` usa para gravação em MP4) —
   `IMFSinkWriter` não expõe nenhum ponto para pegar a amostra codificada crua antes de ele mesmo
   empacotar no MP4, então RTMP (que tem seu próprio empacotamento, FLV) precisa de um caminho
   separado. As implementações reais (`mf_video_encoder.cpp`/`mf_audio_encoder.cpp`) ainda são
   placeholders.
5. **`src/app/stream_destinations.*`** (`StreamDestinations`) — lista de até 16 destinos, persistida
   em `xmit.json` (mesmo diretório de `inputs.txt`/`ptz.json`), com a chave de transmissão guardada
   ali mas **nunca** devolvida por inteiro para a UI (`uiJson()` só manda `keySet`/`keyHint`
   mascarado — ver "Segurança" abaixo).
6. **`src/app/windows/stream_sender.*`** (`StreamSender`) — uma instância por destino, cada uma na
   própria thread. Capta o Program (vídeo via `ProgramOutput::addConsumer`, áudio via
   `AudioEngine::addProgramConsumer` — o mesmo ponto que `rec::Recorder` já usa para gravação) em
   filas limitadas que descartam o mais antigo sob atraso (nunca bloqueiam a thread de
   áudio/vídeo). Cria e inicia os codificadores **antes** de abrir a conexão RTMP: não faz sentido
   abrir uma conexão de verdade com a conta do operador (o que aparece como tentativa de conexão no
   painel da própria plataforma) se nada pode ser enviado por ela.

## Isolamento entre destinos

Cada `StreamSender` tem sua própria thread, seu próprio `RtmpClient`, seus próprios codificadores e
suas próprias filas. Uma falha de rede/codificação em um destino só altera o `status()` daquele
`StreamSender` — os outros continuam normalmente. `startXmit`/`stopXmit` em `app_main.cpp` operam por
id, sem tocar no mapa inteiro.

## Segurança das chaves

- `StreamDestinations::uiJson()` nunca inclui `streamKey`; manda `keySet` (bool) e `keyHint`
  (`maskSecret()`: `"••••" + últimos 4 caracteres`).
- O diálogo de edição da UI sempre carrega o campo de chave vazio, com a dica mascarada como
  placeholder; salvar sem preencher a chave manda `streamKey` vazio, e `app_main.cpp` trata isso como
  "manter a chave já salva" (nunca apaga uma chave por causa de uma edição que não mexeu nela).
- `core::redactSecrets` (já existente, `src/core/redact.hpp`) já documenta suporte a chaves
  RTMP(S) em texto livre antes de qualquer coisa chegar a um log.

## Limitações conhecidas / próximos passos

- Codificação H.264/AAC real via Media Foundation (`mf_video_encoder.cpp`/`mf_audio_encoder.cpp`):
  pendente.
- `RtmpClient` não implementa RTMPS (TLS) ainda — só RTMP puro, que é o que todo preset de plataforma
  oferece de qualquer forma (RTMPS costuma ser necessário só por trás de certos proxies/CDNs).
- Sem SRT (mencionado como protocolo na UI/dados, mas sem implementação).
- Cada destino hoje roda seu próprio par de codificadores mesmo quando dois destinos pedem o mesmo
  bitrate/resolução — evitar essa codificação redundante compartilhando um encoder entre destinos
  compatíveis é uma otimização futura, não implementada nesta primeira versão.
- Nada foi validado contra uma plataforma real, contra hardware de aceleração (NVENC/QSV/AMF) ou em
  uma transmissão de longa duração — só contra um servidor RTMP de teste em loopback.
