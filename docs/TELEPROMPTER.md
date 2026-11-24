# Teleprompter integrado

Texto de roteiro rolando na tela do **apresentador** (segundo monitor, tablet, celular na frente da câmera), controlado pelo operador na aba **TELEPROMPTER**.

## Uso rápido
1. Aba **TELEPROMPTER**: escolha/crie um roteiro (**NOVO**, **IMPORTAR .TXT**, **RENOMEAR**, **EXCLUIR**) e edite no campo de texto (salva sozinho).
2. **ABRIR APRESENTADOR** abre uma janela própria; arraste para o monitor do apresentador e use **F11**, duplo clique ou **TELA CHEIA** (Esc sai).
3. **PLAY/PAUSE**, **⏮** (volta ao início), velocidade, fonte, entrelinha, margens, cores (padrão: branco sobre preto), espelhar (H) para vidro/beam-splitter, inverter (V), linha de leitura, contagem regressiva.
4. Clique num parágrafo da visão do operador, ou na barra de progresso, para saltar até aquele ponto.

Roteiro: uma linha em branco separa parágrafos. Linhas que começam com `#` são **anotações do operador** (aparecem como etiqueta amarela na visão do operador; **não** vão para a janela do apresentador e não contam nas palavras/tempo). Atenção: é ocultação na tela, não sigilo — o texto completo é enviado à janela do apresentador, que só não o exibe.

Arquivos ficam em `%LOCALAPPDATA%\HBStudio\teleprompter\` (`<id>.txt` em UTF-8, `index.tsv` com os nomes, `settings.json` com estilo/velocidade). Importação aceita UTF-8, UTF-16 (BOM) e Windows-1252.

## Atalhos (somente na aba TELEPROMPTER)
| Tecla | Ação |
|---|---|
| Espaço | play/pause (**não** faz CUT nesta aba) |
| ↑ / ↓ | velocidade +10 / −10 |
| Home | volta ao início |

Os atalhos não disparam enquanto você digita no editor/campos. Na janela do apresentador valem as mesmas teclas, mais F11/duplo clique (tela cheia) e Esc. As demais teclas do switcher (1–8, Enter, Ctrl+1..0) continuam ativas nesta aba.

## Modelo de velocidade
Posição = fração 0..1 do roteiro (0 = primeira linha na linha de leitura; 1 = última linha passou). Duas unidades:
* **palavras/min (WPM)**: `px/s = wpm/60 · alturaDoTexto / totalDePalavras` (e o inverso). Tempo total = palavras/WPM.
* **px/s**: velocidade física; a fração por segundo é `px/s / alturaDoTexto`.

A altura do texto vem da janela do apresentador (ela informa `tp|layout`); sem ela, da visão do operador (que é só uma aproximação: a quebra de linhas muda com a largura). Trocar a unidade preserva a velocidade efetiva. A conversão supõe palavras distribuídas uniformemente na altura; linhas curtas e parágrafos fazem a velocidade local variar alguns por cento.

## Sincronização sem deriva
O **host (C++) é o dono** do estado (`src/teleprompter/`): envia `{pos, rate, state}` a cada mudança e um re-sync a cada 0,5 s enquanto roda. Operador e apresentador só extrapolam `pos + rate·Δt` entre mensagens (`requestAnimationFrame`, apenas `transform: translate3d`, sem layout por quadro), então ambos se realinham a cada mensagem. Limite: a latência do `postMessage` (ms) desloca a janela do apresentador de forma imperceptível, mas não foi medida em hardware real.

## Comandos (`tp|...`) — também usados pelo controle remoto
O texto é `tp|<cmd>|<args>` (até 4 partes separadas por `|`; a última preserva `|`).

- `tp|play`, `tp|pause`, `tp|toggle`, `tp|restart` — transporte (play em 0 com contagem > 0 inicia a contagem)
- `tp|speed|<n>` — velocidade na unidade atual (wpm 20–400 / px/s 10–800)
- `tp|seek|<0..1>` — pula para a fração do roteiro
- `tp|nudge|<±n>` — ajuste relativo de velocidade
- `tp|mode|wpm` / `tp|mode|pxs` — unidade de velocidade
- `tp|countdown|<0..30>` — segundos de contagem antes de começar
- `tp|select|<id>`, `tp|new|<nome>`, `tp|rename|<id>|<nome>`, `tp|delete|<id>`, `tp|save|<id>|<texto>`, `tp|import|<nome>|<texto>` — biblioteca de roteiros
- `tp|style|<chave>|<valor>` — `font`, `lh`, `margin`, `fg`, `bg` (`#RRGGBB`), `mirror`, `flip`, `marker` (0/1), `markerpos`
- `tp|presenter|open`, `close`, `fullscreen` — janela do apresentador
- `tp|layout|p\|o|<alturaTextoPx>|<alturaJanelaPx>`, `tp|hello` — usados pelas telas

O controle remoto (módulo `remote`) só repassa `play`, `pause`, `speed` e `seek`. Entradas inválidas (número não finito, cor malformada, id fora de `s<dígitos>`) são ignoradas; números são limitados às faixas acima.

Mensagens host → telas: `tp` (estado), `tplist`, `tpscript` (texto + blocos), `tpstyle`.

## Arquitetura
* `src/teleprompter/script.*` — análise do roteiro (parágrafos, anotações `#`, contagem de palavras, normalização de codificação).
* `scroll.*` — máquina de estados (Parado/Contagem/Rodando/Pausado/Fim) com relógio injetável, conversões WPM↔px/s, estimativa de tempo.
* `store.*` — biblioteca em disco (ids ASCII `s1…`, nomes em `index.tsv`: nunca viram nome de arquivo).
* `controller.*` — interpreta `tp|...`, estado/JSON, persistência de ajustes. Testado em Linux (`tests/teleprompter_*`).
* `presenter_page.*` — extrai a página do apresentador de `ui/index.html` (`<template id="tpPresenterPage">`).
* `src/app/windows/presenter_window.*` — janela nativa com **segundo WebView2** (mesma classe `WebViewHost`, opção `secondary` para fechar sem encerrar o app; mesma pasta de perfil = um único processo de navegador). Escolhido em vez de uma janela GDI porque reaproveita o mesmo caminho de tela cheia, mensagens e renderização por CSS já testados.

## Limites (honestidade)
* **Sem rolagem por voz** (seguir a fala do apresentador). Exigiria reconhecimento de fala em tempo real (ex.: um motor offline como Vosk/Whisper-streaming) casando o texto reconhecido com o roteiro, mais tolerância a improviso; não existe nesta versão.
* A janela do apresentador usa o **WebView2 como segundo controle**; o código Windows (`presenter_window.*`, mudanças em `webview_host.*`/`app_main.cpp`) **não foi compilado nem testado em Windows nesta sessão** — a CI do Windows é quem valida. A lógica portátil (testes gtest) e a UI (Playwright: operador e página do apresentador) foram testadas em Linux.
* Não validado em tablet/celular nem em vidro beam-splitter real; o espelhamento é CSS (`scaleX(-1)`).
* O apresentador em outro dispositivo (celular via rede) não existe: só janela local no mesmo PC.
* A visão do operador aproxima a quebra de linhas da janela real; salto por parágrafo usa a posição física na visão onde se clicou.
