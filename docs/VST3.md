# Hospedagem de plugins VST3

Código em `src/vst3/` (biblioteca `hb_vst3`). Contrato congelado: `src/vst3/vst3.hpp` (não alterar —
é a especificação completa de `scanPlugins()`/`typeIdFor()`/`createEffect()`/`IVst3Editor`). O
efeito retornado por `createEffect()` implementa `hb::audio::Effect` (`src/audio/effect.hpp`, também
congelado) como qualquer outro efeito do projeto, e entra no `EffectRegistry`/`AudioEngine` do mesmo
jeito que os efeitos embutidos (`src/audio/effects/`) e os efeitos de plugin nativo C ABI
(`src/audio/plugin_effect.hpp`) — **este documento descreve só o módulo `src/vst3/` em si; ligar a
descoberta/criação na UI e no `AudioEngine` é uma integração separada**, feita por outro trabalho.

Só VST3 é suportado (VST2 é descontinuado pela Steinberg e não é mais licenciável para hosts novos).
O SDK oficial da Steinberg (`https://github.com/steinbergmedia/vst3sdk`, tag `v3.8.1_build_84`) é
baixado via `FetchContent` em `cmake/modules/vst3.cmake`; desde a versão 3.8 ele é inteiramente
licenciado em MIT (conferido no `LICENSE.txt` do próprio repositório), sem necessidade de cadastro
na Steinberg. A única coisa que o MIT não cobre é o nome/logo "VST", que é marca registrada separada
— ver `LICENSES.md` do projeto.

## Como funciona a descoberta

`scanPlugins(extraFolders)` varre, além das pastas em `extraFolders`:

- Windows: `%COMMONPROGRAMFILES%\VST3` (instalação para todos os usuários) e
  `%LOCALAPPDATA%\Programs\Common\VST3` (instalação só para o usuário atual) — os dois caminhos
  padrão documentados pela Steinberg para plugins VST3 no Windows.
- Em qualquer outra plataforma essas duas variáveis de ambiente simplesmente não existem, então só
  `extraFolders` é usado — é assim que os testes deste módulo rodam em Linux, apontando para uma
  pasta com o plugin de teste (abaixo).

A varredura é recursiva (plugins às vezes ficam em subpastas por fabricante) mas nunca entra dentro
de uma pasta `.vst3` (um "bundle" é tratado como uma unidade, nunca como uma pasta normal a
explorar). Cada módulo `.vst3` encontrado é carregado só o tempo necessário para ler a lista de
classes do seu `IPluginFactory` (via `VST3::Hosting::Module::create()`, a classe de apoio que a
própria Steinberg fornece em `public.sdk/source/vst/hosting/` para quem escreve hosts) e descarregado
de novo em seguida — a varredura nunca deixa nada residente. Um módulo que falha ao carregar, não é
um módulo VST3 de verdade, ou lança uma exceção ao ser inspecionado é simplesmente pulado; a
varredura nunca é interrompida por um módulo ruim.

Cada classe de categoria `kVstAudioEffectClass` (um efeito de áudio — instrumentos e outras
categorias são ignorados) vira um `Vst3PluginInfo`. `typeIdFor()` gera um id estável
(`"vst3:<32 hex minúsculos>"`) a partir do `classId` da classe, usado pelo `EffectRegistry` e salvo
na cadeia de efeitos de um canal — estável entre reinicializações desde que o mesmo módulo continue
instalado no mesmo caminho e exportando a mesma classe.

## Onde o operador instala plugins VST3

Nas pastas padrão do Windows acima (a instalação normal de qualquer plugin VST3 comercial já
escreve lá) ou em qualquer pasta extra que a UI do HB Studio ofereça adicionar (mecanismo de
`extraFolders` — a integração da UI com isso está fora do escopo deste módulo, ver nota no topo).

## Processamento de áudio (portável)

`createEffect()` carrega o módulo, localiza a classe pelo `classId`, e usa
`Steinberg::Vst::PlugProvider` (outra classe de apoio da própria Steinberg, em
`public.sdk/source/vst/hosting/plugprovider.{h,cpp}`, a mesma usada pelo validador oficial do SDK)
para criar e inicializar o `IComponent`/`IEditController` do plugin — ela já trata os dois casos
documentados pela Steinberg: plugin com uma única classe que implementa `IComponent` e
`IEditController` ao mesmo tempo, e plugin com duas classes separadas conectadas via
`IConnectionPoint` (o caso mais comum). O `IAudioProcessor` é obtido por `queryInterface` a partir
do componente.

