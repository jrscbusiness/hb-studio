# Orquestração de engenharia — HB STUDIO (ondas 1 e 2)

Documento do MAESTRO. Escrito ANTES de qualquer execução da onda 2. Data: 2026-10-20.

## 1. Diagnóstico do estado atual

Base: `main` @ 7d04afa. Núcleo C++20 (`hb_core`: event bus, logger com redação de segredos, relógio, switcher, program output, engine, ptz_settings) + app Windows (WebView2, `src/app/windows`) + câmera virtual (`src/virtual-camera`). Módulos de recurso entram por `cmake/modules/*.cmake` (glob no CMakeLists raiz).

Onda 1 (worktrees `worktree-agent-*`), situação verificada por `git log main..<branch>`:

| Frente | Branch | Estado | Observação |
|---|---|---|---|
| (d) Áudio | `worktree-agent-aae225412d8fe21ff` | 3 commits | Integrado. 8 libs/headers + AUDIO.md. |
| (c) PTZ + FOMAKO | `worktree-agent-af169995141bf82e8` | 1 commit | Integrado. VISCA/Pelco/ONVIF-PTZ, safety layer, PresetStore, UDP. |
| (b) RTSP/RTP/H.264 | `worktree-agent-a1ca414d56395f9c6` | **0 commits** (HEAD = base); existe `src/rtsp/` NÃO commitado no worktree | Não integrável: nada commitado. Pendente. |
| (a) ONVIF + discovery | `worktree-agent-a36d5d848b67a965b` | **0 commits**; existem `src/onvif`, `src/discovery` NÃO commitados | Não integrável. Pendente. |
| (e) Revisão adversarial | (sem branch) | nenhum `docs/REVIEW-1.md` | Pendente. |

Lacunas para o objetivo do usuário (sem falhas, configurável, compatível): não há banco de perfis de câmera, nem módulo de configuração tipado, nem simuladores para CI, nem entrada UVC/NDI/HTTP/SRT, nem decodificação, nem fuzzing, nem UI de câmeras.

Restrição desta execução: a ferramenta `Agent` NÃO está disponível ao MAESTRO nesta sessão (só Bash/Read/Edit/Write/Grep/Glob/Skill/ToolSearch e ferramentas de sessão remota). Conforme a regra, não finjo: a onda 2 abaixo está inteiramente **especificada, com prompts prontos (seção 5)**, mas **não foi executada**. Nenhum agente novo foi criado.

## 2. Roster proposto da onda 2 (8 agentes novos, ≤ 10; ≤ 4 simultâneos)

Todos com `isolation: "worktree"`, sem `git push`, sem tocar em `main`, `src/app/windows/*`, `.github/workflows/*`, `src/virtual-camera/*`, nem no `CMakeLists.txt` raiz.

