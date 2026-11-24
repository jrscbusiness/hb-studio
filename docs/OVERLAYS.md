# Overlays dinâmicos (aba GRÁFICOS)

Gráficos desenhados **sobre a saída Program**: cronômetro/contagem regressiva, placar, QR Code (doação/PIX/link),
nome (lower third), legendas ao vivo e chat destacado. Por entrarem no quadro do Program, aparecem na câmera
virtual, na gravação, no NDI e na transmissão. Código em `src/overlay/` e `src/qr/`; testes em `tests/overlay_*.cpp` e `tests/qr_test.cpp`.

## Contrato de composição (dono: módulo overlay; reutilizável pelo compositor de cenas)

* `hb::overlay::Layer` (`layer.hpp`): `id`, `visible`, `zOrder`, `placement` (âncora normalizada x/y + pivô 0..1),
  `active(ctx)`, `render(Frame&, RenderContext&)`.
* `hb::overlay::LayerStack`: thread-safe (`add`/`remove`/`get`/`resort`), `renderOver(Frame&, ctx)` desenha em ordem
  crescente de z (empate: ordem de inserção). Camada que lança exceção é ignorada. Sem nenhuma camada ativa retorna
  sem tocar na memória do quadro.
* **Convenção de alpha** (`blend.hpp`): quadros do Program são **opacos** (alpha 255, preservado); bitmaps de camada
  são **alpha reto** (não pré-multiplicado). `out = (src*a + dst*(255-a) + 127)/255`, `a = alphaDoPixel * opacidade`.
  Sobre destino translúcido (montagem de cartões) usa-se o operador "over" geral de alpha reto.
* **Gancho**: `Engine::setPostRenderHook(fn)` é chamado na thread do relógio do Program logo depois de
  `switcher.renderProgram`. `OverlayManager::programHook()` devolve o callable (mantém só o estado de render vivo,
  não o gerenciador nem o bus). Opcional: `OverlayManager::renderPreview(frame)` desenha no Preview do tamanho do
  Program (janela de saída de Preview) quando a opção "Mostrar também no Preview" está ligada.
  O monitor PVW pequeno da UI **não** recebe gráficos (só o PGM, que já os contém).

## Tipos de elemento (nomes ESTÁVEIS: cenas/templates referenciam como `{type, params}`)

| `type` | O que é | Parâmetros principais (chaves JSON de `ElementConfig`) |
|---|---|---|
| `timer` | cronômetro, regressivo ou progressivo | `mode` down/up, `durationSec`, `format` auto/mm:ss/hh:mm:ss, `blinkAtZero`, `label` |
| `scoreboard` | placar de dois times | `teamA/B`, `colorA/B`, `scoreA/B`, `period`, `showClock` (+ campos do timer) |
| `qr` | QR Code | `qrText`, `qrCaption`, `qrEcc` L/M/Q/H, `qrSize` (fração da altura), `qrFg/qrBg` |
| `lowerthird` | nome + função | `title`, `subtitle` |
| `captions` | legendas ao vivo | `holdSec` (silêncio até sumir), `maxLines` (2), `widthPct` |
| `chatpin` | mensagem de chat destacada | `holdSec` (auto-ocultar; 0 = fixo), `maxLines`, `widthPct` |

Comuns: `id`, `name`, `x`,`y` (âncora 0..1), `pivotX`,`pivotY`, `z`, `size` (altura do texto como fração da altura do
quadro, independe da resolução), `font`, `bold`, `textColor`/`bgColor`/`accent` (inteiros 0xRRGGBB), `bgAlpha`, `opacity`,
`animIn`/`animOut` (`none`, `fade`, `slide-left|right|up|down`), `animMs`. Valores são validados e limitados (`sanitize`).
Um template de cena pode criar elementos com `ovsave|{json}` ou `OverlayManager::upsert(ElementConfig)`.

Persistência: `%LOCALAPPDATA%\HBStudio\overlays.json` (`{"version":1,"previewOverlay":bool,"elements":[...]}`),
gravada a cada alteração de configuração; entradas inválidas são ignoradas, as demais são mantidas. O estado
"no ar" **não** é persistido (tudo começa fora do ar).

## Ligação com legendas e chat (EventBus)

