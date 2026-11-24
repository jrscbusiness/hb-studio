# CHROMA (remoção de fundo por IA + chroma key clássico, por câmera)

Módulo: `src/chroma/` (biblioteca `hb_chroma`, **portável**, testada em Linux/GCC) +
`src/chroma/windows/` (WinHTTP + WIC, só Windows) + `src/chroma/onnx/` (ONNX Runtime, só Windows,
**opcional**). Seção **FUNDO** no diálogo de configurações de cada câmera (engrenagem em CÂMERAS ou
botão direito no tile > Configurações…).

> **Estado de validação:** toda a lógica portátil (chroma clássico, pós-processamento de máscara,
> composição, worker de IA com segmentador falso, downloader com SHA-256, comandos, persistência,
> UI) tem teste automatizado. **NADA disso foi validado em hardware real**: o código do ONNX Runtime
> (`onnx_segmenter.cpp`), do WinHTTP (`winhttp_client.cpp`) e do WIC (`wic_image.cpp`) **nunca foi
> compilado nem executado** pelo autor (ambiente Linux); só o CI do Windows verifica a compilação.
> Os números de custo de CPU abaixo são **estimativas**, não medições.

## Modos

| Modo | O que faz | Precisa de |
|---|---|---|
| Desligado | nada (custo zero) | — |
| Chroma verde / azul | chave por distância no plano CbCr do YCbCr (ignora luminância: sombra na tela ainda some), tolerância + suavidade de borda + supressão de reflexo (*despill*) | só uma tela verde/azul bem iluminada |
| IA | segmentação de pessoa por rede neural local, sem tela verde | ONNX Runtime no build + modelo baixado |

Substituição do fundo: cor sólida, desfoque do próprio fundo, imagem (PNG/JPEG/BMP, recorte
"cover") ou **transparente** (para camadas/cenas).

## Modelo e runtime escolhidos (e por quê)

* **Modelo:** MediaPipe Selfie Segmentation, variante *landscape* (entrada 256×144), na conversão
  ONNX publicada por `onnx-community` no Hugging Face. **Licença Apache-2.0** (modelo original do
  Google; o cartão do modelo declara `apache-2.0`). ~0,45 MB, MobileNetV3. Entrada `pixel_values`
  RGB 0..1 NCHW, saída `alphas`.
  * URL fixada em **commit** (`2497d5b…`), SHA-256 fixado
    `7a0adcfdb1715d3b0ff0f61486d8a181c33f4a208343a5abfccbc6540e872d24`, 462 338 bytes — o hash foi
    conferido manualmente ao escrever o módulo. Não há binário de modelo no repositório nem no
    instalador.
  * Descartados: **Robust Video Matting** (GPL-3.0 — incompatível com a política do projeto);
    **MODNet** (Apache-2.0, mas ~25 MB e entrada 512 — pesado demais para PCs modestos);
    PP-HumanSeg (Apache-2.0; não havia conversão ONNX com URL/hash estáveis verificada aqui).
  * Limitação real do modelo: é feito para **plano médio de uma pessoa** (selfie/videoconferência).
    Bordas de cabelo/dedos ficam moles; várias pessoas funcionam mas sem garantia; fundos com
    cores parecidas com a roupa erram. **Não substitui** uma tela verde bem feita.
* **Runtime:** ONNX Runtime (MIT), provedor **CPU** apenas (2 threads de cálculo por inferência).
  DirectML/GPU **não** está ligado. Foi escolhido por ser o runtime ONNX mais difundido e pequeno
  o bastante para um instalador (a DLL tem ~10–15 MB; **não foi medido aqui**).
