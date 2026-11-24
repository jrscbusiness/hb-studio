# Templates prontos e cenas (banco CENAS)

Cenas combinam várias entradas (câmeras, slides, tela compartilhada) em um único layout, com fundo,
bordas, cantos arredondados e sombra. Uma cena entra na mesa de corte como qualquer câmera:
**CUT, DISSOLVE e T-bar funcionam sem tratamento especial**, e PVW/PGM, miniaturas, gravação,
transmissão, câmera virtual e NDI mostram a cena porque ela é uma fonte (`SceneSource`) da mesa.

## Como usar (operador)

1. Em PRODUÇÃO, clique em **TEMPLATES** (ou num quadro "CENA n — VAZIA" do banco CENAS).
2. Escolha um template (Culto, Aula, Podcast, Evento, Meus). A miniatura usa cores de espaço reservado: não precisa de câmera.
3. **Mapear papéis**: cada papel ("Pregador", "Slides"...) recebe uma entrada. O HB Studio sugere as primeiras
   câmeras conectadas; entradas marcadas "TESTE" mostram o padrão de teste. Desmarque as cenas que não quiser.
4. **CRIAR CENAS**: as cenas vão para as primeiras posições livres do banco CENAS (botões roxos à esquerda dos quadros;
   2 fileiras de 8, teclas 1–8 como nas câmeras, rotuladas C1–C8). Selecione no PREVIEW e use CUT/DISSOLVE.
5. Botão direito numa cena → **Editar cena**: arraste uma camada para movê-la, puxe o canto inferior direito para
   redimensionar, ou use os campos numéricos (posição, tamanho, ajuste, opacidade, cantos, borda, sombra, ordem, fonte e fundo).
   Também é possível renomear, excluir (pede segundo clique) e **Salvar como meu template**.

Limite: 16 cenas. Se um template não couber, nada é criado e o app avisa quantas posições faltam.

## Arquivos

* `%LOCALAPPDATA%\HBStudio\scenes.json` — cenas atuais e "meus templates" (gravação atômica; arquivo ilegível é ignorado).
* `resources/templates/builtin.json` — biblioteca embutida (gerada por `tools/gen_builtin_templates.py`, embutida no executável em tempo de build).

## Formato de template (compartilhável)

