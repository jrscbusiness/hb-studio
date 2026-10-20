# Revisão adversarial nº 1 — HB Studio

Revisor: Claude (modo somente leitura). Base analisada: `f2ea958` (o repositório recebeu commits enquanto eu revisava; os arquivos de `src/core`, `src/video`, `src/switcher`, `src/virtual-camera` não mudaram entre `0e13968` e `f2ea958`).
Nada foi alterado no repositório além deste arquivo. As sondas ficaram fora do repositório, no scratchpad de sessão `/tmp/claude-0/-home-user-hb-studio/cc38f550-1792-58bd-95ad-23a53284f353/scratchpad/p/` (`redact_probe*.cpp`, `redact_big.cpp`, `uaf_probe2.cpp`, `misc.cpp`, `ptz_probe.cpp`), compiladas com g++ 13 contra `src/`.

**Legenda de evidência**
- **[SONDA]** reproduzido compilando e executando código contra `src/` (Linux, g++ 13, `-O1`, às vezes ASan/UBSan).
- **[INSPEÇÃO]** conclusão lida do código e da semântica documentada das APIs. O código Windows não pôde ser executado aqui. Onde há incerteza sobre o comportamento real do Windows/Frame Server, eu digo.

**Veredito curto.** Não encontrei nenhum achado que eu consiga sustentar como CRÍTICO. Os mais graves são: (1) o `redactSecrets` tem tempo quadrático e estoura a pilha em mensagens longas, e deixa vazar vários formatos comuns de segredo; (2) o servidor de pipe da câmera virtual deixa qualquer usuário autenticado criar instâncias do pipe e tomar o único slot de conexão; (3) `ProgramOutput::removeConsumer` não espera callbacks em voo (use-after-free provado por sonda lógica); (4) a conversão/entrega NV12 tem pressupostos de layout e perde tokens de `RequestSample` em falha, o que pode travar o fluxo no Frame Server.

Resumo por severidade: ALTO 6, MÉDIO 12, BAIXO 15 (mais lista de testes).

---

## ALTO

### A1. Pipe da câmera virtual: ACL permite criar instâncias; produtor não autentica o servidor
- `src/virtual-camera/windows/vcam_source.cpp:175` (SDDL), `:182-185` (`CreateNamedPipeW`), `src/virtual-camera/windows/vcam_sender.cpp:107` (`CreateFileW`).
- Evidência: **[INSPEÇÃO]** (semântica de ACL de pipes; não executável aqui).
- Descrição. A DACL `D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)` dá a *qualquer usuário autenticado* `GENERIC_WRITE`. Em named pipes, `FILE_GENERIC_WRITE` inclui `FILE_APPEND_DATA`, que é o mesmo bit de `FILE_CREATE_PIPE_INSTANCE`. Ou seja, `AU` pode criar **novas instâncias** do pipe `\\.\pipe\HBStudioVirtualCamera` (o servidor usa `PIPE_UNLIMITED_INSTANCES`). Além disso a primeira instância não usa `FILE_FLAG_FIRST_PIPE_INSTANCE`, e o remetente abre o pipe com `CreateFileW(GENERIC_WRITE, ..., 0 /*sem SQOS*/)` sem verificar quem é o servidor.
- Cenários concretos:
  1. *Sequestro/vazamento*: outro usuário local (máquina compartilhada, RDP) ou malware com token de usuário comum cria uma instância ouvinte do mesmo pipe (modo INBOUND, igual ao da primeira) e recebe os quadros do Program Output do app (vídeo ao vivo, possivelmente confidencial). O app não tem como saber que não está falando com o Frame Server.
  2. *Squatting antes do serviço*: o pipe só existe enquanto algum app usa a câmera. Um processo que crie o nome antes do Frame Server (com DACL que permita ao LocalService criar instância) fica ouvindo primeiro.
  3. *Forja*: qualquer `AU` pode escrever quadros (ver A2) e mostrar imagem arbitrária na "câmera" de outro usuário/sessão.
  4. Sem flags SQOS, o `CreateFileW` do produtor usa o nível de impersonação padrão; um servidor falso pode chamar `ImpersonateNamedPipeClient` e agir com o token do usuário.
  Risco real: precisa de código local rodando como usuário autenticado (não é remoto: `PIPE_REJECT_REMOTE_CLIENTS` está correto). Em PC pessoal de um único usuário o ganho do atacante é pequeno (mesmo usuário já pode capturar a tela); em máquina compartilhada é relevante. Processos de integridade Low/AppContainer provavelmente são barrados pelo rótulo de integridade padrão (não confirmado).
- Correção sugerida:
  ```cpp
  // servidor: direitos mínimos, sem FILE_CREATE_PIPE_INSTANCE para AU; LS (LocalService) pode recriar instâncias
  L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;0x00100082;;;IU)"   // 0x100082 = SYNCHRONIZE|FILE_WRITE_DATA|FILE_READ_ATTRIBUTES
  // primeira instância reivindica o nome; limite de instâncias baixo (1) para impedir instâncias de terceiros
  DWORD open = PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0);
  CreateNamedPipeW(kPipeName, open, PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,
                   /*nMaxInstances*/ 1, 0, 4*1024*1024, 0, &sa);
  // se falhar com ERROR_ACCESS_DENIED na primeira: nome sequestrado -> registrar e recusar (não ceder).
  ```
  Remetente: `CreateFileW(kPipeName, FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | SYNCHRONIZE, 0, nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, nullptr)` (com GENERIC_WRITE o `CreateFile` pediria `FILE_APPEND_DATA` e seria negado pela ACL nova). Depois do `CreateFile`, chamar `GetNamedPipeServerProcessId` e conferir que o PID é o do serviço `FrameServer` (`QueryServiceStatusEx` → `dwProcessId`). No servidor, `GetNamedPipeClientProcessId` + `QueryFullProcessImageNameW` comparando com `%ProgramFiles%\HB Studio\HBStudio.exe` (verificar na prática se o LocalService consegue abrir o processo do usuário com `PROCESS_QUERY_LIMITED_INFORMATION`). `IU` limita ao usuário interativo; se o app puder rodar fora de sessão interativa, usar o SID do usuário que instalou. Atenção: com `nMaxInstances=1` dois `ActivateObject` no mesmo serviço (ver M3) colidem — resolver com um hub singleton por processo.