* **Como entra no build:** não é baixado automaticamente (não consegui verificar o SHA-256 do zip
  oficial neste ambiente, e uma dependência binária sem hash fixado seria pior que nenhuma). Baixe
  o `onnxruntime-win-x64-<versão>.zip` oficial de github.com/microsoft/onnxruntime/releases, extraia
  em `third_party/onnxruntime` (ou `-DHB_ORT_DIR=...`) — `cmake/modules/chroma.cmake` detecta
  `include/onnxruntime_cxx_api.h` + `lib/onnxruntime.lib`, define `HB_HAVE_ONNX` e liga com
  *delay-load*. Sem isso o app compila e roda **só com chroma clássico** e a UI diz isso.
  * **Pendência do coordenador:** empacotar `onnxruntime.dll` ao lado do `HBStudio.exe` no
    instalador (`setup_main.cpp`/`.rc` embute só o exe e a DLL da câmera virtual). Sem a DLL, a IA
    falha com a mensagem "onnxruntime.dll não encontrada" (sonda segura antes do *delay-load*, que
    senão seria uma exceção SEH).

## Arquitetura (nunca bloquear o relógio de 30 fps)

```
SlotSource::render() ──► FrameFilter::apply(frame)   (thread que renderiza: relógio do PGM)
                              │  trabalho limitado por pixel: chave/upsample da máscara + composição
                              ▼
        ChromaFilter ── submit(frame) não-bloqueante ──► AiMaskWorker (thread própria)
                       ◄── latest() máscara imutável ──   downscale → ONNX → níveis → erode → feather → EMA
```

* `video::FrameFilter` (`src/video/source.hpp`): gancho mínimo por slot; o filtro pertence ao
  **slot** (sobrevive a trocar/limpar a câmera). Exceções do filtro são engolidas. Tudo a jusante —
  switcher, cenas, NDI, câmera virtual, gravação — vê o quadro já processado.
* O worker mantém **um** quadro pendente; se a inferência atrasa, quadros são descartados
  (`dropped`) e a **última máscara é reutilizada**. Há *throttle* por qualidade. O custo do
  downscale (caixa) para 256×144 fica no worker, não no relógio. Após 5 falhas seguidas o worker
  desiste (mensagem na UI) em vez de martelar um modelo quebrado.
* Sem modelo/runtime ou antes da primeira máscara: o quadro passa **intacto** (nunca tela preta).
* Só renders grandes (≥ metade da maior largura vista) alimentam o modelo; tiles pequenos só usam a
  máscara existente.
* Pós-processamento (resolução da máscara, barato): níveis (sensibilidade/suavidade) → erosão →
  *box blur* (feather) → **EMA temporal** (anti-cintilação), depois upsample bilinear na composição.

### Níveis de qualidade (IA)

| Nível | intervalo mín. entre inferências | erosão | feather | peso da máscara nova (EMA) |
|---|---|---|---|---|
| Baixa | 100 ms (~10/s) | 0 | 1 | 140/256 |
| Média | 66 ms (~15/s) | 1 | 1 | 170/256 |
| Alta | 33 ms (~30/s) | 1 | 2 | 200/256 |

A resolução da máscara é a **entrada fixa do modelo (256×144)**; "qualidade" muda taxa e
pós-processamento, não a resolução.

### Proteção de desempenho

`stateJson` do módulo (`{"t":"chroma"}`, mensagem separada do `s`) expõe por slot `inferMs`
(média móvel da inferência), `applyMs` (custo do filtro na thread de render), `dropped`,
`slowAi` (inferência > 90% do intervalo da qualidade escolhida, após 5 amostras) e `slowApply`
(> 15 ms por quadro). A UI mostra "Seu computador está lento para IA: reduza a qualidade".

### Custo esperado (ESTIMATIVA — NÃO validado em hardware real)

* Inferência MobileNetV3 256×144, CPU, 2 threads: ordem de poucos a ~20 ms por análise em CPU
  moderna recente; PCs fracos/antigos podem passar de 50 ms (aí o aviso aparece e a taxa cai).
