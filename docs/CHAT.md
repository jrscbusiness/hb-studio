# Chat unificado (aba CHAT)

Uma única caixa de mensagens que junta o chat ao vivo de **YouTube, Facebook, Instagram e Twitch**. O operador vê tudo numa lista, filtra, silencia usuários **só localmente** e clica numa mensagem para **destacá-la no ar** (elemento "Chat fixado" da aba GRÁFICOS).

> Leia a seção [O que NÃO foi validado](#o-que-não-foi-validado). Nada aqui foi testado contra os serviços reais: o desenvolvimento não teve acesso a contas, projetos Google/Meta nem a um Windows.

## Resumo honesto por plataforma

| Plataforma | O que o app faz | Precisa de | Observação |
|---|---|---|---|
| **YouTube** | Consulta `liveChatMessages.list` respeitando `pollingIntervalMillis` | Projeto no Google Cloud, Client ID OAuth próprio, login da conta | Gasta cota da API (veja abaixo) |
| **Facebook** | Consulta `/{id-do-video}/comments` a cada 5 s | App Meta + token de acesso de **Página** | Só vídeos de Página; revisão do app para uso público |
| **Instagram** | **Indisponível / limitado.** Não há leitura por consulta; só o webhook `live_comments` | Endpoint HTTPS público + revisão do app Meta | O app não hospeda esse endpoint; a UI mostra "LIMITADO / INDISPONÍVEL" |
| **Twitch** | IRC anônimo (somente leitura) sobre WebSocket seguro | Só o nome do canal | Não envia mensagens, não usa login |

## O que você precisa configurar

### YouTube (OAuth 2.0 de aplicativo instalado, loopback + PKCE)

O HB Studio **não traz** um Client ID embutido: você usa o seu.

1. No [Google Cloud Console](https://console.cloud.google.com/), crie (ou escolha) um projeto.
2. Em *APIs e serviços > Biblioteca*, ative a **YouTube Data API v3**.
3. Em *Tela de permissão OAuth*: tipo **Externo**, preencha nome e e-mail. Adicione o escopo `https://www.googleapis.com/auth/youtube.readonly` e inclua a **sua conta Google como usuário de teste**.
4. Em *Credenciais > Criar credenciais > ID do cliente OAuth*, escolha o tipo **App para computador (Desktop)**. Copie o **Client ID**. O Google costuma fornecer também um *client secret* para esse tipo: ele não é secreto de verdade (aplicativos de computador não conseguem guardá-lo), mas o endpoint de token pode exigi-lo. Cole-o no campo opcional se o Google o mostrar.
5. No HB Studio, aba CHAT > YouTube > CONFIGURAR: informe o **ID ou link do vídeo ao vivo** e o **Client ID**, clique em SALVAR e depois em **AUTORIZAR**. O navegador abre a página de consentimento do Google; ao concluir, o Google redireciona para `http://127.0.0.1:<porta>` (um servidor local, só acessível desta máquina, que vive até 3 minutos) e o app troca o código por tokens (PKCE S256 + parâmetro `state` verificado).
6. Clique em CONECTAR.

Limitações reais:

- **Aplicativo em modo "Teste": o refresh token expira em 7 dias** (regra do Google para apps não publicados). Depois disso a UI mostra "PRECISA AUTORIZAR" e você repete o passo AUTORIZAR. Publicar o app exige verificação do Google para escopos sensíveis; para uso pessoal, manter em Teste e reautorizar semanalmente é o caminho simples.
- O escopo é somente leitura. O app **não** envia mensagens ao chat.
- Se o vídeo ainda não está ao vivo, o app espera (nova consulta a cada 20 s, 1 unidade de cota cada) até aparecer um chat ativo.
- Não usamos `liveChatMessages.streamList` (streaming gRPC/HTTP): só consulta periódica.
- Se o chat da transmissão de outro canal pode ser lido por sua conta: **não validado**.

#### Cota da API (impacto diário)

A cota padrão de um projeto é de **10.000 unidades por dia** (renova à meia-noite, horário do Pacífico), compartilhada por tudo que usa o mesmo projeto. Segundo a documentação do YouTube, cada chamada `liveChatMessages.list` custa **5 unidades**; `videos.list` (para achar o chat do vídeo) custa 1 e é feita uma vez por conexão.

O intervalo entre consultas é o que o servidor manda em `pollingIntervalMillis` (o app nunca vai abaixo de 1 s nem acima de 60 s). Exemplos de conta, supondo o intervalo fixo:

| Intervalo do servidor | Consultas/hora | Unidades/hora | Duração de 10.000 un. |
|---|---|---|---|
| 5 s | 720 | 3.600 | cerca de 2 h 45 min |
| 10 s | 360 | 1.800 | cerca de 5 h 30 min |

Qual intervalo o YouTube realmente devolve varia e **não foi medido aqui**. A UI mostra a cota estimada gasta na sessão (soma de 5 por consulta + 1 por busca de chat). Ao receber `quotaExceeded` o app para de consultar por 30 minutos e avisa. Para transmissões longas, peça aumento de cota no Google Cloud Console.

### Facebook

1. Crie um app em [developers.facebook.com](https://developers.facebook.com/) (tipo *Empresa/Business*).
2. Gere um **token de acesso de Página** (Graph API Explorer ou fluxo de login) com as permissões de leitura de engajamento da Página (em geral `pages_read_engagement` e `pages_show_list`; **confirme a lista atual na documentação da Meta**, ela muda). Prefira um token de longa duração da Página.
3. Aba CHAT > Facebook > CONFIGURAR: informe o **ID do vídeo ao vivo** e cole o token. O token é guardado criptografado e nunca é mostrado de volta (só `••••` + 4 últimos caracteres).

Limitações reais:

- Funciona para transmissões de **Página**. Transmissões em perfil pessoal praticamente não são expostas pela Graph API.
- Enquanto o app Meta está em modo de **Desenvolvimento**, só funciona para pessoas com função no app (admin/desenvolvedor/testador). Para uso por terceiros o app precisa de **Revisão de App** (e, conforme a permissão, verificação de empresa).
- A Graph API pode **omitir o autor** de comentários (privacidade). Nesses casos a UI mostra "Usuário do Facebook".
- Consultamos `/comments` (ordem cronológica reversa, `live_filter=no_filter`, `limit=100`) a cada 5 s. Mais de 100 comentários em 5 s perdem os mais antigos. Não usamos o streaming por eventos (`live_comments` em `streaming-graph.facebook.com`).
- O token vai na query string (convenção da Graph API); o app nunca registra URLs.
- A Graph API não avisa que a live acabou: o app continua consultando até você desconectar.

### Instagram: limitado / indisponível

O que a documentação oficial da Meta oferece para comentários de **Instagram Live** é o campo de **webhook `live_comments`** (contas Business/Creator, permissão `instagram_manage_comments` e correlatas conforme o tipo de login). Não existe endpoint de consulta (polling) para ler o chat ao vivo.

Um webhook exige um **endpoint HTTPS público** que a Meta chama; um aplicativo de desktop atrás de NAT não é um receptor viável, e o app também precisa passar por revisão da Meta. Por isso a aba mostra o Instagram como **LIMITADO / INDISPONÍVEL**, com explicação, em vez de fingir uma conexão.

O que existe no código: um parser testado do payload `live_comments` e o método `ChatService::ingestInstagramWebhook(body)`, para o dia em que um relay (um serviço HTTPS seu que receba o webhook da Meta e repasse ao app) for adicionado. **Não há receptor nem relay**, e a verificação da assinatura `X-Hub-Signature-256` também não foi implementada (seria responsabilidade do relay). O formato exato do payload foi escrito a partir da documentação, sem teste com um webhook real.

### Twitch

Só informe o canal. O app conecta anonimamente em `wss://irc-ws.chat.twitch.tv:443` com um nick `justinfan<número>`, pede as capacidades `twitch.tv/tags` e `twitch.tv/commands` e entra em `#canal`. Não há token, não há login e **não é possível enviar mensagens**. O app responde `PING` com `PONG`, reconecta com espera exponencial (2 s até 60 s), atende `RECONNECT` e derruba conexões sem nenhuma atividade por 7 minutos. Emotes e badges são interpretados (`emotes`, `badges`), mas imagens de emotes **não** são baixadas nem exibidas.

## Como funciona na tela

- Lista unificada com chip colorido por plataforma, autor, texto e hora. Selo DONO/MOD quando a plataforma informa.
- Filtros: plataforma (chips), ocultar comandos (`!algo`), ocultar bots conhecidos (lista fixa: Nightbot, StreamElements, Streamlabs, Moobot, Fossabot, WizeBot...), busca por autor/texto.
- **Clique numa mensagem** para destacá-la no ar. **LIMPAR DESTAQUE** tira o destaque.
- **⊘** ao passar o mouse silencia o usuário (apenas localmente; nada é enviado às plataformas). A lista de silenciados fica no painel de conexões e é guardada em `chat.json`.
- **PAUSAR ROLAGEM** congela a rolagem automática; **COMPACTO** esconde o painel de conexões para janelas pequenas.
- Por plataforma: estado (conectado, reconectando, precisa autorizar, erro, encerrado, limitado), explicação, último erro e CONECTAR/DESCONECTAR.
- O texto das mensagens é inserido sempre com `textContent`: HTML vindo do chat nunca é interpretado. Avatares não são exibidos nem baixados.

## Contratos com os outros módulos (EventBus)

| Tópico | Campos | Quem publica |
|---|---|---|
| `chat.message` | `platform` (`youtube`\|`facebook`\|`instagram`\|`twitch`), `author`, `text`, `id`, `ts` (ms desde 1970, texto) | este módulo, a cada mensagem aceita (nova, não duplicada, de usuário não silenciado) |
| `overlay.chatPin` | `author`, `text`, `platform` | este módulo, ao clicar numa mensagem |

- **Limpar destaque** publica `overlay.chatPin` com `author`, `text` e `platform` **vazios**. O módulo de overlays atual **ignora** `text` vazio (`src/overlay/manager.cpp`), então o botão não tem efeito visível ali: o elemento "Chat fixado" some sozinho pelo tempo de exibição (`holdSec`) ou pelos controles da aba GRÁFICOS. Quando o overlay tratar `text` vazio como "esconder", o botão passa a funcionar sem mudança aqui.
- O histórico de mensagens antigas que o YouTube/Facebook devolvem na **primeira** consulta é descartado por padrão (`skipBacklog`), para que automações ligadas a `chat.message` não reajam a conversas antigas.

Comandos UI para host (`mensagem|arg|resto`): `chatget`, `chatsave|<plataforma>|<json>`, `chatclear|<plataforma>`, `chatconnect|<plataforma>`, `chatdisconnect|<plataforma>`, `chatauth|youtube`, `chatauthcancel`, `chatpin|<n>`, `chatunpin`, `chatmute|<n>`, `chatunmute|<chave>`, `chatclearfeed`, `chatopt|persist|0/1`. `<n>` é o número sequencial da mensagem atribuído pelo host (nunca texto vindo do chat).

Host para UI: `chat_status` (substitui o anterior), `chat_settings`, e `chat_msgs` em lotes numerados (`seq`) de até 50 mensagens a cada 250 ms. Numa enxurrada, as mensagens mais antigas do lote são descartadas da **tela** (contador `dropped`); o buffer na memória e o `chat.message` não perdem nada. Um salto de `seq` faz a UI pedir o histórico de novo (`chatget`).

## Privacidade e segredos

- Mensagens ficam **só na memória** (anel das últimas 500). Salvar em arquivo é **opt-in** (caixa na aba, desligada por padrão): grava `chat_log.jsonl` em `%LOCALAPPDATA%\HBStudio`, **em texto puro**, uma mensagem por linha.
- Configurações (IDs de vídeo/canal, Client ID, silenciados) ficam em `%LOCALAPPDATA%\HBStudio\chat.json`, **sem segredos**.
- Segredos (refresh token do Google, client secret, token da Página) ficam em `%LOCALAPPDATA%\HBStudio\chat_secrets\*.bin`, criptografados por usuário com **DPAPI** do Windows. A UI só recebe `••••` + 4 últimos caracteres. O access token do Google vive só na memória. Mensagens de erro passam por `redactSecrets`.
- "REMOVER CREDENCIAIS" apaga os arquivos locais; a concessão continua listada em *myaccount.google.com/permissions* até você revogá-la lá.
- Fora do Windows não há armazenamento persistente de segredos nem transporte de rede: os adaptadores informam "rede indisponível".

## Arquitetura (para quem for mexer)

`src/chat/`: `types` (modelo normalizado), `irc` (parser IRCv3/Twitch), `parsers` (JSON do YouTube/Graph/webhook), `oauth` (PKCE, URLs, `GoogleTokenManager`), `loopback` (listener 127.0.0.1), `sources` (`IChatSource`: `YouTubeSource`, `FacebookSource`, `InstagramWebhookSource`, `TwitchSource`, com `Backoff` e relógio injetado em `poll(now)`), `hub` (dedupe `(plataforma,id)`, anel, silenciar, EventBus, lotes), `settings`, `service` (threads por plataforma + comandos), `net` (interfaces `IChatHttp`, `IChatSocket`, `ISecretStore`; implementações Windows em `net_win.cpp`). Interface de segredos própria e mínima, de propósito: o coordenador unifica depois com a de `src/ai/`.

Backoff de erros transitórios: YouTube 3 s a 120 s, Facebook 5 s a 300 s, Twitch 2 s a 60 s, dobrando a cada falha; cota esgotada espera 30 min; limite de taxa do Facebook espera 2 min.

## O que foi validado

Testes unitários (`tests/chat_*.cpp`, GoogleTest, sem rede real, transportes falsos, relógio injetado) cobrem: parser IRC (tags, escapes, emotes, badges, ACTION, linhas malformadas), parsers JSON do YouTube/Facebook/webhook, ISO-8601, sanitização UTF-8, PKCE (vetor do RFC 7636), URL de autorização, parser do redirect, gestão de tokens (troca, renovação, cache, `invalid_grant`), o listener loopback real em 127.0.0.1 (Linux), cada adaptador (intervalo do servidor, backoff, cota, erros permanentes/transitórios, 401, backlog, dedupe), o hub (dedupe, anel, silenciar, publicação no EventBus, lotes e enxurrada), configurações e o serviço de ponta a ponta com transportes falsos. Os testes da UI (`tools/ui/ui_test.js`) cobrem a aba (filtros, clique para destacar, silenciar, pausa, compacto, ausência de overflow a partir de 1024x720, ausência de interpretação de HTML).

## O que NÃO foi validado

- **Nenhuma chamada real** a YouTube, Facebook, Instagram ou Twitch. Formatos de resposta, nomes de campos, códigos de erro e custos de cota vêm da documentação pública e de conhecimento prévio, não de execução.
- **Todo o código Windows** (`net_win.cpp`: WinHTTP, WebSocket do WinHTTP, DPAPI; ramo Winsock de `loopback.cpp`; trecho do `app_main.cpp`) foi escrito sem compilador Windows. O CI do Windows e um teste manual são a verificação.
- O fluxo OAuth completo no navegador, a renovação após a expiração real do token e o comportamento "app em Teste, 7 dias".
- Permissões e revisão do app Meta, e o formato real do webhook `live_comments`.
- Se o `ShellExecute` abre o navegador padrão corretamente e como o WebView2 reage ao tráfego de ~4 mensagens/s.
