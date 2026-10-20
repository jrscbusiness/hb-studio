# ONVIF e Descoberta de dispositivos

Módulos: `src/onvif/` (biblioteca `hb_onvif`) e `src/discovery/` (biblioteca `hb_discovery`), registrados em
`cmake/modules/onvif.cmake`. Testes: `build/hb_onvif_tests` (68 testes, sem rede e sem câmera).

> **Estado de validação: nada aqui foi testado contra uma câmera real.** Todos os testes usam respostas XML
> reconstruídas à mão (inspiradas na estrutura típica de Hikvision, Dahua e Axis) e sockets/HTTP falsos.
> A FOMAKO NK20A não foi acessada.

## O que está implementado

### `hb::onvif`
| Arquivo | Conteúdo |
|---|---|
| `crypto` | SHA-1 e Base64 próprios (vetores FIPS/RFC 4648 e exemplo OASIS do UsernameToken). SHA-1 só existe porque o PasswordDigest do WS-Security o exige. |
| `soap` | Envelope SOAP 1.2, cabeçalho WS-Security UsernameToken (`PasswordDigest` = Base64(SHA1(nonce + created + senha)); modo `PasswordText` opcional), `SoapBuilder` com **offset de relógio** aplicado ao `Created`. Relógio e gerador de nonce são injetáveis. |
| `messages` | GetDeviceInformation, GetSystemDateAndTime (sem autenticação), GetCapabilities, GetServices, Media ver10 (GetProfiles, GetStreamUri), PTZ (ContinuousMove, Stop, GotoPreset, SetPreset, RemovePreset, GetPresets, GetStatus), Imaging (GetImagingSettings, Move de foco contínuo, Stop). Valores PTZ são limitados a [-1, 1] e formatados sem depender de locale. |
| `parsers` | Respostas para structs C++ (`types.hpp`). Usam pugixml e navegam por nome local, então prefixos/namespaces diferentes entre fabricantes funcionam. Campos opcionais ausentes viram vazio/`nullopt`; XML malformado, Fault ou campo obrigatório ausente viram `Error` tipado (`MalformedXml`, `Unauthorized`, `SoapFault`, `MissingField`...). Nenhuma exceção escapa; profundidade de busca limitada; corpo HTTP limitado a 4 MiB. |
| `onvif_client` | `OnvifClient::connect()`: GetSystemDateAndTime (calcula offset) -> GetDeviceInformation -> GetCapabilities (fallback GetServices) -> GetProfiles -> GetStreamUri. Escolhe o perfil **main** (maior resolução; empate: H.265/H.264 sobre JPEG, depois bitrate) e **sub** (menor resolução estritamente menor que a do main, preferindo H.264/H.265). Também expõe os comandos PTZ e Imaging individuais. Falha de relógio vira aviso (não fatal); 401/Fault `NotAuthorized` vira `ErrorCode::Unauthorized`. XAddrs/URIs com host `0.0.0.0`/loopback são reescritos para o host de conexão (`alwaysUseConnectHost` força sempre, útil atrás de NAT). |
| `http_client` | Interface `IHttpClient`. Reais: `http_client_posix.cpp` (HTTP puro, sem TLS) e `http_client_win.cpp` (WinHTTP, http/https). |

### `hb::discovery`
| Arquivo | Conteúdo |
|---|---|
| `ws_discovery` | Probe para `dn:NetworkVideoTransmitter`; parser de ProbeMatches (EndpointReference, Types, Scopes, XAddrs); interpretação de scopes (`name`, `hardware`, `location`, `Profile/*`, `type/ptz`, com decodificação `%20`); filtro por prefixo de scope; IP a partir dos XAddrs (evita 169.254.x.x quando possível); palpite heurístico de fabricante. |
| `onvif_source` | `OnvifDiscoverySource`: um socket por interface IPv4, probe reenviado `probeCount` vezes durante `listenTime`, ignora datagramas inválidos e respostas com `RelatesTo` de outro probe, uma interface que falha não derruba as outras. Relógio, sockets, lista de interfaces e MessageID são injetáveis. |
| `engine` | `IDiscoverySource` + `DiscoveryEngine`: fontes em paralelo, cada uma com timeout próprio e `CancelToken`; exceção, erro ou estouro de timeout ficam isolados e aparecem em `SourceReport`. Resultado deduplicado por `id` ou IP (`mergeDevices`) em `DiscoveredDevice {id, name, manufacturer, ip, protocols[], ptz, state}`. Dispositivos ONVIF saem como `RequiresCredentials`. |
| `udp_socket_*` | `IUdpMulticastSocket`: POSIX (`udp_socket_posix.cpp`) e Winsock (`udp_socket_win.cpp`). |