### A2. Pipe: um único slot de conexão sem timeout → qualquer usuário trava a câmera; dois produtores
- `vcam_source.cpp:181-238` (`Run`), `:130-167` (`ReadExact`, `INFINITE`), `vcam_sender.cpp:107-113`.
- Evidência: **[INSPEÇÃO]**.
- Descrição. O servidor mantém no máximo **uma** instância ouvindo; enquanto um produtor está conectado não existe instância livre. Um segundo produtor recebe `ERROR_PIPE_BUSY` (o remetente só tenta de novo a cada 300 ms, silenciosamente) e nunca entra, o que é o comportamento certo para "dois produtores", mas não há política (o primeiro vence para sempre). Como `ReadExact` espera com `INFINITE`, um cliente que conecta (qualquer `AU`, ver A1) e não envia nada, ou envia 1 byte por vez, ocupa o slot indefinidamente: *denial of service* da câmera enquanto o Frame Server estiver com o app Câmera/OBS aberto. Um app legítimo congelado (não morto) tem o mesmo efeito.
- Correção: `WaitForMultipleObjects(..., timeoutMs)` com timeout de leitura de quadro (ex.: 3 s sem quadro completo → `DisconnectNamedPipe` e volta a ouvir); limite de duração por cabeçalho (ex.: o quadro inteiro tem de chegar em < 1 s). Política documentada para o segundo produtor: o mais novo vence (aceitar a segunda conexão e derrubar a primeira) **ou** recusa com log. Combinar com A1 (só o produtor autenticado tem acesso).

### A3. `redactSecrets`: tempo quadrático e estouro de pilha com mensagens longas
- `src/core/redact.cpp:34-35` (regex de esquema `([a-z][a-z0-9+.\-]*://)...`), `:37`, `:49-53`; chamado de `Logger::log` (`src/core/logger.cpp:432`) em **todas** as mensagens.
- Evidência: **[SONDA]** (`redact_big.cpp`).
- Resultado medido (g++ `-O1`, mensagem = N caracteres `a` sem nenhum segredo): N=1.000 → 54 ms; N=10.000 → **5,2 s**; N=20.000 → 22 s; N=30.000 → 49 s; N=50.000 → **SIGSEGV** (pilha de 8 MB). Com pilha de 1 MB (padrão de threads no Windows) já cai com N=10.000. Mensagens com espaços (ex.: 200.000 espaços) são rápidas: o problema é a corrida de caracteres `[a-z0-9+.\-]` (base64, hex, JWT, URL longa, nome de arquivo sem espaços).
- Cenário: qualquer módulo que registre um corpo de resposta, um snapshot em base64 ou uma URL assinada longa bloqueia a thread chamadora por segundos/minutos. `Logger::log` roda inline em quem publicou o evento — por exemplo, a thread do relógio do Program Output via `EventBus` (`Engine` assina `switcher.*`/`program.*` e chama o logger) — e a falha de pilha derruba o processo, violando a regra 2 do CONTRIBUTING ("nenhuma entrada externa pode derrubar o processo").
- Correção: (a) imediatamente, limitar a entrada: `if (text.size() > 4096) text = text.substr(0, 4096) + "...[truncado]";` antes das regex; (b) trocar a regra de userinfo por busca linear: `find("://")`, voltar até o início do esquema, procurar o `@` mais à direita antes do primeiro `/` do authority (isso também corrige A4-1/A4-2); (c) adicionar teste com mensagem de 100 KB sem segredo e limite de tempo.

### A4. `redactSecrets`: segredos que vazam (todos confirmados)
- `src/core/redact.cpp:32-43`. Evidência: **[SONDA]** (`redact_probe.cpp`, `redact_probe2.cpp`; entrada → saída real).
- Foram redigidos corretamente: URL `scheme://user:pass@host` (inclusive maiúsculas e `%40`), `Authorization:`/`Proxy-Authorization:` (inclusive minúsculas), `X-Api-Key:`/`X-Auth-Token:`, `password=`, `"password" : "x"` com espaços, `apikey`/`api-key`, `<wsse:Password>` (inclusive multilinha), `rtmp(s)://host/app/CHAVE`, `passphrase=`.
- **Vazamentos:**
  1. Senha com `@` literal: `rtsp://admin:p@ss@10.0.0.1/s` → `rtsp://***:***@ss@10.0.0.1/s` (vaza o resto da senha: `ss`). A regex do userinfo exclui `@` e para no primeiro.
  2. Senha com `/` literal: `rtsp://admin:pa/ss@10.0.0.1/s` → **sem nenhuma redação** (vaza `admin:pa/ss`).
  3. Senha com espaço: `rtsp://admin:pa ss@10.0.0.1/` e `ftp://u:p w@host` → sem redação.
  4. `Cookie: session=SESSVAL999; other=1` e `Set-Cookie: sid=COOKIEVAL42; HttpOnly` → intactos.
  5. JSON com chave `Authorization`: `{"Authorization":"Bearer ABCDEF123"}` e `{"authorization": "Basic QUJD"}` → intactos (a regra exige `authorization\s*:` sem aspas; a regra chave=valor não inclui `authorization`).
  6. Chaves compostas, por causa de `\b`: `access_token=ACC123`, `refresh_token=REF123`, `client_secret=CS123`, `secretKey=SK123`, `db_password=...`, `user_password=...`, `passwordHash=PH999` → intactos. (`_` é caractere de palavra, então `\btoken` não casa dentro de `access_token`.) OAuth de YouTube/Facebook usa justamente `access_token`/`refresh_token`/`client_secret`.
  7. Valores com espaço, vírgula, ponto-e-vírgula ou aspas escapadas: `{"password":"hun ter2 more"}` → `"*** ter2 more"`; `password="two words"` → `"*** words"`; `password: two words here`; `password=a,b;c` → `***,b;c`; `password="a;b"` → `"***;b"`; `{"token":"abc\"def"}` → `"***"def"`. O valor é cortado no primeiro delimitador.
  8. Segredos em query string de URL: `?access_token=XYZ123`, `?auth=`, `?key=`, `?sig=`, SRT `?streamid=SID123` → intactos (só `passphrase`, `token`, `password` etc. exatos).
  9. `rtmp://live.example.com/KEYONLY789` (chave direto após o host, sem segmento de aplicação) → intacto.
  10. Credenciais soltas fora de header: `Basic YWRtaW46aHVudGVyMg==`, `Bearer eyJhbGciOi...` → intactos.
  11. Texto livre ("login failed for admin / S3CRETPW", "my password is hunter2", `sip:user:secretpw@host`) → intacto (limite inerente; documentar).
  12. `<wsse:Nonce>`/`WWW-Authenticate: ... nonce=` → intactos (nonce em si não é segredo permanente; citado só para completar).
