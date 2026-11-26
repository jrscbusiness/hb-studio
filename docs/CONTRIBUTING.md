# Convenções do projeto (leia antes de contribuir)

HB Studio é C++20, `namespace hb::<modulo>`, código e comentários em **inglês**, documentação em **português (pt-BR)**.

## Estrutura de um módulo
```
src/<modulo>/          headers + .cpp  (include como "<modulo>/arquivo.hpp")
tests/<modulo>_*.cpp   testes GoogleTest
cmake/modules/<modulo>.cmake   registra a biblioteca e os testes do módulo
docs/<MODULO>.md       documentação (pt-BR)
```
Exemplo de `cmake/modules/exemplo.cmake`:
```cmake
add_library(hb_exemplo STATIC src/exemplo/a.cpp src/exemplo/b.cpp)
target_include_directories(hb_exemplo PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(hb_exemplo PUBLIC hb_core)
if(HB_BUILD_TESTS)
  add_executable(hb_exemplo_tests tests/exemplo_a_test.cpp)
  target_link_libraries(hb_exemplo_tests PRIVATE hb_exemplo GTest::gtest_main)
  gtest_discover_tests(hb_exemplo_tests)
endif()
```
Não edite `CMakeLists.txt` raiz, `src/core`, `src/video`, `src/switcher`, `src/app` nem `src/virtual-camera` sem necessidade — use apenas o seu módulo.

## Regras
1. **Compila em GCC (Linux) e MSVC 2022 (Windows)**: sem extensões de compilador, inclua os headers que usa, sem `ssize_t`/`unistd.h` fora de `#ifdef`. Código de plataforma (sockets, WinHTTP…) fica atrás de uma interface e em arquivos/`#ifdef` separados; a lógica pura (parsers, builders, máquinas de estado) deve ser testável **sem rede e sem hardware**.
2. **Isolamento de falhas**: nenhuma entrada externa (rede, XML, pacote) pode derrubar o processo: valide tamanhos, trate exceções nas fronteiras, nunca confie em comprimentos vindos da rede.
3. **Segredos**: nunca registre senhas/tokens (use `hb::core::redactSecrets` / `Logger`, que já redige).
4. **Tempo injetável**: lógica com timeouts/backoff recebe um relógio ou `time_point` por parâmetro para testes determinísticos (sem `sleep` longo nos testes; < 2 s por teste).
5. **Honestidade**: o que não foi validado em hardware real é documentado como *não validado*. Não afirme compatibilidade com um equipamento sem fonte/teste.
6. Dependências externas só via `FetchContent` com versão fixada e licença compatível (MIT/BSD/Apache/MPL/LGPL dinâmica). Liste em `LICENSES.md`.

## Build e testes
```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## Interface (UI): `ui/src/` -> `ui/index.html`
A interface é **um único arquivo** embutido no executável (`ui/index.html`), mas ele agora é um **artefato gerado**. Edite sempre os arquivos em `ui/src/`, rode o montador e **faça commit dos dois** (fonte e `ui/index.html`):
```
python3 -I tools/build_ui.py            # regenera ui/index.html
python3 -I tools/build_ui.py --check    # falha se ui/index.html != o que ui/src/ produz (roda no CI, job Linux)
```
Nunca edite `ui/index.html` à mão: a próxima montagem apaga a alteração e o CI reprova.

Organização de `ui/src/`:
```
index.template.html   casca do documento; linhas `<!-- @include caminho -->` (coluna 0) são trocadas pelo arquivo
css/*.css             um arquivo por área/feature (theme, layout, inputs, cameras, audio, rec, xmit, chat, ...)
views/*.html          uma parte por aba (tab-*.html), modal/menu (modal-*.html, menu-*.html); tab-cfg.html inclui views/cfg/*.html (um card por feature)
js/*.js               um arquivo por feature (core.js = ponte/constantes/DOM base; dispatch.js = onMsg, o despachante de mensagens do host)
fonts/*.woff2         fontes binárias; `@base64(fonts/x.woff2)` no CSS é trocado pelo base64
```
Regras importantes:
- **A ordem dos `@include` no template É a ordem de execução.** CSS: ordem da cascata (arquivos posteriores vencem em empate de especificidade). JS: todo `js/*.js` é colado dentro da **mesma IIFE** (mesmo escopo), de cima para baixo; um arquivo pode usar `const`/`function` definidos nos anteriores (e `function`s são içadas), mas não use um `const` de um arquivo que vem depois antes de ele executar.
- Cada parte termina com exatamente uma quebra de linha (LF). Sem CRLF.
- Arquivo em `css/`, `views/`, `js/` ou `fonts/` que não é referenciado por nenhum `@include`/`@base64` **quebra a montagem** (evita feature "esquecida").
- Os testes de UI (`tools/ui/ui_test.js`) e o CMake continuam usando `ui/index.html`, sem mudanças.

### Como adicionar uma aba ou feature nova
1. Crie `css/minha-feature.css`, `views/tab-minha-feature.html` (a `<section class="view" id="v-...">`), e `js/minha-feature.js` (código que roda dentro da IIFE).
2. Registre cada um com uma linha `<!-- @include ... -->` no `index.template.html`: CSS antes de `</style>`, a view dentro de `<main>`, o JS antes de `js/dispatch.js`. Adicione o botão da aba em `views/header.html` e o ramo `d.t === '...'` em `js/dispatch.js`.
3. Para um card dentro de CONFIG, crie `views/cfg/meu-card.html` e inclua-o em `views/tab-cfg.html`.
4. `python3 -I tools/build_ui.py`, rode `tools/ui/ui_test.js` e faça commit de `ui/src/` **e** `ui/index.html`.

Conflito de merge em `ui/index.html`? Resolva em `ui/src/` e simplesmente regenere o arquivo (`git checkout --theirs ui/index.html` ou qualquer lado, depois `python3 -I tools/build_ui.py`).

## Código Windows (`src/app/windows/*`): como validar sem Windows
O CI do Windows é o verificador final, mas dá para pegar a maioria dos erros bobos (namespace errado, `#include` faltando, uso antes da declaração, lambda *move-only* num `std::function`) localmente:
```
sudo apt-get install -y g++-mingw-w64-x86-64
tools/check_windows_syntax.sh                      # roda g++ -fsyntax-only (mingw) nos .cpp de Windows verificáveis
```
O script lista o que **não** dá para checar (WebView2/WRL, câmera virtual do Media Foundation, SDK do VST3).

Lógica nova do shell do app deve ir para código **portátil e testado**: `src/app/command_router.hpp` (parser `cmd|a|b` e roteador de comandos simples: `router.add("nome", handler, minPartes, maxPartes)` em `app_main.cpp`), `src/app/state_json.*` (JSON do estado enviado à UI), `src/app/json_escape.hpp`. Caminhos em `%LOCALAPPDATA%`: use `win::localAppDataPath()`.