| # | Nome | Persona | Pastas exclusivas | Depende de | Pronto quando (mensurável) | Testes exigidos |
|---|---|---|---|---|---|---|
| W2-1 | `profiles` | Engenheiro de interoperabilidade de câmeras IP, cético com fontes | `src/camera_profiles/`, `profiles/`, `tests/camera_profiles_*`, `cmake/modules/camera_profiles.cmake`, `docs/COMPATIBILITY.md` | ONVIF (só como tipos; usa strings, sem link) | ≥ 15 famílias com perfil JSON (lista na seção 5.1); 100% dos campos com `source` + `confidence` ou `"unverified"`; registry valida schema, migra v1→vN, aceita perfil do usuário com precedência; 0 senha padrão no repo (grep em CI de teste) | ≥ 40 testes: schema válido/ inválido, migração, casamento por manufacturer/model/scopes, override, JSON malformado, template de URL (placeholders), teste que varre `profiles/*.json` contra schema |
| W2-2 | `config` | Engenheiro de sistemas embarcados, foco em robustez de persistência | `src/config/`, `tests/config_*`, `cmake/modules/config.cmake`, `docs/CONFIGURATION.md` | — | Registro tipado (bool/int/double/string/enum/duração) com min/max, default, validação, migração de versão, persistência JSON atômica (tmp+rename+fsync, backup `.bak`), notificação de mudança via event bus; corrupção do arquivo → volta ao último bom/defaults sem crash | ≥ 35 testes incl. escrita interrompida (injeção de falha), JSON truncado/ gigante/ com tipos errados, migração, concorrência (TSan) |
| W2-3 | `sim` | Engenheiro de testes de rede, escreve servidores falsos | `tools/sim/`, `tests/sim_*`, `cmake/modules/sim.cmake`, `docs/SIMULATORS.md` | RTSP e ONVIF integrados (para testes de integração); parte VISCA/PTZ já possível | Simuladores RTSP (DESCRIBE/SETUP/PLAY, RTP/UDP e interleaved TCP, H.264 sintético), ONVIF SOAP (GetDeviceInformation, GetProfiles, GetStreamUri, PTZ, GetSystemDateAndTime), WS-Discovery responder, VISCA-over-IP; injeção de falhas: queda de conexão, 401 (Basic/Digest), perda/reordenação de pacotes, relógio errado, respostas lentas | Testes de integração sem hardware, cada < 2 s, portas efêmeras (0), determinísticos com semente |
| W2-4 | `inputs` | Engenheiro de captura/mídia | `src/inputs/` (uvc/, ndi/, mjpeg/, srt/), `tests/inputs_*`, `cmake/modules/inputs.cmake`, `docs/INPUTS.md` | Interface `CameraInput` do projeto (ler `src/video`) | Provider por entrada atrás de interface + FAKE testável; NDI por `LoadLibrary/dlopen` do runtime (SDK não redistribuído, detecção de ausência com erro claro); UVC (Media Foundation) e SRT sob `#ifdef` marcados “NÃO VALIDADO”; MJPEG/HTTP parser multipart puro | Testes com fakes; parser MJPEG com entradas hostis; carregamento dinâmico com biblioteca ausente |
| W2-5 | `decode` | Engenheiro de codecs | `src/decode/`, `tests/decode_*`, `cmake/modules/decode.cmake`, `docs/DECODING.md` | RTSP (tipo de frame codificado) | `IVideoDecoder` (push NAL/AU → frame), política de fallback HW→SW, MFT (Windows, `#ifdef`) e FFmpeg LGPL dinâmico (carregamento runtime, nada redistribuído); decoder falso determinístico; erro de decodificação nunca derruba o processo | Testes com fake; contrato de fila/limite de memória; TSan |
| W2-6 | `fuzz-qa` | Engenheiro de QA/segurança | `tests/fuzz/`, `tools/fuzz/`, `cmake/modules/fuzz.cmake`, `docs/QA.md` | Parsers de RTSP/ONVIF/PTZ/config integrados | Harness libFuzzer (clang) + fallback de corpus-replay para GCC em ctest, para SOAP/XML, SDP, RTP, VISCA, Pelco, JSON de config/perfis; corpus semente; rodada ASan+UBSan e TSan documentada | Replay de corpus roda no ctest; relatório de crashes (0 abertos) |
| W2-7 | `ui-cameras` | Designer/dev front-end, minimalista (azul e branco) | `ui/index.html` (somente aba CÂMERAS), `docs/UI_CAMERAS.md` | Contrato de mensagens da engine (definir em docs) | Aba com descoberta, adicionar, testar, escolher perfil; verificada com `tools/ui/design_preview.js`; sem alterar `src/app/windows` | Captura de preview anexada; checagem sem erros de console |
| W2-8 | `review-2` | Revisor adversarial (somente leitura) | escreve apenas `docs/REVIEW-2.md` | Todos | Revisão de protocolos/segurança: segredos em log, validação de tamanhos, DoS, race conditions, SSRF via URLs de perfil; lista severidade + reprodutor | — |

Também fecham a onda 1: (e) `docs/REVIEW-1.md` (já em andamento por outro agente, não duplicar).

## 3. Ondas e ordem

- **Onda 1 (em curso)**: áudio e PTZ integrados; RTSP e ONVIF aguardam commit; revisão REVIEW-1 aguarda.
- **Onda 2A (4 simultâneos, independentes)**: `profiles`, `config`, `inputs`, `decode`. Nenhum depende de código ainda não integrado.
- **Onda 2B (após RTSP+ONVIF integrados)**: `sim`, `fuzz-qa`, `ui-cameras`, `review-2` (≤ 4).
- Integração: ordem de dependência `config` → `profiles` → `audio` → `ptz` → `rtsp` → `onvif/discovery` → `decode` → `inputs` → `sim` → `fuzz-qa`. Branch `integration/wave1`.

## 4. Riscos e mitigações

| Risco | Mitigação |
|---|---|
| Trabalho de RTSP/ONVIF não commitado se perde ou chega meio escrito | Não copio arquivos de worktrees alheios (podem estar a meio de escrita). Registro como pendente; reintegrar quando houver commit. |
| Afirmar compatibilidade sem prova | Escala única: Implementado / Testado em simulador / Validado em hardware / Planejado; perfis só com `source`+`confidence`, senão `"unverified"`; teste que falha se um perfil omitir fonte. |
| Senhas padrão ou segredos no repositório/log | Política “NUNCA listar senhas padrão”; teste que varre `profiles/` por palavras-chave; toda saída passa por `redactSecrets`. |
| Conflitos de merge | Pastas disjuntas + um `.cmake` por módulo; ninguém edita CMake raiz. |
| Corrida/travamento em código concorrente | Gate TSan em todo módulo; tempo injetável; testes < 2 s. |
| Entrada hostil derruba o processo | Fuzz-qa; validação de tamanhos; exceções contidas nas fronteiras. |
| Código de SO sem validação (Winsock, MF, NDI) | `#ifdef`, marcado NÃO VALIDADO em docs; fakes cobrem a lógica. |
| Custo | ≤ 4 simultâneos, 8 de 10 agentes; tarefas triviais não viram agente. |
| URLs/dados de fabricantes inventados | Agente `profiles` só registra o que achar em documentação oficial citável; resto “não verificado”. |