- Correção: tratar userinfo por parser (A3), generalizar a regra chave=valor:
  ```
  ((?:[A-Za-z0-9]+[_.\-])*(?:password|passwd|pwd|passphrase|pass|token|secret|api[_\-]?key|stream[_\-]?key|auth|sig(?:nature)?|cookie|sessionid)[A-Za-z0-9_.\-]*)(["']?\s*[:=]\s*)("(?:[^"\\]|\\.)*"|'[^']*'|[^\s"'&;]+)
  ```
  (precisa de passar em novas sondas e de revisão de falsos positivos, ex.: `author=`), tratar `Cookie`/`Set-Cookie` como `Authorization` (linha inteira), `Bearer|Basic <token>` isolados, e `Authorization` como chave JSON. Estender `redact_test.cpp` com cada caso acima como teste de regressão, inclusive negativos (texto normal intacto).

### A5. `ProgramOutput::removeConsumer` não espera callbacks em voo → use-after-free
- `src/video/program_output.cpp:276-281` e `:309-322` (`onTick` copia a lista e chama fora do lock); consumidor afetado: `src/virtual-camera/windows/vcam_sender.cpp:76-97` (`stop()` faz `removeConsumer` e o destrutor destrói `mutex_`/`cv_`).
- Evidência: **[SONDA]** (`uaf_probe2.cpp`): com um callback lento já em andamento, `removeConsumer()` retorna, o "dono" é marcado como destruído, e o callback ainda roda depois: `callback ran after removeConsumer returned and owner destroyed: YES`. Uma versão com ASan e dono alocado no heap travou o processo (comportamento coerente com uso de mutex liberado). O teste existente `RemovedConsumerStopsReceiving` aceita `n <= at + 1`, ou seja, já tolera esse "+1" (`tests/program_output_test.cpp:115`).
- Cenário: `VirtualCameraController::doDeactivate` → `sender->stop()` → `~PipeFrameSender`, enquanto um tick do relógio está dentro do lambda `[this]`. Janela curta (o lambda só guarda o `shared_ptr`), mas é corrupção de memória em desativação/saída do app. O mesmo vale para `EventBus::unsubscribe` (`event_bus.cpp:242-247`) com handlers que capturam `this`.
- Correção: o consumidor deve ser removido de forma síncrona. Opção simples: manter um `std::shared_ptr<Consumer>` por registro e, em `removeConsumer`, esperar que o contador de execuções em voo desse consumidor chegue a zero (com `cv`), proibindo chamada a partir do próprio callback; alternativa: o lambda captura `std::weak_ptr<State>` e o remetente guarda um `shared_ptr<State>` que sobrevive ao destrutor.

### A6. Conversão/entrega NV12 sem validação do layout e sem `SetCurrentLength`
- `vcam_source.cpp:527-558` (`Deliver`), `:540-548`.
- Evidência: **[INSPEÇÃO]**; sem Windows para provar. Marco como ALTO porque o código nunca rodou numa máquina real e o resultado possível é imagem corrompida/preta no OBS.
- Descrição:
  1. O plano UV é assumido em `scanline0 + pitch*1080`. Superfícies de vídeo costumam ter altura alinhada (1080 → 1088); o offset do plano UV deve vir do formato do buffer (`MFGetPlaneSize`, `MFCalculateImageSize`/`MF_MT_DEFAULT_STRIDE`, ou do tipo da amostra do alocador), não de `height` literal. Se o alocador devolver buffer com linhas de preenchimento, o UV fica deslocado (cores erradas/verde-magenta). O selftest (`tools/hb_vcam_selftest.cpp`) só confere luma em 2 pixels, então não pega isso.
  2. Se `pitch <= 0` ou `length < pitch*h*3/2` o código **silenciosamente não escreve nada** e mesmo assim entrega a amostra (conteúdo não inicializado/anterior do pool) em vez de retornar erro.
  3. Não há `buf->SetCurrentLength(...)`; só `Lock2D/Unlock2D`. Em buffers de memória do alocador o `GetCurrentLength` pode ser 0 e consumidores que usam o comprimento corrente (Source Reader, conversores) leriam zero bytes. Defina explicitamente `SetCurrentLength(pitch*h*3/2)`.
  4. Faltam atributos habituais de amostra de câmera (`MFSampleExtension_DeviceTimestamp`, `MFSampleExtension_FrameCorruption=0` etc.); `SetSampleTime(MFGetSystemTime())` tem jitter da thread (não usa o relógio de quadros, então intervalos entre quadros podem sair irregulares).
