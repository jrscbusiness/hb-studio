# CONTROLE REMOTO (celular/tablet como segunda tela)

Módulo: `src/remote/` (biblioteca `hb_remote`, portátil — compila em GCC e MSVC) + a página móvel
`src/remote/mobile_page.html` (embutida no executável, sem CDN) + o cartão **Controle remoto** na aba
CONFIG. Testes: `tests/remote_*_test.cpp` (sockets reais em 127.0.0.1) e `tools/ui/remote_ui_test.js`
(navegador real contra o servidor real, via `hb_remote_sim`).

> **Estado de validação:** o servidor, o protocolo WebSocket, o pareamento, a lista de comandos
> permitidos e a página móvel foram testados **no Linux** (GCC, Chromium headless). O código
> específico do Windows (`tcp_stream_win.cpp`, `random_win.cpp`, `lan_win.cpp` e a ligação em
> `app_main.cpp`) foi escrito **sem compilador Windows** e **não foi validado**: o CI do Windows é a
> primeira compilação. **Nada foi testado em celulares/tablets reais, em roteadores reais nem com o
> firewall do Windows real.** Use como "não validado em hardware real" até alguém testar.

## Como usar
1. CONFIG → **Controle remoto** → **LIGAR**. O cartão mostra o(s) endereço(s) (ex.:
   `http://192.168.0.10:8787/`) e um **PIN de 6 dígitos**.
2. No celular/tablet (mesmo Wi-Fi), abra o endereço no navegador, digite o PIN e um nome para o aparelho.
3. Pronto: VÍDEO (PVW/PGM, 16 fontes em 2 bancos, CUT, AUTO, T-bar), ÁUDIO (faders, mute, solo) e
   SAÍDA (REC, destinos de transmissão, teleprompter, miniaturas).

Sempre **desligado ao abrir o programa** (o estado "ligado" nunca é salvo). Ao desligar, todos os
aparelhos são desconectados e as chaves, apagadas.

## Segurança (o que existe e o que NÃO existe)
**Limite principal: é HTTP/WS puro, sem TLS.** Na rede local não há certificado válido que um celular
aceite sem aviso. Quem estiver na mesma rede e puder capturar pacotes (Wi-Fi aberto, rede
comprometida) pode ver o tráfego, **incluindo o PIN digitado e a chave do aparelho**. Use em rede de
confiança (a do estúdio/igreja), nunca em Wi-Fi público. Mitigações:
- **Só rede local:** abre soquetes apenas nos endereços IPv4 privados das placas de rede (nunca
  `0.0.0.0`) e ainda recusa conexões cuja origem não seja privada/loopback/link-local.
- **Sem acesso sem autenticação:** o endereço só serve a página (sem dados). Todo o resto exige
  pareamento por PIN ou chave de sessão.
- **PIN:** 6 dígitos de um CSPRNG do sistema (`getrandom` / `BCryptGenRandom`), uso único (cada
  pareamento gera um PIN novo). **Anti-força-bruta:** 5 PINs errados queimam o PIN (troca por outro) e
  bloqueiam o pareamento por 30 s, 60 s, 120 s… (teto de 15 min) — mesmo o PIN certo é recusado
  durante o bloqueio. 3 tentativas por conexão.
- **Chave por aparelho:** 32 bytes aleatórios; o servidor guarda só o SHA-256; comparações em tempo
  constante. **Sessões curtas:** expiram em 4 h sem uso e em 12 h no máximo (precisa parear de novo).
  As chaves ficam só na memória: reiniciar o HB Studio desconecta todos.
- **Origem:** o upgrade WebSocket exige `Origin` igual ao `Host` (bloqueia sites maliciosos abertos no
  navegador do operador) e o `Host` precisa ser um IP ou `localhost` (bloqueia DNS rebinding).
- **Lista fechada de comandos** (`src/remote/commands.cpp`): `cut`, `auto` (100–5000 ms),
  `pvw`/`pgm` (0–15), `tbar` (0–1), `mute`/`solo`/`fader` (canais 0–15, fader até +6 dB), `rec`
  start/stop, `xmit` iniciar/parar **um** destino, `tp` (`play|pause|speed|seek`, para o teleprompter).
  Cada comando é **reconstruído** a partir de valores tipados e validados: texto do cliente nunca é
  repassado (sem injeção de `|`). Não há como editar destinos, mudar formato, instalar plugin etc.
