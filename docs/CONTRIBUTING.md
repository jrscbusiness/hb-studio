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
