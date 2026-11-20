# NDI (descoberta/recepção de câmeras e envio do Program como fonte NDI)

Módulo: `src/ndi/` (biblioteca `hb_ndi`), registrado em `cmake/modules/ndi.cmake`. Testes:
`build/hb_ndi_tests` (23 testes — 18 de descoberta/recepção + 5 de `NdiSender`, sem rede, sem
Runtime NDI instalado e sem hardware NDI real).

> **Estado de validação: nada aqui foi testado contra o NDI Runtime real nem contra uma câmera/fonte
> NDI real.** Todos os testes usam um `FakeNdiBackend` em processo (`tests/ndi_test_util.hpp`), sem
> rede. O backend real do Windows (`src/ndi/ndi_backend_win.cpp`) nem compila neste ambiente de CI
> (ele só entra no build quando `WIN32` está definido) — foi escrito e revisado a partir da
> documentação pública da NDI, nunca executado. Veja "Limitações conhecidas" no fim deste documento.

## Por que nunca há o SDK/header da NDI no repositório

A licença da NDI não permite redistribuir o SDK/Runtime. Em vez de vendorizar o header oficial, o
`src/ndi/ndi_backend_win.cpp` reconstrói apenas a parte da ABI binária da NDI que o módulo precisa
(layout de structs, assinaturas de função, nomes de símbolos exportados pela DLL) a partir da
documentação pública da NDI (docs.ndi.video) e cruzando com bindings open-source gerados por
ferramentas automáticas (`bindgen`/`libloading`) direto contra os headers reais, como `ndi-sys` e
`gst-plugin-ndi` — usados só como referência de ABI (nomes, ordem de campos, valores numéricos),
nunca como código copiado. Isso é o mesmo modelo documentado pela própria NDI para "dynamic
loading": qualquer programa pode carregar a DLL do Runtime em tempo de execução e chamar as funções
pelo nome, sem precisar linkar contra o SDK em tempo de build. Detalhes e fontes consultadas estão
comentados no topo de `ndi_backend_win.cpp`. Ver também `LICENSES.md`.

## Como funciona a descoberta

1. `hb::ndi::NdiDiscovery` guarda a lista de fontes NDI vistas na rede. **Não faz nenhuma chamada
   de rede até a primeira vez que alguém chama `sources()`** (ex.: a aba CÂMERAS é aberta pela
   primeira vez) — depois disso, uma thread de fundo cria um "find" do backend uma única vez e
   relê `findCurrentSources()` a cada ~1 segundo, pelo resto da vida do `NdiDiscovery`.
2. Se o `find` falhar na criação (ex.: Runtime não carregado), a thread continua tentando a cada
   poll — se o Runtime for instalado depois, a detecção começa a funcionar sem reiniciar o app.
