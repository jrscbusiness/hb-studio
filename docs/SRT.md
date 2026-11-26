# SRT (Secure Reliable Transport) — decisão e como encaixar depois

**Estado: NÃO implementado.** Escolher "SRT" no editor de destino mostra "SRT ainda não é suportado" e o
`StreamSender` recusa iniciar (nunca finge transmitir). A interface `IStreamTransport`
(`src/streaming/transport.hpp`) já existe e é o ponto de encaixe.

## Por que não foi implementado agora

Avaliação honesta das duas rotas:

1. **libsrt via `FetchContent`.** A libsrt (Haivision, MPL 2.0) exige uma biblioteca de criptografia
   (OpenSSL, mbedTLS ou GnuTLS) para o AES do modo criptografado — sem ela só dá para compilar SRT *sem*
   senha. No Windows isso significa trazer OpenSSL/mbedTLS para a CI e para o instalador, mais a cadeia de
   licenças (`LICENSES.md`), mais ~MB no instalador, e tudo isso sem ter como validar contra um servidor
   SRT real neste ambiente (a CI do Windows não tem destino SRT). Entregar isso "às cegas" arriscaria
   quebrar o build do Windows (a única plataforma-alvo) por um recurso que nenhuma plataforma-preset
   (YouTube/Twitch/Facebook) usa para ingestão com chave de transmissão.
2. **Implementar o protocolo do zero.** SRT é grande: handshake (induction/conclusion, cookie, extensões
   HSREQ/KMREQ), ARQ com NAK e retransmissão, TSBPD (entrega com tempo marcado), controle de congestionamento
   (LiveCC), criptografia AES-CTR/GCM com troca de chaves (KM), keep-alive, shutdown e MPEG-TS como carga
   útil (o SRT transporta TS, não FLV — falta também um *muxer* TS com PAT/PMT/PES/PCR). Não cabe de forma
   limpa e testável sem um par SRT de referência para interoperar.

## Passos exatos para implementar depois

1. `cmake/modules/srt.cmake`: `FetchContent_Declare(srt GIT_REPOSITORY https://github.com/Haivision/srt GIT_TAG <tag fixa>)`,
   `ENABLE_APPS=OFF`, `ENABLE_SHARED=OFF`, `USE_ENCLIB=mbedtls` (mbedTLS também via FetchContent: menor que
   OpenSSL e licença Apache-2.0), só no Windows; adicionar ambas a `LICENSES.md` e aos avisos do instalador.
2. `src/streaming/mpegts_muxer.{hpp,cpp}` (portável, testado): H.264 (Annex-B) + AAC (ADTS) → pacotes TS de 188 bytes
   (PAT, PMT, PES, PCR). `src/streaming/flv.*` não serve aqui: SRT leva TS.
3. `src/streaming/srt_transport.{hpp,cpp}`: `class SrtTransport : public IStreamTransport` — `open()` faz
   `srt_connect` com `streamid` (a "chave" do destino), `SRTO_PASSPHRASE` opcional, `SRTO_LATENCY`; `sendVideo`/
   `sendAudio` empacotam via o muxer e chamam `srt_sendmsg` (mensagens de até 1316 bytes = 7 pacotes TS).
   `permanentFailure()` = rejeição de `streamid`/senha.
4. `makeStreamTransport("srt")` (`src/streaming/transport.cpp`) passa a devolver `SrtTransport`. Nada mais muda:
   `ReconnectingPublisher`, estatísticas, bitrate adaptativo e a UI já funcionam sobre `IStreamTransport`.
5. Ajustar `StreamSender` para o *framing*: hoje ele envia corpos de tag FLV; para SRT o `SrtTransport` precisa
   receber unidades codificadas brutas (adicionar métodos `sendVideoUnit`/`sendAudioUnit` à interface, ou
   fazer o `StreamSender` escolher o empacotador pelo protocolo).
6. Testes: muxer TS com vetores conhecidos; `SrtTransport` contra um par `srt-live-transmit` em loopback (opcional na CI).
7. UI: remover o aviso "SRT ainda não é suportado" (`ui/src/js/xmit.js`, `xmitHint`) e documentar campos
   extras (latência, senha) no editor de destino.

Até lá, para ingestão SRT use um servidor próprio (MediaMTX/`srt-live-transmit`) recebendo RTMP(S) do HB Studio
e republicando em SRT.