- Correção:
  ```cpp
  const DWORD need = static_cast<DWORD>(pitch) * h * 3 / 2;
  if (pitch <= 0 || length < need) { buf2d->Unlock2D(); return MF_E_BUFFERTOOSMALL; }
  // offset do UV: usar o MF_MT_DEFAULT_STRIDE/altura alinhada do tipo do alocador; validar com MFGetPlaneSize
  buf->SetCurrentLength(need);   // ou o tamanho real do formato
  ```
  e acrescentar ao selftest: verificação de cor de um pixel UV conhecido (barra magenta/vermelha) e `pitch != largura`.

---

## MÉDIO

### M1. Falha em `Deliver` perde o token de `RequestSample` (fluxo pode travar)
- `vcam_source.cpp:511-523`. **[INSPEÇÃO]**.
- O token é retirado de `m_tokens` antes de `AllocateSample`; se a alocação falhar (`MF_E_SAMPLEALLOCATOR_EMPTY` quando o cliente ainda segura as 10 amostras, `MF_E_SHUTDOWN`, etc.) o token **não volta** e nenhum `MEMediaSample` correspondente é enviado nem é enviado `MEError`. O Frame Server contabiliza pedidos pendentes; se perder N pedidos, pode esperar para sempre. Além disso, `RequestSample` descarta o token mais antigo ao passar de 8 (`:380`) sem avisar.
- Correção: em falha, recolocar o token no início da fila (ou entregar a última amostra boa / enviar `MEError` quando for erro permanente); ao estourar o limite, entregar uma amostra descartável ou aumentar o limite em vez de apagar tokens.

### M2. Corridas em `FrameHub::Stop/Start` e no worker de `HBStream`
- `vcam_source.cpp:100-118`, `:417-427`, `:475-490`. **[INSPEÇÃO]**.
- `FrameHub::Stop` solta o lock, faz `join`, depois `CloseHandle(m_stop); m_stop = nullptr` **fora do lock**. Se `Start()` rodar entre o `join` e o `CloseHandle` (por `SetStreamState(RUNNING)` chamado pelo Frame Server sem passar pelo lock da fonte), o `Start` cria um novo `m_stop` e uma nova thread, e o `Stop` pendente fecha o handle novo e zera `m_stop`: a nova thread passa a esperar em handle inválido (`WAIT_FAILED != WAIT_OBJECT_0` ⇒ laço sem parada) e o próximo `Stop` não consegue sinalizá-la (`SetEvent(nullptr)`), então `join` trava para sempre. O mesmo padrão em `HBStream::Stop`: `m_quit = true`, thread movida e junção fora do lock; um `StartLocked` concorrente (`m_quit=false`, `m_worker` não joinable) cria um segundo worker `Pace` enquanto o antigo ainda não saiu.
- Além disso `SetStreamState(STOPPED)` (`:396-398`) não para worker nem hub: a thread de pacing a 30 Hz e o servidor de pipe continuam vivos com o stream parado (consumo e pipe aberto desnecessários).
- Correção: tornar `Stop` atômico (manter lock durante a troca de estado e usar um contador de geração; só zerar `m_stop` dentro do lock depois do join, ou usar um evento por geração), e fazer `SetStreamState(STOPPED)` parar worker/hub.

### M3. Ciclo `HBStream` ↔ `HBSource`, `ShutdownObject` vazio, vários `ActivateObject`
- `vcam_source.cpp:301-303` (`m_parent = parent`), `:603`, `:655-672`, `:813-822`, `:809`. **[INSPEÇÃO]**.
- O stream guarda `ComPtr` forte da fonte e a fonte guarda `ComPtr` forte do stream: só `Shutdown()` quebra o ciclo. Se o Frame Server liberar sem chamar `Shutdown` (queda, caminho de erro), a fonte, o stream, a thread `Pace` e a thread do servidor de pipe vivem para sempre dentro do serviço, o pipe fica "ocupado" por uma instância zumbi e `DllCanUnloadNow` nunca devolve `S_OK`. O mesmo ocorre no caminho de falha de `Initialize` (`:602`). O destrutor de `HBStream` com `m_worker` joinable chamaria `std::terminate` se o ciclo fosse quebrado sem `Shutdown`.
- `HBActivate::ShutdownObject()` retorna `S_OK` sem desligar a fonte criada (a ativação nem guarda a fonte). Se o Frame Server usar `IMFActivate::ShutdownObject` para encerrar, nada é encerrado.
- Cada `ActivateObject` cria um `HBSource` com seu próprio `FrameHub` no mesmo nome de pipe; duas fontes simultâneas disputam o pipe (um produtor cairia numa fonte "morta" que mostra NO SIGNAL).
- Correção: stream com ponteiro não-proprietário/fraco para a fonte (a fonte é dona do stream e o desliga no `Shutdown`); `HBActivate` guarda a fonte criada e a desliga em `ShutdownObject`; hub único por processo (singleton protegido) compartilhado pelas fontes.

### M4. Log em diretório gravável por "Todos"; crescimento sem limite
- `vcam_source.cpp:57-83` (`Log`), `src/app/windows/setup_main.cpp:238-240` (`icacls ... /grant *S-1-1-0:(OI)(CI)M`). **[INSPEÇÃO]**.
- `C:\ProgramData\HBStudio` recebe `Everyone:Modify`, e o serviço (LocalService) abre `vcam.log` por caminho, em modo append. Qualquer usuário local pode apagar/substituir o arquivo, criar link/reparse (por exemplo via links de objeto) e fazer um serviço privilegiado anexar texto onde ele escolher (conteúdo fixo, mas ainda é um primitivo de escrita privilegiada), ou forjar o log. Qualquer produtor mal-comportado também enche o disco: cada reconexão com cabeçalho inválido (`:221`) escreve uma linha, sem rotação nem limite.
- Correção: dar `LOCAL SERVICE` (`*S-1-5-19`) Modify e Users somente leitura (ou usar `%WINDIR%\ServiceProfiles\LocalService\AppData\Local\HBStudio`); abrir com `FILE_FLAG_OPEN_REPARSE_POINT` e verificar; limite de tamanho (ex.: 2 MB, rotação simples) e limite de taxa por mensagem.