## 5. Prompts prontos (autossuficientes)

Cabeçalho comum a todos (cole no início de cada prompt):

> Projeto HB STUDIO (C++20, `namespace hb::<modulo>`, código/comentários em inglês, docs em pt-BR). Leia `docs/CONTRIBUTING.md`, `ARCHITECTURE.md`, `README.md`, `CMakeLists.txt`. Trabalhe no seu worktree/branch. Regras: compila sem warnings em GCC `-Wall -Wextra -Wpedantic` e MSVC 2022; lógica pura testável sem rede/hardware; nenhuma entrada externa derruba o processo; nunca logue segredos (`hb::core::redactSecrets`); tempo injetável; testes < 2 s; dependências só via FetchContent com versão fixa e licença permissiva, listadas em `LICENSES.md`; documente o que não foi validado em hardware como “não validado”. Registre o módulo apenas com `cmake/modules/<modulo>.cmake`. NÃO edite `CMakeLists.txt` raiz, `src/core`, `src/video`, `src/switcher`, `src/app`, `src/virtual-camera`, `.github/workflows`. NÃO faça `git push`, nem mexa em `main`, nem em branches/worktrees de outros. Faça commits pequenos no seu branch. Antes de terminar rode build normal (0 warnings), `ctest`, e uma execução com `-DCMAKE_CXX_FLAGS="-fsanitize=thread -g"` em outro diretório de build. Relatório final: arquivos criados, commits, números de build/ctest/TSan, o que NÃO foi validado, pendências.

### 5.1 `profiles`
Pastas exclusivas: `src/camera_profiles/`, `profiles/*.json`, `tests/camera_profiles_*`, `cmake/modules/camera_profiles.cmake`, `docs/COMPATIBILITY.md`. Entregue `CameraProfileRegistry` + JSON orientado a dados (nada hardcoded por fabricante): por família, templates de URL RTSP main/sub (com placeholders `{host}`, `{port}`, `{channel}`), portas padrão, política de credenciais (NUNCA listar senhas padrão; apenas “exige troca no primeiro acesso” etc., se houver fonte), protocolos de controle (ONVIF/VISCA/Pelco/CGI), capacidades PTZ (presets máx., base 0/1, faixas de velocidade), quirks (TCP obrigatório, Digest vs Basic, relógio, codecs), regras de casamento (ONVIF manufacturer/model/scopes, override do usuário). Campo `schema_version`, validação de schema, migração, perfil customizado do usuário (diretório configurável, precedência sobre os embarcados), erros de perfil isolados (um perfil ruim não derruba os outros). Pesquisa web somente leitura por fabricante: Hikvision, Dahua, Axis, Reolink, Amcrest, Uniview, TP-Link VIGI/Tapo, Foscam, Sony SRG, Panasonic AW, Canon CR, PTZOptics, BirdDog, Marshall, AVer, Lumens, Datavideo, FOMAKO (ver `docs/FOMAKO.md`), genéricas OEM ONVIF, webcams UVC. Cada dado leva `source` (URL de documentação oficial) e `confidence` (alto/médio/baixo); sem fonte confiável → `"unverified"`. Não invente URLs. `docs/COMPATIBILITY.md` com a escala Implementado / Testado em simulador / Validado em hardware / Planejado (nada será “Validado em hardware”). Testes: ≥ 40 (ver roster).

### 5.2 `config`
Pastas: `src/config/`, `tests/config_*`, `cmake/modules/config.cmake`, `docs/CONFIGURATION.md`. Registro tipado de configurações com schema (chave, tipo, default, min/max, enum, descrição, flag “requer reinício”), validação, migração de versão, persistência JSON atômica (arquivo temporário, flush, rename, backup `.bak`, recuperação do backup se o principal estiver corrompido), eventos de mudança. Cobrir latência, buffers, transporte RTSP (UDP/TCP/auto), codecs, perfis de câmera, PTZ (velocidades, limites, timeouts). Padrões seguros. Segredos (senhas) nunca em texto claro nos logs; documentar onde ficam armazenados (apenas interface `ISecretStore`, implementação DPAPI/Keychain fora de escopo → pedido). JSON parser: usar nlohmann/json via FetchContent com versão fixa (MIT) ou o parser já presente no repositório se houver; checar limites de profundidade/tamanho.