A ativação (`Vst3Effect::prepare()`) ativa os busses de áudio de entrada/saída (bus principal,
índice 0), negocia a contagem de canais pedida via `IAudioProcessor::setBusArrangements()` e confere
com `IComponent::getBusInfo()` que o plugin realmente aceitou (se não aceitou a contagem de canais
pedida, a ativação falha com um erro em pt-BR em vez de processar incorretamente), chama
`setupProcessing()`/`setActive(true)`/`setProcessing(true)`. `createEffect()` chama `prepare(48000,
2)` como teste de fumaça logo após criar o efeito — exatamente como o comentário de `vst3.hpp`
descreve — para nunca registrar um plugin morto; o chamador real prepara de novo com a taxa/canais
da sessão antes de usar.

`Effect::process()` converte o buffer intercalado (formato de todo o resto do projeto) para o
formato planar (não intercalado) que `IAudioProcessor::process()` exige, usando buffers de
"scratch" alocados uma vez em `prepare()` (nunca dentro de `process()`) — blocos maiores que 4096
amostras (`Vst3Effect::kMaxBlockFrames`) são processados em pedaços, sem alocação. Todo acesso direto
ao plugin (processar, ativar, criar o editor) é protegido contra exceção C++, seguindo o mesmo
padrão de `src/audio/plugin_effect.cpp`/`src/plugins/native_source.hpp`: depois de 8 blocos ruins
seguidos (lançou exceção, devolveu erro, ou produziu `NaN`/`Inf`) o efeito se autodesliga e passa o
áudio adiante sem tocar (igual ao adaptador de plugin C ABI) até ser preparado de novo.

`params()` de um efeito VST3 é **sempre vazio**, por decisão de produto: o plugin mostra a própria
interface gráfica (abaixo), não uma lista de sliders genéricos do HB Studio. O único controle que a
UI do HB Studio oferece diretamente é o bypass que todo efeito já tem — isso é feito inteiramente
pelo `AudioEngine`/`EffectRegistry` (o `slot` que embrulha cada efeito já checa o bypass antes de
chamar `process()`, ver `src/audio_io/audio_engine.cpp`), sem nada específico de VST3.

## Plugin de teste (fixture)

Não há nenhum plugin VST3 de terceiro disponível neste ambiente (sem licença, sem acesso à
instalação de um DAW real). `plugins/examples/vst3-test-fixture/` é um plugin VST3 **mínimo escrito
para este projeto** (não é um plugin comercial/real): ganho fixo de -6 dB (`x0,5`), sem parâmetros,
sem editor gráfico — o suficiente para testar a descoberta e o processamento de áudio de ponta a
ponta. Ele é compilado como um alvo de teste (`hb_vst3_test_fixture`, só quando `HB_BUILD_TESTS`
está ligado) via o próprio mecanismo de empacotamento do SDK da Steinberg
(`smtg_add_vst3plugin`, que monta a estrutura `Contents/<arquitetura>/<nome>.so` no Linux — mesma
estrutura que `VST3::Hosting::Module::create()` espera ao carregar), nunca instalado nem
empacotado com o aplicativo final. Ver o `README.md` dentro dessa pasta.

Como o `IEditController` do fixture é a classe `EditController` do próprio SDK, sem modificação
(`createView()` dela devolve `nullptr`), **não há como testar a janela do editor nativo
(`IVst3Editor`) neste ambiente** — nem com um plugin real (que não existe aqui) nem com o fixture
(que deliberadamente não tem editor). Ver a seção abaixo.

## Editor nativo (Windows apenas) — NÃO TESTADO

`src/vst3/windows/vst3_editor.cpp` implementa `IVst3Editor`: uma janela `HWND` de nível superior
simples (sem WebView2, sem nada do resto da UI do aplicativo), criada na thread de controle/UI
(nunca na thread de áudio, como o contrato exige). Ela não roda seu próprio laço de mensagens — o
aplicativo já tem um (é um app WebView2), e qualquer `HWND` criado nessa mesma thread já recebe
mensagens por esse laço automaticamente, igual a qualquer outra janela do aplicativo.

- `openEditor()` cria a view do plugin (`IEditController::createView(kEditor)`), confere que ela
  suporta `kPlatformTypeHWND`, chama `IPlugView::attached()`, e redimensiona a janela para o
  tamanho que o plugin reportar.
- Um `IPlugFrame` mínimo (`EditorFrame`) reage a `resizeView()` (quando o próprio plugin pede para
  mudar de tamanho) redimensionando o `HWND` e repassando `onSize()` de volta à view, conforme o
  contrato documentado em `pluginterfaces/gui/iplugview.h`.
