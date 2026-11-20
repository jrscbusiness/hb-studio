# NDI (descoberta e recepção de câmeras na rede)

Módulo: `src/ndi/` (biblioteca `hb_ndi`), registrado em `cmake/modules/ndi.cmake`. Testes:
`build/hb_ndi_tests` (18 testes, sem rede, sem Runtime NDI instalado e sem hardware NDI real).

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
  (`ndi_backend_win.cpp`) — carregamento da DLL, resolução de símbolos via `GetProcAddress`, o
  layout exato dos structs da NDI e a conversão de formato de vídeo. Nada disso compila ou roda
  neste ambiente (Linux, sem o Windows SDK e sem o NDI Runtime instalado), então não há como
  afirmar que funciona contra hardware/software NDI real. Os valores numéricos usados (formato de
  cor, FourCCs, tipos de quadro) foram conferidos contra bindings `bindgen` publicados a partir dos
  headers reais da NDI (ver comentário no topo do arquivo) — incluindo uma checagem matemática
  independente do empacotamento de FourCC documentado pela própria NDI —, mas isso é confirmação de
  dados públicos, não um teste de execução.
- **Limitação conhecida:** nenhuma fonte NDI real, nenhuma instalação do NDI Runtime e nenhuma
  máquina Windows estavam disponíveis para validar o backend real ponta a ponta. A primeira pessoa
  a rodar o HB Studio num Windows com o NDI Runtime instalado e uma fonte NDI real na rede é, na
  prática, o primeiro teste de integração deste backend — qualquer divergência de ABI entre o que
  foi reconstruído aqui e uma versão futura/diferente da NDI apareceria ali, não antes.
- Compilação 32 bits (`Processing.NDI.Lib.x86.dll`) não é suportada: o backend real assume x64,
  consistente com o resto do aplicativo Windows do HB Studio.