3. `NdiDiscovery::available()` reflete `NdiBackend::available()`; quando `false`,
   `unavailableMessage()` devolve um texto pt-BR pronto para a UI ("Instale o NDI Runtime em
   ndi.video/tools...").
4. Cada fonte (`NdiSourceInfo{name, urlAddress}`) é só um nome + um endereço opaco do NDI; a UI
   nunca precisa entender o endereço, só repassá-lo de volta ao criar um `NdiSource`.

## Como funciona a recepção (`NdiSource`)

`NdiSource` implementa `hb::video::LiveSource` (a mesma interface de `DShowSource` e
`PluginSource`): uma thread própria chama `recvCreate` uma vez e depois `recvCaptureVideo` em loop
(timeout de 150 ms por chamada, para reagir rápido a `stop()`), guardando o quadro mais recente
atrás de um mutex como `shared_ptr<const video::Frame>`. `render()` nunca toca a rede: ele só copia
o ponteiro do quadro mais recente e escala (letterbox) para o tamanho pedido — ou desenha um
quadro escuro de "sem sinal" se não há quadro recente (> 1,5 s) ou nenhum ainda.

A conversão de formato (UYVY/RGBA/BGRX/... para BGRA8) é responsabilidade do **backend**, não do
`NdiSource` — é por isso que o backend real pede explicitamente o formato de cor
`NDIlib_recv_color_format_BGRX_BGRA` à NDI (BGRA quando há canal alfa, BGRX quando não há) e ainda
assim sabe converter os formatos mais comuns (BGRA/BGRX/RGBA/RGBX/UYVY) defensivamente, caso a NDI
entregue algo diferente do pedido.

Estados de `status()` (mesmos quatro de todo `LiveSource`):
- **Connecting**: thread ainda não conseguiu criar o receptor, ou já conseguiu mas ainda não
  recebeu o primeiro quadro (até ~3 s).
- **Online**: há um quadro com menos de 1,5 s.
- **NoSignal**: receptor criado, mas sem quadro novo há mais de 1,5 s (ou nunca recebeu um, após os
  3 s iniciais) — por exemplo, a fonte NDI foi desligada do lado de lá.
- **Error**: o receptor não pôde ser criado (fonte não existe mais / Runtime indisponível), ou o
  backend reportou uma falha irrecuperável durante a captura. É um estado terminal: a thread para
  de chamar o backend (chame `stop()`/`start()` para tentar de novo).

`start()`/`stop()` são idempotentes (chamar duas vezes não cria uma segunda thread nem trava) e
`render()`/`status()` nunca lançam exceção nem bloqueiam por mais que o tempo de um `lock_guard`,
mesmo que o backend se comporte mal (timeout constante, erro constante, tamanhos de quadro que não
batem com o buffer entregue — tudo isso é validado e descartado antes de tocar a memória do quadro).

## Como funciona o envio (`NdiSender`) — publicar o Program como fonte NDI

`NdiSender` (`src/ndi/sender.cpp`, não tocado por esta mudança) é o caminho inverso de `NdiSource`:
em vez de consumir uma fonte NDI da rede, ele publica este processo *como* uma fonte NDI, para que
qualquer receptor NDI na LAN (incluindo outra cópia do HB Studio) possa descobri-la e se conectar —
mesma `NdiSourceInfo`/mesmo caminho de descoberta de qualquer outra fonte.

`feed()` é pensado para ser chamado diretamente da própria thread que já produz o Program Output
(o mesmo padrão que a câmera virtual e o gravador já usam para consumir frames do Program): o
handle de envio é criado "sob demanda" na primeira chamada real (não no construtor), e `feed()`
nunca bloqueia por mais tempo do que o próprio envio da NDI já leva — ver abaixo por que a chamada
ao backend é síncrona e, ainda assim, atende a essa exigência.

No backend real do Windows (`ndi_backend_win.cpp`), `sendCreate`/`sendDestroy`/`sendVideo` resolvem
três símbolos adicionais por nome (mesmo padrão de `GetProcAddress` do lado de recepção):
`NDIlib_send_create`, `NDIlib_send_destroy`, `NDIlib_send_send_video_v2`. Decisões relevantes,
documentadas com a URL da fonte no topo do próprio arquivo:

- **FourCC usado para enviar: BGRA nativo.** `video::Frame`/o buffer que `sendVideo` recebe já é
  BGRA8 top-down (mesmo formato interno usado em todo o resto do projeto), e a NDI tem suporte
  nativo a esse FourCC — nenhuma conversão é necessária no caminho de envio (diferente da recepção,
  que converte de volta para BGRA a partir do que a câmera/fonte remota entregar).
- **Chamada síncrona (`NDIlib_send_send_video_v2`), não assíncrona.** A documentação pública da NDI
  descreve a variante assíncrona (`NDIlib_send_send_video_async_v2`) como exigindo que o buffer
  passado permaneça válido até a *próxima* chamada de envio (ou uma sincronização explícita) — ou
  seja, a NDI pode continuar lendo esse ponteiro em segundo plano depois que a função retorna. O
  contrato de `NdiSender::feed()`/`NdiBackend::sendVideo` só garante que o buffer é válido durante
  a própria chamada (é o buffer do Program Output da chamada atual, que pode ser reescrito/trocado
  logo depois). Usar a versão assíncrona arriscaria um use-after-free real quando o próximo quadro
  do Program sobrescrevesse esse buffer enquanto a NDI ainda o lê. A versão síncrona, por outro
  lado, é documentada como só retornando depois que a NDI terminou de usar o buffer daquela
  chamada — exatamente o tempo de vida que este código tem para oferecer. Isso é consistente com o
  texto de `ndi.hpp` ("não deve bloquear além do que o próprio envio assíncrono da NDI já faz
  internamente"): a leitura adotada aqui é que esse texto permite o bloqueio inerente ao próprio
  envio de rede da NDI (que tanto a versão síncrona quanto a assíncrona fazem, de formas
  diferentes), não que exige literalmente chamar a API assíncrona da NDI — e a escolha segura para
  o tempo de vida do buffer prevalece sobre a escolha "não bloqueante por fora".
- **`clock_video = false` na criação do sender.** A NDI pode, opcionalmente, pausar dentro de
  `NDIlib_send_send_video_v2` para "ritmar" o envio na taxa de quadros declarada
  (`frame_rate_N`/`frame_rate_D`) quando `clock_video = true`. Isso foi deliberadamente desligado:
  é quem decide a cadência de `feed()` (a própria thread do Program Output) que já sabe a que
  taxa está rodando, e uma pausa adicional dentro do backend seria um bloqueio que o chamador não
  pediu.
- **`sendOk_` separado de `ok_`.** A disponibilidade de envio é resolvida/guardada
  independentemente da disponibilidade de recepção/descoberta (mesmo padrão de resolução por
  símbolo, mas uma flag própria). Na prática todo NDI Runtime público exporta os dois conjuntos de
  símbolos juntos, mas a interface `NdiBackend` não promete isso — manter os dois caminhos
  independentes custa nada e deixa o backend mais robusto a essa hipótese, sem mudar nenhum
  comportamento observável quando (como é o caso normal) ambos os conjuntos de símbolos existem.
  Limitação conhecida desse detalhe: a fábrica `defaultBackend()` ainda decide "Runtime disponível
  ou não" olhando só `available()` (que reflete `ok_`, i.e. recepção) — se um Runtime hipotético só
  tivesse os símbolos de envio e não os de recepção, `defaultBackend()` devolveria o backend
  indisponível mesmo assim, perdendo também a capacidade de envio. Não é um cenário realista hoje
  (nunca visto em nenhum Runtime NDI publicado), por isso não foi resolvido aqui — ficaria mais
  claro resolver isso junto da ligação na UI (`app_main.cpp`), que está fora do escopo desta
  mudança.

## O que acontece sem o NDI Runtime instalado

Fora do Windows, ou no Windows sem o Runtime instalado, `defaultBackend()` devolve um backend cujo
`available()` é sempre `false` e todo o resto é um no-op seguro (handle nulo, lista vazia, timeout
sem erro) — nunca um crash ou um ponteiro nulo sendo desreferenciado mais adiante.
`NdiDiscovery::unavailableMessage()` e o estado `Error` de qualquer `NdiSource` criado nesse caso
trazem a mesma orientação: baixar o NDI Runtime gratuito em **ndi.video/tools**. O resto do HB
Studio funciona normalmente; nenhuma outra funcionalidade depende do NDI.

Quando o Runtime está instalado, o backend real do Windows o localiza, nesta ordem:
1. Variável de ambiente `NDI_RUNTIME_DIR_V6` (o instalador do NDI 6 a define).
2. Variável de ambiente `NDI_RUNTIME_DIR_V5` (compatibilidade com instalações mais antigas; o
   instalador do NDI 6 também costuma definir as duas apontando para o mesmo lugar).
3. Pastas padrão de instalação (`%ProgramFiles%\NDI\NDI 6 Runtime\v6\...`,
   `%ProgramFiles%\NDI\NDI 5 Runtime\v5\...` e variações), como última tentativa antes de deixar o
   Windows procurar a DLL pelo `PATH` normal.

Em nenhum desses casos a DLL do Runtime é copiada para dentro do HB Studio nem redistribuída.

## Limitações conhecidas / o que foi testado de verdade

- **Testado de verdade (roda neste CI, em Linux e deve rodar igual no Windows):** toda a lógica de
  `NdiDiscovery` e `NdiSource` — polling, transições de estado, `start()`/`stop()` idempotentes,
  ausência de vazamento de thread, e robustez a um backend hostil (timeout constante, erro
  constante, tamanhos de quadro inconsistentes entre chamadas) — usando `FakeNdiBackend`
  (`tests/ndi_test_util.hpp`), inteiramente em processo. Essa é a parte independente de plataforma
  e de hardware, e é a maior parte do módulo.
- **Só raciocinado a partir da documentação pública, nunca executado:** o backend real
  (`ndi_backend_win.cpp`), tanto o lado de recepção quanto o de envio (`sendCreate`/`sendDestroy`/
  `sendVideo`, adicionados nesta mudança) — carregamento da DLL, resolução de símbolos via
  `GetProcAddress`, o layout exato dos structs da NDI (incluindo `NDIlib_send_create_t`, novo) e a
  conversão de formato de vídeo. Nada disso compila ou roda neste ambiente (Linux, sem o Windows
  SDK e sem o NDI Runtime instalado), então não há como afirmar que funciona contra hardware/
  software NDI real — isso vale igualmente para o envio: nenhum receptor NDI real (outra cópia do
  HB Studio, OBS, um monitor de forma de onda, etc.) nunca viu um quadro publicado por este código.
  Os valores numéricos usados (formato de cor, FourCCs, tipos de quadro, e agora também
  `NDIlib_frame_format_type_progressive`/`NDIlib_send_timecode_synthesize`) foram conferidos contra
  bindings `bindgen` publicados a partir dos headers reais da NDI (ver comentário no topo do
  arquivo) — incluindo uma checagem matemática independente do empacotamento de FourCC documentado
  pela própria NDI —, mas isso é confirmação de dados públicos, não um teste de execução. A escolha
  de usar a chamada de envio síncrona (`NDIlib_send_send_video_v2`) em vez da assíncrona, pelo
  motivo de tempo de vida do buffer explicado acima, também é só raciocínio a partir da
  documentação — nunca foi medido se isso tem algum custo de desempenho perceptível em produção
  (ex.: travar a thread do Program Output por um tempo mensurável em uma rede congestionada); se
  isso vier a ser um problema real, a opção assíncrona exigiria primeiro garantir, na camada acima
  (`NdiSender`/quem chama `feed()`), que o buffer do Program permaneça válido até a chamada
  seguinte — não é uma troca segura de se fazer só dentro do backend.
- **Limitação conhecida:** nenhuma fonte NDI real, nenhuma instalação do NDI Runtime e nenhuma
  máquina Windows estavam disponíveis para validar o backend real ponta a ponta. A primeira pessoa
  a rodar o HB Studio num Windows com o NDI Runtime instalado e uma fonte NDI real na rede é, na
  prática, o primeiro teste de integração deste backend — qualquer divergência de ABI entre o que
  foi reconstruído aqui e uma versão futura/diferente da NDI apareceria ali, não antes.
- Compilação 32 bits (`Processing.NDI.Lib.x86.dll`) não é suportada: o backend real assume x64,
  consistente com o resto do aplicativo Windows do HB Studio.
