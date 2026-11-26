# TRANSMISSÃO (transmissão ao vivo multiplataforma via RTMP/RTMPS)

Módulo: `src/streaming/` (biblioteca `hb_streaming`, protocolo — portável) + `src/streaming/windows/`
(biblioteca `hb_streaming_win`, codificação — Windows/Media Foundation) + `src/app/stream_destinations.*`
(lista de destinos, persistida) + `src/app/windows/stream_sender.*` (orquestração por destino) +
aba **TRANSMISSÃO** na UI (primeira aba, antes de PRODUÇÃO).

> **Estado de validação:** o protocolo RTMP (handshake, AMF0, chunking, connect/createStream/publish,
> envio de vídeo/áudio) é testado de ponta a ponta contra um servidor RTMP de teste em socket TCP real
> (`tests/rtmp_client_test.cpp`). **RTMPS** é testado em duas camadas: a lógica de TLS (laço de E/S,
> enquadramento de registros, escritas parciais, rejeição de certificado) com um motor TLS falso
> (`tests/tls_stream_test.cpp`, `tests/rtmps_test.cpp`); o motor real (Schannel, `src/streaming/tls_schannel.cpp`)
> só é verificado por compilação — **não foi validado contra um servidor real** (YouTube/Twitch/etc.). Os
> codificadores H.264/AAC via Media Foundation (`mf_video_encoder.cpp`/`mf_audio_encoder.cpp`) estão
> implementados, mas **sem validação em hardware de aceleração (NVENC/QSV/AMF) nem em transmissão longa**.
> Nada aqui foi testado contra uma plataforma real. **SRT não está implementado** (veja `docs/SRT.md`).

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

**Só YouTube e Twitch vêm com o endereço do servidor pré-preenchido** (RTMP e RTMPS:
`rtmp://a.rtmp.youtube.com/live2` / `rtmps://a.rtmps.youtube.com:443/live2`, `rtmp://live.twitch.tv/app` /
`rtmps://live.twitch.tv:443/app`; trocar o protocolo no editor troca o preset, nunca um endereço digitado à mão).
São os dois endereços mais estáveis e documentados publicamente; para as demais plataformas (Facebook, Kick, Vimeo, LinkedIn, X)
o campo fica em branco com uma dica — um endereço chutado errado é pior que nenhum, porque falha
silenciosamente só na hora de transmitir. "Personalizado" cobre qualquer servidor RTMP/RTMPS próprio
(ex.: um servidor `nginx-rtmp` ou `MediaMTX` da própria igreja/produtora). O Facebook exige RTMPS
(`rtmps://live-api-s.facebook.com:443/rtmp/`, **endereço não verificado**: copie o da sua página de transmissão;
o editor só mostra isso como dica).

## Arquitetura

1. **`src/streaming/amf0.*`** — codificador/decodificador AMF0 (o formato dos comandos RTMP:
   `connect`, `createStream`, `publish`, `onStatus`, `_result`). Só o subconjunto de tipos que RTMP
   usa nesses comandos (Number, Boolean, String, Object, Null, Undefined) — sem arrays ECMA, Date,
   XML, referências ou AMF3, que nenhum servidor real manda nessa troca.
2. **`src/streaming/flv.*`** — monta os *bodies* de tag de vídeo/áudio no formato FLV (RTMP reaproveita
   esse layout exato para suas mensagens de vídeo tipo 9 e áudio tipo 8, só sem o enquadramento de
   arquivo FLV). Puro empacotamento de bytes, sem codec nenhum.
3. **`src/streaming/transport.hpp`** (`IStreamTransport`) — a costura entre "o que um destino faz" (publicar
   corpos de mensagem de vídeo/áudio) e "como os bytes chegam" (RTMP, RTMPS, futuramente SRT).
   `makeStreamTransport("rtmp"|"rtmps"|"srt")`; `srt` devolve `nullptr` com a explicação (ver `docs/SRT.md`).
4. **`src/streaming/rtmp_client.*`** (`RtmpClient`, implementa `IStreamTransport`) — cliente RTMP(S) do zero:
   handshake simples, multiplexação de chunk stream, `connect`/`createStream`/`publish`, envio de vídeo/áudio.
   Construído sobre `hb::rtsp::ITcpStream`. Síncrono/bloqueante: correto porque cada destino roda na própria thread.
   Falhas "permanentes" (servidor recusou a publicação, certificado rejeitado, URL inválida) são marcadas
   (`permanentFailure()`) para que a reconexão não insista.
5. **RTMPS** — `src/streaming/tls_engine.hpp` (`ITlsEngine`, máquina de estados TLS *sem E/S*),
   `tls_stream.*` (`TlsStream`: decorador `ITcpStream` que cuida do laço de E/S, buffers, escritas parciais,
   `WouldBlock`; o handshake roda dentro de `pollConnect()`) e `tls_schannel.cpp` (motor real no Windows:
   Schannel/SSPI — `AcquireCredentialsHandle`, laço `InitializeSecurityContext`, `EncryptMessage`/
   `DecryptMessage` com `SECPKG_ATTR_STREAM_SIZES`). Veja "RTMPS" abaixo.
6. **`src/streaming/windows/mf_encoder.hpp`** — contrato (`IH264Encoder`/`IAacEncoder`) para codificação
   H.264/AAC via `IMFTransform` diretamente (não `IMFSinkWriter`, que só grava MP4). Extensões em tempo de
   execução (melhor esforço): `setBitrateKbps` (`CODECAPI_AVEncCommonMeanBitRate`) e `requestKeyFrame`
   (`CODECAPI_AVEncVideoForceKeyFrame`).