- **Sem segredos no caminho:** `RemoteState` (`src/remote/state.hpp`) não tem campo para chave de
  transmissão, URL de servidor ou caminho de arquivo; só nomes, estados e níveis.
- **Controle de exibição:** a página usa `textContent` (nunca `innerHTML`) e CSP restritivo; nome do
  aparelho é sanitizado.
- **Limites de abuso:** cabeçalho HTTP ≤ 8 KB, mensagens do cliente ≤ 4 KB, no máximo 12 conexões
  (6 por IP, 8 sem autenticar), 60 comandos/s por aparelho, timeouts contra conexões lentas
  (slowloris), frames WebSocket mascarados/RSV/opcodes/UTF-8 validados (RFC 6455).
- Painel "aparelhos conectados" com **EXPULSAR**, e **DESCONECTAR TODOS** (também troca o PIN).

## Latência e robustez
- A thread do feeder (vídeo) **nunca toca em soquete**: só guarda o *último* estado/miniatura numa
  caixa protegida por mutex curto. Cada cliente lembra o que já viu; cliente lento simplesmente pula
  versões intermediárias (descarte por estouro). Cliente sem progresso de envio por 4 s é desconectado.
  Testado: um cliente que para de ler não atrasa o `broadcast*` (< 50 ms medido) nem os outros clientes.
- Estado: ≤ 10 Hz por cliente e só quando mudou (o app o monta a ~7,5 Hz, apenas com cliente
  conectado). Miniaturas: **desligadas por padrão, opt-in por aparelho**; 240×135 JPEG q55 a ~2 fps,
  teto de 100 KB/s por cliente.
- Comandos rodam numa thread própria (fila limitada) e chamam o **mesmo** `onMessage` que as mensagens
  do WebView — nenhuma lógica duplicada.
- A página reconecta sozinha (espera crescente até 8 s) e mostra faixa "Conexão perdida".

## Windows: firewall e rede
Na primeira vez que se liga o controle remoto o Windows mostra o aviso do Firewall: marque **Redes
privadas** e "Permitir acesso". Se negar/fechar, o celular não conecta (ajuste depois em *Segurança do
Windows → Firewall → Permitir um aplicativo*). Se a rede estiver marcada como **Pública**, o Windows
bloqueia por padrão; troque o perfil da rede para Privada.

## Limites honestos
- Redes de convidados, hotspots de operadora, repetidores com **"isolamento de clientes/AP isolation"**
  e VLANs separadas bloqueiam celular→computador; nesse caso não há o que o HB Studio possa fazer.
- Só IPv4. Se o computador tiver várias redes, todas as privadas são listadas (use a do Wi-Fi do celular).
- O QR code de pareamento **não** foi implementado (outro módulo é dono do gerador de QR em C++);
  o cartão mostra URL e PIN como texto. Nenhuma biblioteca de QR foi incorporada.
- Os comandos `tp|...` são só repassados: quem os trata é o módulo de teleprompter (outro agente).
  Enquanto ele não existir, são ignorados em silêncio pelo despachante.
- A lista de fontes mostra os 16 slots do mixer; o app monta o estado a ~7,5 Hz com
  `AudioEngine::snapshot()` (cópia completa); com muitos efeitos/dispositivos isso pode ser mais caro
  que o necessário — não medido no Windows.
- SHA-1 próprio (`sha1.cpp`) só serve ao handshake do WebSocket (exigido pela RFC); nada de
  segurança depende dele.

## Protocolo (resumo)
`GET /` → página. `GET /ws` (upgrade) → JSON em texto:
cliente `{"t":"pair","pin","name"}` | `{"t":"auth","token"}` | `{"t":"cmd","c":...}` |
`{"t":"thumbs","on":bool}` | `{"t":"p"}`; servidor `hello` | `paired{token}` | `pairfail` | `authfail` |
`s{...estado}` | `err{e}` | `pong`; miniaturas = frames binários `[0=PVW|1=PGM][JPEG]`.