- `closeEditor()` chama `view->setFrame(nullptr)` e `view->removed()` antes de destruir o `HWND`,
  como o contrato pede; o destrutor de `Vst3Effect` chama `closeEditor()` antes de desativar o
  plugin, então a janela nunca fica referenciando um componente já destruído.

**Este código nunca foi compilado nem executado neste ambiente** — não há toolchain do Windows nem
um plugin VST3 real com editor disponível aqui. Ele foi escrito com cuidado contra a documentação
oficial da interface `IPlugView`/`IPlugFrame` e seguindo o padrão do exemplo oficial da Steinberg
(`public.sdk/samples/vst-hosting/editorhost`), mas é **só raciocinado a partir da documentação**:
só o CI do Windows (compilação) e, depois, um operador com Windows 11 real e um plugin VST3 com
editor (testes manuais) podem confirmar que funciona de verdade. Qualquer problema de
dimensionamento, DPI, ou comportamento de foco que só aparece na prática não foi — e não podia ter
sido — verificado aqui.

## Limitações conhecidas

- Só o bus de áudio principal (índice 0) de entrada e de saída é ativado; busses auxiliares/side
  chain não são suportados.
- Se o plugin não aceitar a contagem exata de canais pedida (`IComponent::getBusInfo()` depois de
  `setBusArrangements()` não bate), a ativação falha com erro em vez de tentar adaptar — mais simples
  e mais seguro do que silenciosamente processar com uma contagem de canais diferente da pedida, mas
  significa que plugins que insistem numa configuração de canais fixa e diferente da sessão não vão
  funcionar.
- Nenhuma automação de parâmetro é enviada ao plugin por bloco (`inputParameterChanges` é sempre
  vazio) — consistente com a decisão de produto de não expor os parâmetros do plugin na UI do HB
  Studio, mas quer dizer que um plugin que depende de receber mudanças de parâmetro via
  `IParameterChanges` em vez de só UI/estado interno não vai se comportar como em outro host.
  `setParam()`/`getParam()` do `Effect` são sempre no-ops.
- Nenhuma informação real de transporte/tempo é passada (`ProcessContext` vai sempre com `state=0`);
  plugins que dependem de tempo/BPM do host (delays sincronizados, etc.) não vão funcionar
  corretamente.
- Um plugin que trava ou corrompe memória roda no mesmo processo do HB Studio e não pode ser
  totalmente contido — mesma postura de segurança dos outros plugins nativos hospedados pelo
  projeto (ver o modelo de segurança em `docs/PLUGINS.md`): só instale plugins de autores confiáveis.
- O editor nativo (`IVst3Editor`, Windows) nunca foi exercitado com um plugin de verdade nem
  compilado neste ambiente — ver seção acima.
- O plugin de teste (fixture) não tem parâmetros nem editor, então o caminho de
  "negociação de contagem de canais diferente da pedida" e o editor gráfico em si não têm cobertura
  de teste automatizado neste módulo (só raciocínio + a implementação espelhando o comportamento
  documentado pela Steinberg).

## Testes

`tests/vst3_scan_test.cpp` e `tests/vst3_effect_test.cpp` (alvo `hb_vst3_tests`, roda em
`ctest`) cobrem, de ponta a ponta contra o plugin de teste real acima (compilado e carregado de
verdade, não simulado):

- `scanPlugins()` encontra a classe do fixture, com os campos corretos (`name`, `vendor`,
  `category`, `classId` em hex minúsculo de 32 caracteres), ignora uma pasta inexistente e um
  arquivo `.vst3` inválido, e é aditivo com `extraFolders`.
- `typeIdFor()` é estável entre duas varreduras.
- `createEffect()` com módulo inexistente ou `classId` desconhecido devolve `nullptr` com erro em
  vez de lançar exceção/travar.
- `createEffect()` bem-sucedido devolve um `Effect` com `params()` vazio e `typeId()` correto.
- Processamento aplica o ganho fixo corretamente (estéreo, mono via renegociação de canais, e um
  bloco maior que o tamanho interno de "chunk" para exercitar o laço de múltiplos pedaços).
- `reset()` não trava e o efeito continua processando depois.

Tudo isso roda limpo sob AddressSanitizer + UndefinedBehaviorSanitizer (sem vazamentos, sem UB
relatado). O que **não** foi testado de verdade (só implementado com cuidado a partir da
documentação): o editor nativo Windows (seção acima) e o caminho de uma contagem de canais rejeitada
pelo plugin (o fixture aceita qualquer contagem).
