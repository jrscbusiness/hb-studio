# Camada de rede compartilhada (`src/net`)

Biblioteca `hb_net` (`cmake/modules/net.cmake`). Existe para que o código sensível a segurança de rede (cliente
WinHTTP, DPAPI, validação de URL/host) exista **uma única vez**: uma correção vale para todos os módulos.

## Peças

| Arquivo | Função |
|---|---|
| `net/http.hpp` | `IHttpClient`, `HttpRequest`, `HttpResponse`. Implementação real: `makeWinHttpClient()` (`http_win.cpp`, só Windows). Fora do Windows devolve um cliente que sempre falha com mensagem clara. |
| `net/http_fake.hpp` | `FakeHttpClient`: cliente em memória para testes (fila de respostas ou *handler*, registra requisições e uploads). Usa a mesma validação e a mesma política de corpo do cliente real. |
| `net/body_reader.hpp` | Política portátil do corpo da resposta (sink de streaming, limite de bytes, truncar ou falhar, progresso/cancelamento). Compartilhada pelo cliente real e pelo fake. |
| `net/secret_store.hpp` | `ISecretStore` (`protect`/`unprotect` de blobs opacos). Real: DPAPI por usuário com *entropy* opcional (`makeDpapiSecretStore`). Teste: `FakeSecretStore`. |
| `net/url.hpp` | `parseUrl`, `isLoopbackHost`, `isRfc1918Host`, `isLocalNetworkHost`, `parseDottedQuad`. |

## Garantias fixas (não configuráveis)
- Certificado HTTPS **sempre** verificado: nenhum `SECURITY_FLAG_IGNORE_*` em `src/net` (o teste `net_source_guard_test`
  falha se alguém introduzir).
- Redirecionamento nunca rebaixa https para http (`REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP`); `RedirectPolicy::Never` devolve o 3xx.
- A requisição é recusada antes de tocar a rede se a URL não for http/https estrita (sem *user-info*, espaços, `\`),
  se `httpsOnly` e a URL for http, ou se um cabeçalho contiver CR/LF (injeção de cabeçalho).
- Hosts: `127.evil.com` e `10.0.0.1.evil.com` **não** são locais; octetos com zero à esquerda (`010.0.0.1`) não são
  quads decimais (ambiguidade octal). Antes essa correção existia só em `src/ai` e `src/cloud`.

## Como um módulo usa a camada
Cada módulo mantém sua interface pequena (`IAiHttp`, `IChatHttp`, `ICloudHttp`, `IHttpDownloader`, `IDownloader`/`IHttpPoster`,
`hb::onvif::IHttpClient`, `hb::chroma::IHttpClient`, `ISecretStore` próprios) e ganha um **adaptador fino e portátil**
(`src/<modulo>/net_adapter.cpp`) sobre `hb::net::IHttpClient`. A política do módulo (timeouts, tetos de tamanho,
`Retry-After`, mensagens pt-BR, portão de banda da nuvem) fica no adaptador; os testes existentes continuam usando seus
fakes e há testes novos dos adaptadores com `FakeHttpClient`. O arquivo Windows de cada módulo ficou só com a fábrica
(`makeDefault...`) que monta o cliente WinHTTP com User-Agent, modo de proxy e TLS próprios.

| Módulo | Adaptador | Observação |
|---|---|---|
| `src/ai` | `net_adapter.cpp` | HTTP + DPAPI (entropy `HBStudio.ai.v1`) + `isLoopbackHost` |
| `src/onvif` | `net_adapter.cpp` | sem proxy (câmeras na LAN) |
| `src/chroma` | `net_adapter.cpp` | só HTTPS, só status 200 |
| `src/captions` | `net_adapter.cpp` | download com progresso + POST |
| `src/cloud` | `net_adapter.cpp` | sem redirecionamento; fatias de 32 KiB com portão; arquivo de segredos por nome (DPAPI, entropy `HBStudio.cloud.v1`); `isLocalNetworkHost` |
| `src/chat` | `net_adapter.cpp` | HTTP + um arquivo por segredo (DPAPI sem entropy). **WebSocket (Twitch) continua em `net_win.cpp`**: usa `WinHttpWebSocket*`, que o cliente compartilhado não cobre |
| `src/marketplace` (e `src/updater`, que o reutiliza) | `net_adapter.cpp` | TLS 1.2/1.3, máx. 5 redirecionamentos, ETag |

Compatibilidade de dados: entropy e descrição DPAPI de cada módulo foram preservadas, então blobs já gravados continuam
decifrando.

## Diferenças conhecidas (diagnósticos, não comportamento)
- Textos de erro de transporte ficaram padronizados por etapa (`WinHttpConnect falhou (WinHTTP n)`); cada adaptador mantém o
  idioma que o módulo já usava.
- Um corpo acima do teto, no modo *truncar* (nuvem, legendas), interrompe a leitura em vez de continuar descartando.
- `Content-Length` agora é lido em 64 bits (antes 32 bits em legendas/marketplace).

## Não validado
Tudo que é Windows (`http_win.cpp`, `secret_store_win.cpp`, fábricas) foi verificado apenas com `tools/check_windows_syntax.sh`
(mingw) e pela CI; nenhum teste executou WinHTTP/DPAPI reais.