### M5. `PipeFrameSender::stop()` pode bloquear para sempre
- `vcam_sender.cpp:58-67`, `:87-97`, `:129-131`. **[INSPEÇÃO]**.
- `WriteFile` síncrono num handle não-overlapped; `stop()` só pede parada e faz `join`. Se o Frame Server travar/for suspenso com o pipe cheio (buffer de 4 MB), a thread do remetente fica presa em `WriteFile` e `join` nunca retorna: `doDeactivate`/`shutdown()` (saída do app, `vcam_controller.cpp:286-289`) pendura.
- Correção: abrir com `FILE_FLAG_OVERLAPPED` e esperar `{evento de I/O, evento de parada}` com timeout, ou chamar `CancelSynchronousIo(thread_.native_handle())` em `stop()` antes do `join`.

### M6. Exceção do renderizador: consumidores ficam sem quadros e o contador mente
- `src/video/program_output.cpp:295-303`. **[SONDA]** (`misc.cpp renderer`): renderizador que lança a partir do 6º tick → `calls=30 consumer got=5 produced=30 rendererErrors=25`.
- O comentário diz "repeat the last frame", mas o código só incrementa `produced_` e retorna: os consumidores (câmera virtual, gravação) **não recebem nada** e a câmera virtual cai para NO SIGNAL após 1 s (`CopyLatest`, `:124`), enquanto `framesProduced()` e o FPS da UI continuam "normais". Sem quadro anterior, o fluxo cai no caminho normal e publica o conteúdo residual do buffer do pool.
- O teste `ThrowingRendererKeepsOutputAlive` (`tests/program_output_test.cpp:118-132`) é vazio quanto a isso: `out.latest()` é verdadeiro mesmo que nenhum quadro novo seja publicado.
- Correção: no `catch`, publicar `prev` (reenviar `latest_` aos consumidores com novo `index/pts`) e **não** contar `produced_`; contar um `repeatedFrames_` à parte. Ajustar o teste para exigir que o consumidor continue recebendo.

### M7. Instalador elevado: execução de binários por nome, nomes de conta localizados, falhas ignoradas, sem retry
- `src/app/windows/setup_main.cpp:78-89` (`runHidden`), `:237`, `:240`, `:273`, `:293`, `:179`, `:65-69`. **[INSPEÇÃO]**.
  1. `CreateProcessW(nullptr, L"icacls ...")`, `taskkill`, `cmd.exe` sem caminho: o Windows procura primeiro no **diretório do executável do instalador** (ex.: `Downloads`). Um `icacls.exe` plantado ao lado do `HBStudio-Setup.exe` é executado **elevado**. Usar `GetSystemDirectoryW` + caminho absoluto (`%SystemRoot%\System32\icacls.exe`).
  2. `"NT AUTHORITY\LOCAL SERVICE"` é nome, não SID; em Windows localizado (o público alvo é pt-BR) a resolução do nome pode falhar. Usar `*S-1-5-19`. O retorno de `runHidden` é **ignorado** (também o `WaitForSingleObject` com timeout de 20 s), então o instalador relata sucesso mesmo que a ACL não tenha sido aplicada.
  3. `running_` (`:179`) é colocado em `true` e **nunca volta a `false`**: depois de uma falha (por exemplo "Feche o HB Studio"), o botão "tentar de novo" é ignorado até reabrir o instalador.
  4. `writeResource` grava direto sobre `HBStudio.exe`/`hb_vcam_source.dll` (`CREATE_ALWAYS`): falha no meio deixa arquivo truncado; o aviso de `ERROR_SHARING_VIOLATION` (`:68`) é avaliado depois de `lastError()` chamar `FormatMessage`, que pode alterar o último erro; para a DLL a dica "feche o HB Studio" é enganosa (quem a segura é o Frame Server; `stopFrameServer` espera só 8 s e ignora falha de parada por serviços dependentes).
  5. O instalador elevado hospeda WebView2 (navegador completo) com o diretório de dados em `LocalAppData` do usuário que elevou. Ver M8: sem restrição de navegação, qualquer navegação para conteúdo externo roda dentro de processo elevado e a página pode enviar `install`/`uninstall`.
  6. Desinstalar: `registerDll(dll,false)` tem retorno ignorado e a DLL é apagada mesmo se o desregistro falhar (CLSID órfão em HKLM); `ProgramData\HBStudio` (com ACL `Everyone:Modify`) não é removido.