Um arquivo `.json` com `"format":"hb-templates"` e `"version":1`. **Exportar…**/**Importar template…** na tela de
templates trabalham com este formato (será o formato de distribuição do marketplace de plugins). Importar nunca
sobrescreve um template embutido (o id vira `u-<id>`). Limites: 1 MB, 256 itens por lista.

```json
{
  "format": "hb-templates", "version": 1,
  "templates": [{
    "id": "meu-culto", "name": "Meu culto", "category": "culto",
    "description": "Pregador com slides em PiP",
    "roles": ["Pregador", "Slides"],
    "scenes": [{
      "id": "pip", "name": "Slides + PiP",
      "background": {"type": "gradient", "from": "#0b1220", "to": "#1c2f55", "angle": 90},
      "layers": [
        {"role": "Slides", "rect": [0, 0, 1, 1], "fit": "fit"},
        {"role": "Pregador", "rect": [0.74, 0.70, 0.24, 0.24], "fit": "fill", "radius": 0.08,
         "border": {"width": 0.004, "color": "#ffffff"},
         "shadow": {"blur": 0.02, "dx": 0.004, "dy": 0.008, "opacity": 0.55}, "z": 1}
      ],
      "overlays": [{"type": "lowerthird", "text": "{Pregador}"}]
    }]
  }]
}
```

* `category`: `culto`, `aula`, `podcast`, `evento` ou `meus` (outras viram abas só em "Todos").
* `roles`: papéis que o operador mapeia a entradas. Papéis usados por camadas são adicionados automaticamente.
* **Cena**: `background` (`solid` com `color`; `gradient` com `from`, `to`, `angle` em graus, 0 = esquerda→direita, 90 = cima→baixo;
  `image` com `path` de um **BMP** 24/32 bits sem compressão e `color` de reserva), `layers` (até 16), `overlays` (dados puros).
* **Camada**: `role` e/ou `slot` (0–15; `-1` = painel de espaço reservado), `rect` `[x,y,w,h]` normalizado 0–1 do quadro
  (um retângulo com `w == h` é 16:9 num quadro 16:9), `fit` (`fit` mostra tudo com barras, `fill` cobre cortando, `crop` com `zoom` 1–4 e `crop` `[ax,ay]`),
  `radius` (fração do lado menor, 0–0,5), `border` (`width` em fração da altura do quadro), `shadow` (`blur`, `dx`, `dy` em fração da altura; `opacity`),
  `opacity`, `z` (ordem de desenho).
* **Overlays**: `{"type": "...", ...}` são apenas dados (ex.: `lowerthird`, `title`, textos com `{papel}`). Esta versão **não os desenha no vídeo**:
  o coordenador ligará esses presets ao motor de overlays depois da integração. Na prévia do editor aparece só uma dica visual.
* Valores fora dos limites são ajustados; JSON inválido, `format`/`version` errados (ou versão mais nova que 1) ou mais de 16 camadas são recusados com mensagem.

## Comandos UI → host (`window.chrome.webview.postMessage`, campos separados por `|`)

| Comando | Efeito |
|---|---|
| `sceneapply\|<templateId>\|<roleMapJson>[\|<idsDeCenaCsv>]` | cria as cenas; `roleMapJson` = `{"Pregador":0,"Slides":3}` (slots 0-based) |
| `scenesave\|<cenaJson>` | grava/atualiza (por `id`); sem `id` usa a primeira posição livre |
| `scenerename\|<id>\|<nome>`, `scenedelete\|<id>` | renomeia / exclui |
| `scenetemplate\|<idsCsv>\|<nome>` | "salvar como meu template" (papéis viram `Câmera n` quando a camada não tinha papel) |
| `templatedelete\|<id>` | exclui um template do usuário |
| `sceneexport\|<id>` / `sceneimport` | abre o diálogo de salvar/abrir arquivo (o host acrescenta o caminho e reenvia ao serviço) |

Host → UI: `scenes_state` (`{"t":"scenes_state","max":16,"scenes":[{"slot":0,"scene":{...}}],"templates":[...]}`), enviado no `ready` e a cada mudança.
Quadros: `c0`..`c15` (miniaturas das cenas, só para posições ocupadas) pelo mesmo caminho de `renderUiFrame` dos quadros `s0`..`s15`.

## Arquitetura

* `src/scene/` — `scene.*` (modelo + JSON), `compositor.*` (compilação por tamanho + composição), `scene_source.*` (fonte da mesa),
  `template_library.*` (biblioteca/arquivos), `scene_service.*` (cenas, persistência e protocolo de comandos). Tudo portátil (Linux/GCC e MSVC), com testes em `tests/scene_*_test.cpp`.
* **Por que um banco CENAS na mesa**: `Engine` acrescenta 16 `SceneSource` *depois* das 16 câmeras (índices 16–31 da mesa). Nenhuma API da mesa mudou; sem cenas
  as posições renderizam um painel escuro e o comportamento/UI das 16 câmeras permanece igual. Alternativa descartada: um modo "cena" separado do PGM/PVW exigiria duplicar a lógica de transições.
* **Desempenho**: ao definir uma cena ela é *compilada* para o tamanho do Program (máscaras de cantos, borda, sombra desfocada, fundo); por quadro só há
  cópia do fundo + render de cada fonte no tamanho da camada + mistura com máscara. Sem alocação por quadro (buffers por thread). A camada de baixo que cobre o quadro inteiro é
  renderizada direto no destino. Outros tamanhos (miniaturas/monitores) compilam na primeira vez (fora do relógio do Program). Se o aspecto de uma câmera mudar, recompila uma vez.
* Fontes com transparência (ex.: um filtro chroma-key por câmera) são respeitadas: o alfa da fonte vale como cobertura (alfa direto, não pré-multiplicado).

## Limites desta versão / não validado

* Sem animação por camada (as transições da mesa funcionam entre cenas inteiras).
* Fundo de imagem: só BMP (sem PNG/JPEG); há um gancho `ImageLoader` para o Windows plugar o WIC, ainda não ligado.
* O aspecto da câmera usado para "caber/preencher" vem da resolução informada pelo dispositivo (16:9 se desconhecida).
* Overlays dos templates (lower-third, título) são dados: ainda **não são desenhados**.
* Miniaturas do seletor são desenhadas pelo `canvas` da UI com cores de espaço reservado (aproximação do resultado real, sem a sombra/borda idênticas ao compositor C++).
* Verificado em Linux (testes unitários e UI no Chromium). **Não validado**: compilação MSVC/CI Windows, WebView2, os diálogos nativos de importar/exportar,
  desempenho com câmeras reais em 1080p60 e em hardware fraco. A medição de tempo por quadro em 1080p (4 camadas com sombra) está nos testes apenas como limite frouxo.
