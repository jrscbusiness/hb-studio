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