### M8. Host WebView2: sem trava de navegação, sem try/catch na ponte, `onReady` reexecutável
- `src/app/windows/webview_host.cpp:164-168`, `:171-180`, `:181-188`, `:189`; `src/app/windows/app_main.cpp:136-166`, `:180`, `:217`. **[INSPEÇÃO]**.
  1. A ponte `chrome.webview.postMessage("cmd|…")` dá acesso aos comandos do app (câmera virtual, switcher, gravação de config) a **qualquer página** carregada no controle. Não há `add_NavigationStarting` (cancelar tudo que não seja o documento embutido), `add_NewWindowRequested` (cancelar), nem `put_AreDevToolsEnabled(FALSE)`/`put_AreBrowserAcceleratorKeysEnabled(FALSE)`/`put_AreHostObjectsAllowed(FALSE)`. Um link, arrastar-e-soltar de URL ou redirecionamento leva a navegação externa com a ponte ativa.
  2. O handler de mensagens não protege contra exceções (`onMessage_(narrow(w))` dentro do callback COM). Exceção (`std::bad_alloc`, `std::system_error` de `std::thread` em `vcam.activate()`, etc.) atravessa a fronteira COM → término. Conflita com a regra 2 do CONTRIBUTING.
  3. `NavigationCompleted` chama `onReady_()` **a cada** navegação concluída (também F5/`location.reload()`). `app_main.cpp:217` usa `onReady` para `startFeeder()`, que faz `feeder = std::thread(...)`: atribuir a uma `std::thread` já joinable chama `std::terminate`. Um F5 (aceleradores do navegador ficam ligados por padrão) deve derrubar o app. Não pude executar o WebView2, então é inferência; basta tornar `onReady` idempotente (`std::call_once` ou checar `feeder.joinable()`).
  4. `dissolve|ms`: `startDissolve(idx(1) > 0 ? idx(1) : 500)` sem teto; a UI (ou uma página externa) pode enviar `INT_MAX` e deixar o switcher "em transição" por dias (bloqueia `cut`, `setPreview`, `setProgram`).
  5. `NavigateToString` tem limite de 2 MB de HTML (documentado pelo WebView2); `ui/index.html` tem ~28 KB, hoje OK, mas cresce.

### M9. Resolução incompatível é descartada em silêncio (sem log) nos dois lados
- `vcam_source.cpp:224-229` e `vcam_sender.cpp:124`. **[INSPEÇÃO]**.
- O servidor lê o quadro inteiro (até 64 MB já alocados) e depois o ignora com `continue` sem logar; o remetente também pula quadros ≠ 1920×1080 sem avisar. Um Program Output 720p ou 1080p60→25 fps produz uma câmera "NO SIGNAL" sem nenhuma pista, contrariando o próprio roteiro de diagnóstico de `docs/VIRTUAL_CAMERA.md`.
- Correção: validar `(width,height,stride)` contra as constantes **antes** de alocar/ler a carga; em divergência, logar uma vez e desconectar (ou fazer escala no remetente); expor `framesDropped` no `status()` do controlador.

### M10. Validação de cabeçalho com multiplicação de 32 bits
- `vcam_source.cpp:218-221`. **[INSPEÇÃO]**; aritmética verificável.
- `h.payloadBytes != h.stride * h.height` é multiplicação `uint32_t` (com wrap). Ex.: `stride=65536`, `height=65537` → produto 65536 (mod 2³²), `payloadBytes=65536` passa na verificação. A checagem seguinte (`:226-229`) barra por dimensão, então não há corrupção de memória hoje, mas a validação é ineficaz e `staging.resize(payloadBytes)` aloca até 64 MB por cabeçalho hostil antes de qualquer conferência semântica (CPU/memória de um serviço compartilhado). Use `uint64_t` e valide as dimensões antes (ver M9).

### M11. Sem sanitizers/TSAN em CI
- `.github/workflows/ci.yml`. **[INSPEÇÃO]**.
- O projeto é fortemente concorrente (relógio, consumidores, UI, pipe) e há diretório `build-tsan` local, mas a CI só roda a build Release normal. Os achados A5/M2/M6 teriam sido pegos por um job ASan/TSan no Linux.

### M12. Cadeia de suprimentos e publicação
- `.github/workflows/ci.yml` (download do WebView2 SDK via `Invoke-WebRequest` do nuget.org sem verificação de hash; `permissions: contents: write` no workflow inteiro; `gh release delete latest --cleanup-tag` seguido de nova release a cada push em `main`; instalador sem assinatura). **[INSPEÇÃO]**, severidade moderada: o instalador elevado e a DLL carregada pelo Frame Server saem direto da CI sem assinatura/verificação de integridade. Fixe o hash do pacote nuget, restrinja `contents: write` ao job de release e planeje assinatura de código.

---

## BAIXO