* `caption.text` `{text, final:"1"|"0", lang}`: resultado parcial (`0`) substitui a linha em andamento; final (`1`)
  é acrescentado ao texto corrente. Mostra no máximo `maxLines` linhas (as últimas), quebra automática na
  largura `widthPct`, some com fade após `holdSec` s de silêncio; fala nova após o silêncio recomeça do zero.
  Quem produz o evento é o módulo de legendas; aqui só se consome. O elemento precisa estar "NO AR".
* `overlay.chatPin` `{author, text, platform}`: mostra o cartão (liga o elemento sozinho) e oculta após `holdSec`.
  Todos os elementos do tipo correspondente recebem o evento.

## Protocolo UI ⇄ host

UI → host: `ovnew|<tipo>`, `ovsave|<json>` (id vazio cria), `ovdel|<id>`, `ovshow|<id>|1|0`,
`ovtimer|<id>|start|pause|reset|toggle|set|add[|segundos]`, `ovscore|<id>|a|b[|delta]` / `reset` / `swap`,
`ovperiod|<id>|<texto>`, `ovmove|<id>|up|down` (z), `ovpreview|1|0`.
Host → UI: `overlay_state` `{preview, elements:[{...config, clock, on, running, finished, qrOk}]}`, enviado a cada
comando, em `ready` e ~4x/s quando algo muda (relógios). A prévia da aba usa o quadro PGM (que já contém os gráficos).

## Texto

`ITextRasterizer` (medir + rasterizar UTF-8 em BGRA alpha reto, quebra de linha, alinhamento, contorno, sombra).
`MaskTextRasterizer` implementa tudo isso de forma portátil sobre um `IGlyphSource` mínimo (medir uma linha e
devolver a máscara de cobertura). Implementações: **GDI** (`windows/gdi_glyphs.*`; escolhido por exigir só
`gdi32`, sem COM nem DirectWrite, o menor risco sem poder compilar no Windows) e uma fonte bitmap 5x7 embutida
(dígitos, A-Z, pontuação; outros caracteres viram um glifo-caixa) usada nos testes e em builds não-Windows.
Cada elemento guarda o cartão já rasterizado e só o refaz quando texto/estilo/tamanho do quadro muda (cronômetro:
1x por segundo). Emoji: o GDI usa font linking e desenha glifos monocromáticos quando a fonte não os tem; emoji
colorido **não** é suportado.

## QR Code (`src/qr/`)

Codificador próprio (ISO/IEC 18004, modelo 2, versões 1-10, numérico/alfanumérico/byte, ECC L/M/Q/H, máscara por
penalidade). Verificação (sem leitor de QR disponível no ambiente): vetores conhecidos de Reed-Solomon
("HELLO WORLD" 1-M e o exemplo "01234567" da norma), bits de formato e versão, tabela de capacidades, e uma leitura
independente da matriz (geometria das áreas funcionais re-derivada no teste, formato por força bruta, desmascarar,
leitura em zigue-zague, síndromes RS = 0, payload decodificado) para todas as versões/ECC/máscaras. **Não foi lido
com câmera de celular.** Zona de silêncio de 4 módulos, fundo opaco.

## Desempenho (NÃO validado em hardware real)

* Sem elemento no ar: o gancho retorna sem tocar no quadro (custo ≈ copiar um shared_ptr).
* Com elementos: um alpha-blend por pixel do retângulo do cartão (nada de tela cheia). Estimativa por cálculo, não
  por medição: cartões típicos (timer, lower third) < 0,3 Mpx por quadro; QR/legendas maiores, ainda sem ultrapassar
  ~1 Mpx. Rasterização de texto (GDI) acontece na thread do Program só quando o texto muda (1x/s no cronômetro);
  a primeira exibição de um texto novo paga esse custo uma vez. Se isso se mostrar caro, o próximo passo é
  pré-rasterizar fora da thread do relógio (ponto de extensão: `Element::buildCard`).
* A chave do cache de cartão é montada por quadro enquanto o elemento está visível (alocação pequena).
* Limite de 32 elementos.

## Limitações conhecidas

* Código Windows (GDI, ligação em `app_main.cpp`, aba da UI no WebView2) não foi compilado/executado aqui; a CI do Windows é o verificador.
* Contorno do texto usa dilatação quadrada (cantos mais "quadrados" em contornos grossos).
* Legendas: o elemento precisa estar no ar; o idioma (`lang`) é ignorado na exibição.