### 5.3 `sim`
Pastas: `tools/sim/`, `tests/sim_*`, `cmake/modules/sim.cmake`, `docs/SIMULATORS.md`. Pré-requisito: `src/rtsp`, `src/onvif`, `src/discovery` integrados; se ausentes, entregue só VISCA-over-IP e a base de servidor TCP/UDP e deixe o restante pendente. Simuladores com portas efêmeras, semente fixa, injeção de falhas (queda, 401 Basic/Digest, perda/reordenação/duplicação RTP, relógio errado no `GetSystemDateAndTime`, respostas lentas, SOAP malformado). Testes de integração ponta a ponta contra os módulos reais.

### 5.4 `inputs`
Pastas: `src/inputs/`, `tests/inputs_*`, `cmake/modules/inputs.cmake`, `docs/INPUTS.md`. Providers de `CameraInput` (ver `src/video`) para UVC (Media Foundation, `#ifdef _WIN32`, NÃO VALIDADO), NDI (carregamento dinâmico do runtime com `LoadLibrary`/`dlopen`; SDK NÃO redistribuído; detecção de ausência com erro claro; incluir NDI HX como capacidade a detectar, não afirmada), HTTP/MJPEG (parser multipart puro e testável), SRT (somente interface + fake se não houver lib permissiva viável). Cada provider tem fake e testes.

### 5.5 `decode`
Pastas: `src/decode/`, `tests/decode_*`, `cmake/modules/decode.cmake`, `docs/DECODING.md`. `IVideoDecoder` (entrada: unidades de acesso codificadas; saída: frames), fila limitada, política HW→SW, implementação Media Foundation (`#ifdef`, NÃO VALIDADO) e FFmpeg LGPL carregado dinamicamente (nada redistribuído; licença documentada), decoder falso. Se `src/rtsp/decoder.hpp` existir após a integração, alinhe a interface com ele em vez de duplicar.

### 5.6 `fuzz-qa`
Pastas: `tests/fuzz/`, `tools/fuzz/`, `cmake/modules/fuzz.cmake`, `docs/QA.md`. Harness libFuzzer (clang, opcional) + replay de corpus no ctest sob GCC para: SOAP/XML, SDP, RTP, VISCA, Pelco, JSON de config e de perfis. Corpus semente; executar ASan+UBSan e TSan; corrigir NÃO nos módulos alheios — abrir lista de achados em `docs/QA.md` com reprodutor.

### 5.7 `ui-cameras`
Somente `ui/index.html` (aba CÂMERAS) e `docs/UI_CAMERAS.md`. Estilo existente (azul e branco, minimalista). Leia o cabeçalho de `tools/ui/design_preview.js` e use-o para verificar. Não altere `src/app/windows`. Defina o contrato de mensagens JS↔host em docs (pedido ao autor).

### 5.8 `review-2` (somente leitura)
Revisão adversarial de `integration/wave1`: segredos em logs, validação de tamanhos, DoS, corridas, injeção XML, SSRF/URL de perfil, afirmações de compatibilidade sem fonte. Escreve apenas `docs/REVIEW-2.md` (severidade, arquivo:linha, reprodutor).

## 6. Pedidos ao autor do projeto (arquivos fora do meu escopo)

1. Integrar o novo glob `cmake/modules/*.cmake` já existe: nenhuma mudança necessária no CMake raiz. Se quiser flags de aviso globais (`-Wall -Wextra -Wpedantic`) e opção `HB_SANITIZE`, é pedido para o `CMakeLists.txt` raiz.
2. `.github/workflows`: adicionar jobs Linux GCC (build+ctest) e TSan (`-fsanitize=thread`) para os módulos de recurso; hoje a CI não os exercita.
3. `src/app/windows`: ligar PTZ (`hb_ptz`), áudio (`hb_audio`) e, quando existirem, câmeras/perfis ao app; expor contrato de mensagens da aba CÂMERAS; armazenar credenciais via DPAPI (Windows) / Keychain (macOS) por trás de `ISecretStore`.
4. `src/virtual-camera`: nenhuma mudança solicitada nesta onda.
5. Implementação real de transporte serial/TCP para PTZ (interface pronta, sem implementação) e validação do Winsock em máquina Windows.

## 7. Registro de execução

Ver o relatório final do MAESTRO; resumo de integração em `integration/wave1` (merges de áudio e PTZ; build GCC 0 warnings; ctest 164/164; TSan 164/164 sem avisos).