7. **`src/streaming/stream_stats.*`**, **`adaptive_bitrate.*`**, **`reconnecting_publisher.*`** — lógica pura
   e portável (relógio injetável, testada em `tests/*_test.cpp`): saúde, bitrate adaptativo, reconexão.
8. **`src/app/stream_destinations.*`** (`StreamDestinations`) — lista de até 16 destinos, persistida em
   `xmit.json`, com a chave guardada ali mas **nunca** devolvida por inteiro à UI (`uiJson()` só manda
   `keySet`/`keyHint`). Campo novo: `adaptiveBitrate` (padrão `false`).
9. **`src/app/windows/stream_sender.*`** (`StreamSender`) — uma instância por destino, na própria thread.
   Capta o Program (vídeo via `ProgramOutput::addConsumer`, áudio via `AudioEngine::addProgramConsumer`) em
   filas limitadas que descartam o mais antigo sob atraso (e contam o descarte). Cria os codificadores
   **antes** de abrir a conexão e envia por um `ReconnectingPublisher`.

## RTMPS (TLS)

- `rtmps://host[:porta]/app` — porta padrão **443**. O esquema da URL decide o TLS; se o destino diz
  "RTMPS" mas a URL ainda é `rtmp://`, o `StreamSender` promove para `rtmps://` (porta 443 se não escrita).
- O `tcUrl` anunciado no `connect` usa `rtmps://` e inclui a porta quando não é a padrão.
- **Verificação do certificado sempre ligada, sem opção para desligar.** Schannel valida a cadeia contra o
  repositório de raízes do Windows e confere o nome do servidor; depois do handshake a cadeia é validada de novo
  explicitamente (`CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, nome do servidor)`). Certificado
  inválido/expirado/de outro nome = falha **permanente** com mensagem em pt-BR (sem repetir tentativas).
  Não há checagem de revogação online (OCSP/CRL).
- Só TLS 1.2 é habilitado (todos os destinos atuais aceitam); TLS 1.3 e renegociação não são tratados.
- Em builds não-Windows não há motor TLS: RTMPS falha com erro claro — nunca cai para texto puro.
- Limitação conhecida: se o socket aceitar só parte de um registro TLS, o resto sai no próximo
  `send()`/`waitReadable()`/`stop()` (mídia chega a cada poucos ms).

## Saúde da transmissão

`StreamStats` (janela deslizante de 10 s, histórico de 30 s) por destino: bitrate **alvo** (vídeo+áudio) × **real**
(payload enviado, sem cabeçalhos), bitrate atual pedido ao codificador (difere do alvo sob bitrate adaptativo),
quadros de vídeo/áudio **descartados** pelas filas limitadas (total e na janela), **reconexões** bem-sucedidas
após queda, último erro, nome do codificador (`x264`/`nvenc`/`qsv`/`amf`) e profundidade das filas. Vai na
mensagem `xmit_status` (campo `stats`, mais `reconnecting` e `adaptive`) e aparece no card do destino na aba
TRANSMISSÃO: sparkline em `<canvas>` (30 s de bitrate) e uma linha "5.9 / 6.2 Mbps · descartes 0 · reconexões 0 ·
fila 0/1 · nvenc"; fica em destaque quando há descartes recentes ou reconexão.

## Bitrate adaptativo (opcional, padrão DESLIGADO)

Caixa "Bitrate adaptativo" no editor do destino. A cada ~1 s o controlador (`AdaptiveBitrate`, lógica pura)
olha a fila de envio de vídeo e os descartes: **estresse** = descartes novos, fila ≥ 50% da capacidade, ou fila
≥ 25% e crescendo. Após 3 amostras seguidas de estresse e 5 s desde a última mudança, reduz o bitrate de vídeo
×0,85 (piso: **40% do alvo**). Recupera **devagar**: só depois de 20 s contínuos saudáveis (fila ≤ 15%, sem
descartes) e 10 s desde a última mudança, soma 5% do alvo por vez (e exige outros 20 s saudáveis para o passo
seguinte). Amostras neutras não reduzem nem recuperam (histerese, evita oscilação).
O encoder recebe o novo bitrate via `ICodecAPI` (`CODECAPI_AVEncCommonMeanBitRate`) — **melhor esforço**: muitos
MFTs de hardware ignoram/recusam; nesse caso o recurso se desativa sozinho e o card mostra o aviso.
Não validado em hardware real.

## Reconexão automática

`ReconnectingPublisher`: ao perder a conexão, tenta de novo com espera exponencial (1, 2, 4 … máx. 30 s, ±20%
de jitter), até 20 tentativas seguidas; o contador só zera depois de 30 s de conexão estável (um link que
oscila continua com espera longa). Recusa do servidor (chave/app inválidos), certificado rejeitado, URL
inválida ou protocolo indisponível = desiste na hora. A cada nova conexão os timestamps recomeçam em 0, os
cabeçalhos AVC/AAC são reenviados e o vídeo só volta a partir de um quadro-chave (o encoder é instruído a gerar
um). Enquanto desconectado, os quadros são descartados (o card mostra RECONECTANDO e o tempo até a próxima tentativa).
Testado com transporte falso e relógio manual (`tests/reconnecting_publisher_test.cpp`).

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

- **SRT não implementado** — ver `docs/SRT.md` (decisão, passos exatos, interface de encaixe).
- Schannel (RTMPS) e `setBitrateKbps`/`requestKeyFrame` do Media Foundation: só compilados, **não validados** em
  Windows real nem contra plataformas reais.
- Cada destino roda seu próprio par de codificadores mesmo quando dois destinos pedem o mesmo bitrate/resolução
  (compartilhar um encoder é otimização futura).
- Nada foi validado contra uma plataforma real, hardware de aceleração (NVENC/QSV/AMF) ou transmissão longa.