- **B1. `ProgramClock::start/stop` não são thread-safe entre si e `stop()` a partir do callback.** `src/core/clock.cpp:129-144`; `ProgramOutput::start` faz `if (running()) return;` (TOCTOU). **[INSPEÇÃO]** Em sonda, `stop()` chamado dentro de um consumidor "funcionou" por acidente (o `join` na própria thread lança `system_error`, capturado pelo `try/catch` do consumidor, e `running_` já tinha sido zerado). Documentar/proibir.
- **B2. `ProgramOutput::start` com fps inválido publica `program.started` mesmo sem iniciar.** `program_output.cpp:256-261` + `clock.cpp:131`. **[SONDA]** `fps 0/1: running=0, program.started published=1`.
- **B3. `mixFrames` não valida tamanhos iguais.** `src/video/frame_ops.hpp:20-30`. **[SONDA]** (ASan): `a` 64×64 e `b` 8×8 → `heap-buffer-overflow` (leitura). Hoje o `Switcher` dimensiona `scratchA_/B_` pelo `out`, então a precondição vale; vale um `assert`/retorno antecipado, e comparar também `stride`.
- **B4. `downscaleBox` com acumulador `unsigned` de 32 bits.** `frame_ops.hpp:41`. **[SONDA]** quadro 8192×4608 todo 255 reduzido a 1×1 → média 27 (esperado 255). 4096×4096 ainda dá 255 (limite ≈ 16,8 M pixels por célula). Use `uint64_t`. Também há `y * src.height / dh` em `int` (`:36`, `:39`), seguro nas resoluções atuais.
- **B5. `Switcher` com zero fontes → null deref.** `src/switcher/switcher.cpp:147-161`. **[SONDA]** `Switcher({})` + `renderProgram` → SEGV. Precondição não validada (o construtor só trata `size() < 2`).
- **B6. Transição "grudada" com saída parada.** `switcher.cpp:122-136`, `:170-178`. **[SONDA]** `startDissolve(100)` sem `renderProgram` em andamento: 300 ms depois `transitioning=1`, `cut()=0`, `setPreview=0`. Se o Program Output for parado durante uma transição, a UI fica sem cortes até reiniciar. Resolver a transição em `state()`/`cut()` por tempo, ou cancelá-la ao parar.
- **B7. `SyntheticSource` com número negativo indexa fora do array.** `src/video/synthetic_source.cpp:373,390`. **[SONDA]** (`UBSan/ASan`): `number_=-3` → `kGlyph[-3]`. Hoje o número é `i+1` do `Engine`; impor `number_ >= 1`.
- **B8. `Engine::renderUiFrame` aceita lixo depois do id** (`"s3garbage"` → fonte 3) e só funciona com `kSourceCount <= 10`. `src/app/engine.cpp:326-331`. **[INSPEÇÃO]**
- **B9. `setPreview` publica `pgm` lido em outra seção crítica.** `switcher.cpp:93`; o evento pode carregar um `pgm` que já mudou. **[INSPEÇÃO]**
- **B10. Injeção de linhas no log.** `Logger::log` não neutraliza `\n`/`\r` na mensagem (`logger.cpp:378-393`): um nome de câmera ou resposta de rede pode forjar linhas `[INFO]`. **[INSPEÇÃO]** (a redação não cobre isso).
- **B11. Codificação de fontes no MSVC.** `CMakeLists.txt` não passa `/utf-8` e os arquivos contêm texto pt-BR (`vcam_controller.cpp:218`, `engine.cpp:277`, `setup_main.cpp`, etc.). Sem `/utf-8` e sem BOM, o MSVC interpreta o fonte na página de código ANSI do host; em máquinas com outra página de código (ex.: 932/936) os literais viram lixo, e `win::widen()` (UTF-8) devolve U+FFFD. Em en-US/pt-BR (1252) costuma passar por acaso. **[INSPEÇÃO]** Adicionar `target_compile_options(... /utf-8)` globalmente.
- **B12. `PtzSettings`.** (a) `cleanName` corta o nome no 24º ponto de código **antes** dos bytes de continuação: 23×`a`+`ç` → saída com `C3` truncado (UTF-8 inválido gravado em disco) — **[SONDA]** `ptz_probe.cpp` (`src/app/ptz_settings.cpp:166-173`). (b) `save()` documentado como "atômico", mas faz `std::remove(path)` + `std::rename` (`:335-336`): se o processo morrer entre os dois, o arquivo some (no Windows `rename` não sobrescreve); use `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`/`ReplaceFileW`, e cheque erro após `close()`, não só `f.good()` antes. (c) `static_cast<int>(double)` de valores fora da faixa (`1e300`) é UB formal (a sonda não acusou; o `clamp` depois mascara). **[INSPEÇÃO]** Os testes do módulo não foram revisados.
- **B13. `EventBus::unsubscribe` não espera handlers em voo** (mesma família de A5) e `publish` copia todos os `std::function` a cada evento. `event_bus.cpp:242-264`. **[INSPEÇÃO]**
- **B14. Contrato de media source (nuances).** `vcam_source.cpp:673-711`: depois de `Stop()` → `Start()`, `m_streamSelected` é `false`, então envia `MENewStream` de novo para o mesmo stream em vez de `MEUpdatedStream`; reiniciar uma fonte já iniciada deveria gerar `MESourceSeeked`; `StartLocked` reinicializa o alocador (`:482`) mesmo com o stream já em execução enquanto o worker pode estar alocando; o `ConnectNamedPipe` cancelado (`:207`) não espera o `OVERLAPPED` da pilha completar antes de sair do escopo (`GetOverlappedResult(..., TRUE)`). `GetAllocatorUsage` não checa shutdown. **[INSPEÇÃO]**
- **B15. Ferramentas.** `tools/hb_vcam_poc.cpp`: a thread de entrada é `detach()`ada e referencia `enter` na pilha de `main`. `docs/VIRTUAL_CAMERA.md` ainda manda executar `install.ps1`/`uninstall.ps1` e baixar o artefato `hb-studio-vcam-poc`, que não existem mais depois do commit `292a88d` (instalador gráfico). `vcam_controller.cpp:307-313` chama `MFStartup` sem `MFShutdown`; `doActivate`/`doDeactivate` misturam apartamentos (câmera criada na thread MTA, desligada em outra/na UI STA) — se `IMFVirtualCamera` não for agile, isso é indefinido (não verificado).

---

## Testes

**Não verificam o que dizem / vacuosos**
1. `ProgramOutput.ThrowingRendererKeepsOutputAlive` (`tests/program_output_test.cpp:118`): ver M6; `EXPECT_TRUE(out.latest())` é verdadeiro mesmo com os consumidores sem receber nada.
2. `ProgramOutput.RemovedConsumerStopsReceiving` (`:105`): a tolerância `at + 1` esconde exatamente o problema A5.
3. `Engine.SwitcherChangesReachProgramAndLog` (`tests/engine_test.cpp:252`): `cut()` altera o estado e publica o evento de forma síncrona; o `sleep_for(150ms)` e o relógio não participam. Não prova que o **quadro** do programa mudou.
4. `Hardware.DetectReturnsSomething`: exige `ramBytes > 0`, falha em plataformas sem `/proc/meminfo`/Windows (macOS declarado como alvo em `ARCHITECTURE.md:470`).
5. `hb_vcam_selftest`: confere luma em dois pixels; não verifica plano UV/cores, `pitch != largura`, eco do token (`RequestSample(nullptr)` sempre), `SampleTime/Duration`, `Stop()`→`Start()`, `Shutdown` com worker ativo, cabeçalho inválido, segundo produtor, nem resolução incompatível.