* Composição por quadro renderizado: O(pixels). Em 1080p a chave clássica/composição custa
  da ordem de **5–15 ms por render** (CPU de um núcleo) — e roda na thread do relógio do PGM, então
  com fundo **desfoque/imagem** em 1080p o orçamento de 33 ms pode apertar (o desfoque é recalculado
  a cada 3º render e a imagem é redimensionada uma vez por tamanho). Use resolução de programa
  menor ou o chroma clássico em máquinas fracas. Várias câmeras com IA ao mesmo tempo somam
  threads de inferência (uma por slot em modo IA, sessão ONNX compartilhada).

## Alpha: convenção para quem compõe (cenas, overlays)

Quadros são **BGRA8 com alpha DIRETO (não pré-multiplicado)** — a mesma convenção que
`src/scene/compositor` usa ao ler a saída de `render()` de um slot.

* Substituição **cor/desfoque/imagem**: todo pixel sai com `A = 255`.
* **Transparente**: `A` = máscara (0..255), `B,G,R` intactos (no chroma clássico já com *despill*).
  "Over": `out = fg*A/255 + bg*(255-A)/255`.
* Quem ignora alpha (preview JPEG, codificadores MF, `mixFrames` do switcher, NDI sem alpha) mostra
  a **câmera sem recorte** nas áreas transparentes. Só caminhos que respeitam alpha (cenas) veem o
  recorte. Para ter fundo "sumido" na saída do PGM, use cor/desfoque/imagem, ou coloque a câmera
  transparente numa cena sobre outro fundo.

## Download do modelo

`ModelStore`: baixa só quando o operador clica **BAIXAR MODELO**; HTTPS obrigatório (sem credenciais
na URL), para `…onnx.part` com **teto de 2× o tamanho esperado**, SHA-256 incremental; só renomeia
se tamanho **e** hash baterem — qualquer divergência apaga o arquivo e mostra o erro. Um arquivo
já presente é re-verificado na inicialização (inválido = removido). Progresso/cancelar/erros na UI.
HTTP está atrás de `IHttpClient` (testado com falso); `WinHttpClient` segue redirecionamentos
(o Hugging Face redireciona para o CDN) sem rebaixar HTTPS→HTTP. Destino:
`%LOCALAPPDATA%\HBStudio\models\`. Se a internet exigir proxy autenticado, falha com mensagem
(usa o proxy padrão do sistema). **Não testado numa rede real.**

## Comandos (UI → host) e persistência

* `chroma|<slot>|<chave>|<valor>` — chaves: `mode` (off/green/blue/ai), `tol`, `soft`, `spill`
  (0–100), `q` (low/med/high), `repl` (color/blur/image/transparent), `color` (#rrggbb), `blur`
  (0–100), `img` (caminho). Entrada inválida é ignorada; números são limitados.
* `chromamodel|download|cancel|remove`, `chromaimg|<slot>` (abre o seletor de arquivo nativo).
* Persistência: **9º campo opcional** de `inputs.txt` (`source_assignments`), string compacta
  `mode=ai;tol=50;…` sem tabs/novas linhas. Arquivos antigos (8 campos) continuam válidos. Limite
  conhecido: como mora na linha do dispositivo, só persiste para slots **com câmera atribuída**;
  trocar de câmera mantém o fundo, `devclear` (esquecer a câmera) apaga.
* Barramento de eventos: este módulo não publica nem consome tópicos congelados.

## Limitações conhecidas / não feito

* Cor-chave fixa (verde/azul "típicos"); não há conta-gotas para amostrar a tela real.
* Sem aceleração por GPU (DirectML) e sem modelo de matting fino (cabelo).
* Imagem de fundo: sem animação/vídeo; decodificada uma vez, redimensionada por tamanho de render.
* A prévia da UI mostra o resultado (tile já filtrado); não há comparação "antes" lado a lado.
* Windows: código de WinHTTP/WIC/ONNX por revisar em CI e em máquina real.
