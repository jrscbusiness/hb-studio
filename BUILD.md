# Build

Requisitos: CMake ≥ 3.24, compilador C++20 (GCC 13 / Clang / MSVC 2022), acesso à internet
(GoogleTest é baixado via FetchContent).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # 35+ testes
build/hb_probe 3 1080p30                     # smoke test headless
```
Opções: `-DHB_BUILD_TESTS=OFF`, `-DHB_BUILD_TOOLS=OFF`.
Verificação de races: `-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` (Linux/Clang/GCC).

Estado: verificado apenas em Linux (GCC 13). Windows 11/MSVC ainda não testado.