**Flakiness provável em CI (temporização)**
- `Clock.DeliversRoughlyTheRequestedRate` (`tests/clock_test.cpp:180`): 60 fps por 500 ms com limite inferior 20 (de ~30). Em runner Windows (timer ~15,6 ms) ou máquina carregada pode cair abaixo, e o limite superior 32 pode ser excedido por rajada se o relógio não pular. Mais robusto: afirmar sobre `tickDeadline` e `stats().skippedTicks + ticks` em vez de contar callbacks por parede.
- `ProgramOutput.*` e `Engine.*` dormem 150-300 ms e exigem `> 5` quadros ou `pgm == 1` após dissolve de 200 ms em 500 ms (`tests/engine_test.cpp:265`). Margens estreitas em runner sobrecarregado; use espera ativa com prazo (poll até condição, timeout de 5 s).
- `Clock.SlowCallbackSkipsInsteadOfBursting`/`ThrowingCallback...`: dependem de `sleep_for` ≥ período; aceitáveis, mas dê folga.

**Cenários importantes sem teste**
- `redactSecrets`: nenhum teste negativo/adversarial (todos os vazamentos de A4), nenhuma entrada longa (A3), nenhum teste de `Logger` com mensagens grandes.
- Corrida de `removeConsumer`/`unsubscribe` sob TSAN (A5); `ProgramOutput::start/stop` repetido; `ProgramClock` reiniciado; `stop()` de dentro do callback; `tickDeadline` com `Rational` extremo (`num`/`den` ≈ 2³¹: `rem * den * kNs` estoura 64 bits; hoje `start()` só rejeita ≤ 0).
- `Switcher` com 0/1 fonte, dissolve com saída parada (B6), `mixFrames` com tamanhos diferentes, `downscaleBox` com quadros grandes (B4).
- Extrair para cabeçalho puro, testável no Linux, a lógica hoje presa em `vcam_source.cpp` (namespace anônimo): validação de `FrameHeader`, `BgraToNv12` (incluindo `pitch != largura`, UV) e a fila de tokens — hoje nada disso é testável fora do Windows.
- Protocolo do pipe: cabeçalho com magic errado, `payloadBytes` extremo, desconexão no meio do quadro, dois produtores, produtor lento (timeout).
- `PtzSettings`: nome com caractere multibyte no limite de 24, persistência com falha de escrita.
- CI: nenhum job ASan/UBSan/TSan.

---

## Pontos positivos (para equilibrar)
- A fórmula BT.709 limitada de `BgraToNv12` confere com os coeficientes teóricos (verifiquei Y, Cb, Cr); o empacotamento NV12 (U depois V entrelaçados, UV a metade da altura, mesmo `pitch`) está correto.
- `GetEvent` não segura o lock durante a espera bloqueante; `Shutdown` da fonte libera o lock antes de desligar o stream e junta as threads fora do lock; a ordem de travas fonte→stream é consistente (o stream nunca chama a fonte com o próprio lock).
- `PIPE_REJECT_REMOTE_CLIENTS`, `GetEvent`/`QueueEvent` com `MF_E_SHUTDOWN`, o perfil de sensor `Legacy` e a lista de interfaces (`IMFMediaSourceEx`, `IMFGetService`, `IKsControl`, `IMFSampleAllocatorControl`) seguem o contrato esperado do Frame Server.
- `tickDeadline` é livre de deriva e resiste a 10 dias a 60 fps; o `FramePool` com `shared_ptr<State>` permite que quadros sobrevivam ao pool.
- `EventBus` executa handlers fora do lock e isola exceções; `Logger` isola falhas de sink.

---

## Os 10 achados mais importantes
1. **A3** — `redactSecrets` quadrático (5 s para 10 mil caracteres) e SIGSEGV com ~50 mil (10 mil com pilha de 1 MB): trava ou derruba quem registra mensagens longas. [SONDA]
2. **A4** — `redactSecrets` vaza senhas com `@`/`/`/espaço, `Cookie`/`Set-Cookie`, `access_token`/`refresh_token`/`client_secret`, `?access_token=`/`?key=`/`?sig=`/`streamid`, JSON `"Authorization"`, valores com espaço/vírgula/`;`. [SONDA]
3. **A5** — `removeConsumer` não espera callbacks em voo: use-after-free ao desativar a câmera virtual. [SONDA]
4. **A1** — ACL do pipe (`GENERIC_WRITE` ⇒ `FILE_CREATE_PIPE_INSTANCE`) + sem `FIRST_PIPE_INSTANCE` + remetente sem verificar o servidor/SQOS: sequestro do vídeo e forja. [INSPEÇÃO]
5. **A2** — Um único slot de pipe sem timeout: qualquer usuário autenticado (ou um app travado) mantém a câmera ocupada. [INSPEÇÃO]
6. **A6/M1** — NV12: offset do plano UV presumido, falha silenciosa quando o buffer é pequeno, falta `SetCurrentLength`, e token de `RequestSample` perdido quando `AllocateSample` falha. [INSPEÇÃO]
7. **M6** — Exceção do renderizador faz os consumidores pararem de receber quadros (a câmera virtual cai para NO SIGNAL) enquanto os contadores seguem normais; o teste correspondente é vacuoso. [SONDA]
8. **M2/M3** — Corridas `FrameHub::Stop/Start`, ciclo stream↔fonte, `ShutdownObject` vazio e vários hubs no mesmo pipe: threads/pipe zumbis dentro do Frame Server. [INSPEÇÃO]
9. **M7/M4** — Instalador elevado: `icacls`/`taskkill` sem caminho absoluto (planting), nome de conta localizado, falhas ignoradas, `running_` sem reset; log em `ProgramData` com `Everyone:Modify` gravado pelo LocalService. [INSPEÇÃO]
10. **M8** — WebView2 sem trava de navegação nem try/catch na ponte, e `onReady` reexecutável (F5) que atribui a uma `std::thread` joinable (término do processo); `dissolve` sem teto. [INSPEÇÃO]