Exemplo de uso:
```cpp
hb::discovery::DiscoveryEngine engine;
engine.addSource(std::make_shared<hb::discovery::OnvifDiscoverySource>(), std::chrono::seconds(5));
auto report = engine.run();   // report.devices, report.sources

auto http = hb::onvif::makeDefaultHttpClient();
hb::onvif::ClientOptions opt;
opt.deviceServiceUrl = "http://192.168.1.64/onvif/device_service";
opt.credentials = {"admin", "senha"};
hb::onvif::OnvifClient cam(*http, opt);
auto info = cam.connect();     // info.value().main.stream.uri, .sub, .ptzSupported
cam.ptzContinuousMove(info.value().main.profile.token, 0.4, 0, 0, 1.0);
cam.ptzStop(info.value().main.profile.token);
```
Segredos: a senha nunca vai em claro no modo Digest e nunca é registrada; o logger opcional recebe apenas método, URL sem userinfo e a ação SOAP (e o `Logger` ainda aplica `redactSecrets`).

## O que NÃO foi validado

- **Nenhuma câmera real** (nem Hikvision, Dahua, Axis, nem a FOMAKO NK20A). As fixtures são reconstruções; firmwares reais têm variações que não conhecemos.
- **Windows**: `http_client_win.cpp` e `udp_socket_win.cpp` foram escritos sem compilador Windows e **não foram compilados nem executados**. O restante compila em GCC 13 (Linux); MSVC 2022 não foi testado.
- **Multicast em LAN real**: o código POSIX de UDP foi compilado e roda nos testes só com sockets falsos. O cliente HTTP POSIX foi verificado apenas contra um servidor Python em loopback (resposta com Content-Length; respostas `chunked` não foram exercitadas).
- **TLS/HTTPS**: o cliente POSIX não suporta; o WinHTTP suporta mas não foi testado (sem opção de ignorar certificado inválido, de propósito).
- **HTTP Digest/Basic**: só WS-Security. Câmeras que exigem autenticação HTTP no transporte (alguns firmwares Hikvision/Dahua com "ONVIF auth" configurado assim) falharão com 401 até implementarmos.
- **Media2 (ver20/media)** e **Profile T/H.265 via Media2**: só Media ver10. Câmeras apenas-Media2 reportarão `Unsupported`.
- **GetSystemDateAndTime**: usa só `UTCDateTime`. Câmeras que enviam apenas `LocalDateTime` + `TimeZone` POSIX não têm offset corrigido (viram aviso).
- Descoberta: apenas IPv4, sem WS-Discovery unicast para IP manual (previsto: probe ONVIF direto ao IP), sem `Hello`/`Bye` contínuos. A deduplicação cruzada entre fontes só trata correspondência direta de id/IP (sem cadeias). O fabricante é um palpite por texto do scope.
- PTZ: só velocidade normalizada genérica (ContinuousMove); sem AbsoluteMove/RelativeMove, sem espaços de coordenadas específicos, sem `GetConfigurationOptions`. O rate-limit de comandos e o "stop" por watchdog (ARCHITECTURE.md, seção 6) pertencem à camada PTZ Engine, não a este módulo.
- Imaging: apenas GetImagingSettings, Move (foco contínuo) e Stop; sem SetImagingSettings.

## Como testar com a FOMAKO NK20A (quando houver acesso)

1. Rede: câmera e PC na mesma sub-rede; anote IP, usuário e senha (crie um usuário ONVIF dedicado, se a interface web permitir) e confirme que ONVIF está habilitado.
2. Sincronize o relógio da câmera (NTP) ou confira se `clockSynced` é verdadeiro em `ConnectionInfo`: relógio errado é a causa mais comum de `NotAuthorized`.
3. **Descoberta**: rode `DiscoveryEngine` com `OnvifDiscoverySource` com Wireshark aberto (filtro `udp.port == 3702`). Verifique o Probe saindo na interface certa, o ProbeMatches de volta e se `scopes`/`XAddrs` fazem sentido. Se nada responder: firewall do Windows (UDP de entrada), Wi-Fi/VLAN bloqueando multicast, ou várias NICs.
4. **Conexão**: `OnvifClient::connect()` com `logger` ativo. Capture com Wireshark (`http.host == <ip>`) e **guarde os XMLs de resposta** de GetCapabilities/GetServices/GetProfiles/GetStreamUri: eles viram fixtures reais em `tests/` (anonimize serial/IP).
5. Confirme `main`/`sub`, abra as URIs RTSP em um player (VLC/ffplay) e anote se exigem credenciais na URL.
6. **PTZ**: com `ptzContinuousMove` pequeno (0.2) e timeout curto (1 s) seguido de `ptzStop`; depois `ptzGetStatus`, `ptzSetPreset`/`ptzGotoPreset`/`ptzGetPresets`/`ptzRemovePreset`. Registre quais falham (a NK20A pode exigir tokens de preset numéricos específicos ou ter só controle proprietário/VISCA; ver ARCHITECTURE.md, seção 6, **[VALIDAR NK20A]**).
7. Se 401 com `SOAP NotAuthorized` mesmo com relógio certo: tente `passwordType = PasswordText`; se ainda falhar, veja se a câmera pede autenticação HTTP Digest (limitação acima).
8. Só depois de executar os passos 3-6 com sucesso o item PTZ/ONVIF da NK20A pode ser marcado como validado.
