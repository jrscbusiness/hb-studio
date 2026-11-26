# Build

Requisitos: CMake ≥ 3.24, compilador C++20 (GCC 13 / Clang / MSVC 2022), Git e acesso à internet (as
dependências são baixadas via FetchContent na primeira configuração: GoogleTest, pugixml, miniz,
miniaudio, SDK VST3, whisper.cpp — versões fixadas, lista completa em [LICENSES.md](LICENSES.md)).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure -j 4   # ver nota abaixo sobre -j
build/hb_probe 3 1080p30                          # smoke test headless
```

No Windows (gerador Visual Studio) acrescente `--config Release` ao build e `-C Release` ao `ctest`;
os executáveis saem em `build/Release/`.

> **Nota sobre `ctest -j`:** alguns testes usam nomes fixos de pasta temporária, então, se um teste
> falhar em paralelo, rode `ctest --test-dir build --rerun-failed --output-on-failure` (serial) antes
> de investigar. O CI faz exatamente isso.

## Opções do CMake

| Opção | Padrão | Efeito |
|---|---|---|
| `HB_BUILD_TESTS` | `ON` | Compila os testes (GoogleTest). `-DHB_BUILD_TESTS=OFF` acelera o build de quem só quer o app |
| `HB_BUILD_TOOLS` | `ON` | Compila `hb_probe`, `hb_pack`, `hb_remote_sim` e as ferramentas da câmera virtual |
| `HB_CAPTIONS_WHISPER` | `ON` no Windows, `OFF` nos demais | Compila o reconhecimento de fala local (whisper.cpp, só CPU, estático) das Legendas ao vivo. Com `OFF` o app compila e roda sem o motor local (só o motor em nuvem, opcional). Baixa e compila o whisper.cpp — é a dependência mais pesada do build |
| `HB_ORT_DIR` | `third_party/onnxruntime` | Pasta com o ONNX Runtime extraído (veja a próxima seção). Opcional: sem ele o chroma por IA fica desativado e só o chroma clássico funciona |
| `HB_WEBVIEW2_DIR` | `third_party/webview2` | Pacote NuGet `Microsoft.Web.WebView2` extraído. Sem ele, `HBStudio.exe` e `HBStudio-Setup.exe` não são gerados (só as bibliotecas e os testes) |
| `HB_EFFECTS_TSAN` | `OFF` | Gera também o executável de testes de efeitos com ThreadSanitizer |
| `FETCHCONTENT_BASE_PATH` | `<build>/_deps` | Pasta das dependências baixadas; apontá-la para fora do `build/` permite guardá-las em cache (é o que o CI faz) |

Verificação de races: `-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` (Linux/Clang/GCC).

## Windows: aplicativo e instalador

`HBStudio.exe` e `HBStudio-Setup.exe` só são gerados quando o SDK do WebView2 está disponível:

```
# PowerShell
Invoke-WebRequest "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.3856.49" -OutFile wv2.zip
Expand-Archive wv2.zip third_party/webview2 -Force
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
```

A versão do SDK usada pelo CI está em `WEBVIEW2_SDK_VERSION` no `.github/workflows/ci.yml`.
O instalador embute `HBStudio.exe` e `hb_vcam_source.dll` como recursos. A versão mostrada no
rodapé do app (`0.3.0+<commit>`) vem do `HB_VERSION` no `CMakeLists.txt` mais o hash curto do Git.

### Chroma por IA: ONNX Runtime (opcional) e `onnxruntime.dll`

1. Baixe o `onnxruntime-win-x64-<versão>.zip` oficial em
   <https://github.com/microsoft/onnxruntime/releases> e extraia em `third_party/onnxruntime`
   (de modo que existam `include/onnxruntime_cxx_api.h` e `lib/onnxruntime.lib`), **ou** aponte
   outra pasta: `cmake -S . -B build -DHB_ORT_DIR=C:/libs/onnxruntime-win-x64-1.x.y`.
2. Reconfigure. O CMake imprime `Chroma: ONNX Runtime found ...`; o app liga com *delay-load*, então
   **sem a DLL o app ainda abre** e a IA apenas avisa "onnxruntime.dll não encontrada".
3. **Distribuindo a DLL:** `onnxruntime.dll` precisa estar na mesma pasta do `HBStudio.exe`. **O instalador a
   embute automaticamente** quando `third_party/onnxruntime` (ou `HB_ORT_DIR`) contém `include/`, `lib/onnxruntime.lib`
   e `lib/onnxruntime.dll`: o CMake copia a DLL (e `LICENSE`/`ThirdPartyNotices.txt`, MIT) para `build/embed/` e o
   `setup.rc` ganha os recursos `IDR_ORT_DLL/LICENSE/NOTICES`; o instalador grava a DLL ao lado do exe, os avisos em
   `licenses\` e remove tudo ao desinstalar. Sem a pasta, nada é embutido e o instalador é o de antes. O CI baixa o
   zip oficial na versão fixada em `ORT_VERSION` (`ci.yml`; SHA-256 em `ORT_ZIP_SHA256`). Localmente, para testar sem
   instalar: copie `onnxruntime.dll` para `build/Release/`. O modelo de segmentação **não** vai no instalador: é baixado
   sob demanda pelo usuário (URL e SHA-256 fixados).

### Legendas locais (whisper.cpp)

Ligado por padrão no Windows (`HB_CAPTIONS_WHISPER=ON`); compila em CPU, sem OpenMP e sem
`-march=native` (o instalador roda em outros PCs; o ggml exige AVX2 em x86-64). Os modelos Whisper
não vão no repositório nem no instalador: são baixados sob demanda. Para um build mais rápido
durante o desenvolvimento de outra área: `-DHB_CAPTIONS_WHISPER=OFF`.

## Builds mais rápidos: cache de compilação

- **ccache (GCC/Clang, geradores Makefile/Ninja):**
  `cmake -S . -B build -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache`.
  Não funciona com o gerador *Visual Studio* (que ignora lançadores de compilador); no Windows use
  `--parallel` (e `CXXFLAGS=/MP`) ou configure com Ninja num "Developer Command Prompt" (os
  caminhos de saída mudam: `build/HBStudio.exe`, sem a subpasta `Release`).
- **Dependências em cache:** `-DFETCHCONTENT_BASE_PATH=<pasta fora do build>` mantém as dependências
  baixadas (e já compiladas) entre reconfigurações.
- O CI do GitHub usa tudo isso (ccache no Linux, cache de dependências nos dois sistemas,
  `--parallel`, `ctest -j 4`, cancelamento de execuções antigas da mesma branch e dispensa de
  mudanças só de documentação).

Estado: o build e os testes rodam no Linux (GCC 13) e no runner Windows do GitHub (MSVC 2022, no
CI). Não confunda isso com validação em hardware real — veja o "Status de validação" no
[README](README.md).

## Assinatura de código (Windows) — por que o Windows bloqueia o instalador

`HBStudio-Setup.exe` sai do CI **sem assinatura digital** (Authenticode). Isso faz o Windows
SmartScreen mostrar "O Windows protegeu o computador" / "não foi possível verificar o autor" no
primeiro clique — comportamento normal e esperado para **qualquer** executável baixado sem
assinatura, não é um defeito do instalador. Para abrir mesmo assim: na tela azul, clique em "Mais
informações" → "Executar assim mesmo" (ou, nas Propriedades do arquivo, marque "Desbloquear").

Para o aviso parar de aparecer (inclusive para quem baixa pela primeira vez), é preciso um
**certificado de assinatura de código** (code signing) — pago, de uma autoridade certificadora
(DigiCert, Sectigo, SSL.com etc.), com verificação de identidade/CNPJ. Dois tipos:

- **OV (Organization Validation)**: mais barato (~US$100–400/ano); o SmartScreen continua avisando
  até o instalador acumular reputação própria (certo número de downloads/execuções sem queixas).
- **EV (Extended Validation)**: mais caro (~US$300–600/ano, costuma exigir um token USB/HSM); some
  o aviso do SmartScreen **imediatamente**, sem precisar acumular reputação.

Assim que você tiver um certificado `.pfx`, o CI (`.github/workflows/ci.yml`, passo "Sign
installer") já está pronto para usá-lo automaticamente — não precisa mexer em nada além de
cadastrar dois *secrets* no repositório (Settings → Secrets and variables → Actions):

- `WIN_CODESIGN_PFX_BASE64`: o arquivo `.pfx` convertido para base64 numa linha só —
  `certutil -encode seu-certificado.pfx codificado.txt` no Windows (depois remova as linhas
  `-----BEGIN/END CERTIFICATE-----` do `codificado.txt`), ou `base64 -w0 seu-certificado.pfx` no
  Linux/macOS.
- `WIN_CODESIGN_PFX_PASSWORD`: a senha do `.pfx`.

Sem esses *secrets*, o passo de assinatura é pulado silenciosamente e o build continua normal (como
hoje). O passo assina só o instalador (`HBStudio-Setup.exe`, o arquivo que de fato é baixado e
carrega a "marca da web" que aciona o SmartScreen); o `HBStudio.exe` e o `hb_vcam_source.dll`
embutidos dentro dele continuam sem assinatura própria — como não são baixados diretamente pelo
navegador, não carregam essa marca e não disparam o SmartScreen por conta própria.

> Ao editar o workflow: **nunca** referencie `secrets.*` diretamente em um `if:` de passo (já
> causou rejeição do arquivo de workflow inteiro); passe o secret por `env:` e teste a variável
> dentro do script, como o passo "Sign installer" faz.
