# Desempenho do vídeo (preview, Program, conversões)

Este documento registra a medição e as otimizações dos caminhos de vídeo quentes do HB Studio. Tudo é C++ portátil
(sem GPU, sem Direct3D) e pode ser medido e testado no Linux; a reescrita para GPU foi rejeitada de propósito porque
não poderia ser validada sem Windows.

## Como medir

```
cmake -S . -B build-bench -G Ninja -DCMAKE_BUILD_TYPE=Release -DHB_BUILD_BENCH=ON
cmake --build build-bench --target hb_bench
build-bench/hb_bench [--quick] [--filter texto]
```

`hb_bench` (`tools/bench_video.cpp`) não faz parte do `ctest` (tempos dependem da máquina). Ele mede o custo **por
quadro, em 1 núcleo**, em 720p, 1080p e 4K: `fitFrame`, `downscaleBox` (caminho do preview da UI), `mixFrames`
(dissolve), composição de cenas, mistura de overlays, conversão YUY2/UYVY/NV12 para BGRA, o `Switcher`, o relógio real do
`ProgramOutput` a 30/60 fps (intervalo entre quadros e ticks perdidos) e o caminho de quadros da UI sem JPEG
(`Engine::renderUiFrame`).

## Máquina e compilador

Linux 6.18 (VM), Intel Xeon @ 2,10 GHz, 4 threads, GCC 13.3, `-O3 -DNDEBUG` (CMake Release). **A VM é compartilhada e
ruidosa (±20 % entre execuções)**: a tabela abaixo usa o menor entre dois processos alternados (antes/depois) para cada
caso. Ganhos menores que ~1,3x devem ser lidos como "sem diferença mensurável".

## Antes e depois (ms por quadro, 1 núcleo; menor = melhor)

| Caso | Antes (ms) | Depois (ms) | Ganho |
|---|---:|---:|---:|
| fitFrame 1:1 -> 720p | 1.60 | 0.25 | 6.3x |
| fitFrame 4:3 pillarbox -> 720p | 1.33 | 0.25 | 5.2x |
| fitFrame 4K downscale -> 720p | 1.75 | 0.55 | 3.2x |
| downscaleBox 720p -> 640x360 | 3.30 | 1.11 | 3.0x |
| downscaleBox 720p -> 960x540 | 7.29 | 1.92 | 3.8x |
| downscaleBox 720p -> 192x108 | 1.04 | 0.75 | 1.4x |
| downscaleBox 720p -> 320x180 | 1.57 | 0.59 | 2.7x |
| mixFrames dissolve 720p | 0.46 | 0.47 | 1.0x |
| fitFrame 1:1 -> 1080p | 3.87 | 0.59 | 6.5x |
| fitFrame 4:3 pillarbox -> 1080p | 3.31 | 0.56 | 6.0x |
| fitFrame 720p upscale -> 1080p | 3.59 | 0.97 | 3.7x |
| fitFrame 4K downscale -> 1080p | 4.26 | 1.00 | 4.3x |
| downscaleBox 1080p -> 640x360 | 4.18 | 1.53 | 2.7x |
| downscaleBox 1080p -> 960x540 | 7.29 | 2.48 | 2.9x |
| downscaleBox 1080p -> 192x108 | 1.78 | 0.92 | 1.9x |
| downscaleBox 1080p -> 320x180 | 2.34 | 1.07 | 2.2x |
| mixFrames dissolve 1080p | 1.00 | 1.05 | 1.0x |
| fitFrame 1:1 -> 4K | 16.20 | 2.48 | 6.5x |
| fitFrame 4:3 pillarbox -> 4K | 14.07 | 2.20 | 6.4x |
| fitFrame 720p upscale -> 4K | 13.60 | 2.57 | 5.3x |
| downscaleBox 4K -> 640x360 | 9.86 | 4.91 | 2.0x |
| downscaleBox 4K -> 960x540 | 13.89 | 6.44 | 2.2x |
| downscaleBox 4K -> 192x108 | 6.96 | 3.63 | 1.9x |
| downscaleBox 4K -> 320x180 | 6.74 | 3.79 | 1.8x |
| mixFrames dissolve 4K | 7.58 | 7.92 | 1.0x |
| YUY2->BGRA 720p | 2.12 | 0.68 | 3.1x |
| UYVY->BGRA 720p | 2.07 | 0.68 | 3.1x |
| NV12->BGRA 720p | 2.78 | 0.68 | 4.1x |
| YUY2->BGRA 1080p | 4.76 | 1.45 | 3.3x |
| UYVY->BGRA 1080p | 4.77 | 1.44 | 3.3x |
| NV12->BGRA 1080p | 10.42 | 1.55 | 6.7x |
| YUY2->BGRA 4K | 23.73 | 6.28 | 3.8x |
| UYVY->BGRA 4K | 22.82 | 6.07 | 3.8x |
| NV12->BGRA 4K | 36.68 | 5.91 | 6.2x |
| overlay blendBitmap lower-third 720p | 0.36 | 0.13 | 2.8x |
| overlay blendBitmap lower-third op=.6 720p | 0.27 | 0.15 | 1.7x |
| overlay blendBitmap full-frame 720p | 2.73 | 1.58 | 1.7x |
| overlay fillRect translucent full 720p | 1.82 | 1.28 | 1.4x |
| overlay blendBitmap lower-third 1080p | 0.55 | 0.32 | 1.7x |
| overlay blendBitmap lower-third op=.6 1080p | 0.61 | 0.37 | 1.7x |
| overlay blendBitmap full-frame 1080p | 6.08 | 3.58 | 1.7x |
| overlay fillRect translucent full 1080p | 4.13 | 3.15 | 1.3x |
| overlay blendBitmap lower-third 4K | 2.26 | 1.33 | 1.7x |
| overlay blendBitmap lower-third op=.6 4K | 2.47 | 1.56 | 1.6x |
| overlay blendBitmap full-frame 4K | 24.35 | 18.70 | 1.3x |
| overlay fillRect translucent full 4K | 17.05 | 15.36 | 1.1x |
| scene composeScene 3 layers 720p | 4.07 | 5.09 | 0.8x |
| scene composeScene blend-only 720p | 2.55 | 2.43 | 1.1x |
| scene composeScene 3 layers 1080p | 10.46 | 12.23 | 0.9x |
| scene composeScene blend-only 1080p | 5.93 | 5.30 | 1.1x |
| scene composeScene 3 layers 4K | 43.27 | 50.96 | 0.8x |
| scene composeScene blend-only 4K | 24.90 | 24.02 | 1.0x |
| switcher renderProgram cut 720p | 0.63 | 0.62 | 1.0x |
| switcher renderProgram dissolve 720p | 1.75 | 1.67 | 1.0x |
| switcher renderProgram cut 1080p | 1.49 | 1.50 | 1.0x |
| switcher renderProgram dissolve 1080p | 4.04 | 3.90 | 1.0x |
| switcher renderProgram cut 4K | 5.69 | 6.05 | 0.9x |
| switcher renderProgram dissolve 4K | 20.04 | 20.68 | 1.0x |
| UI renderUiFrame pgm [baixa] (program 1080p) | 2.57 | 1.09 | 2.3x |
| UI renderUiFrame pvw [baixa] (program 1080p) | 0.01 | 0.01 | 1.1x |
| UI renderUiFrame s0 [baixa] (program 1080p) | 0.00 | 0.00 | 1.0x |
| UI renderUiFrame pgm [alta] (program 1080p) | 6.34 | 1.68 | 3.8x |
| UI renderUiFrame pvw [alta] (program 1080p) | 0.05 | 0.04 | 1.1x |
| UI renderUiFrame s0 [alta] (program 1080p) | 0.00 | 0.00 | 1.0x |
| UI renderUiFrame pgm [maxima] (program 1080p) | 9.67 | 2.49 | 3.9x |
| UI renderUiFrame pvw [maxima] (program 1080p) | 0.18 | 0.18 | 1.0x |
| UI renderUiFrame s0 [maxima] (program 1080p) | 0.01 | 0.01 | 0.9x |
| UI feeder tick: pgm+pvw+8 tiles [alta] | 6.47 | 1.85 | 3.5x |

Relógio do Program (`ProgramOutput`, fonte sintética): em 720p/1080p, 30 e 60 fps, o intervalo mediano ficou em
33,3 ms / 16,7 ms antes e depois, sem ticks perdidos. Em 4K a 60 fps o relógio perde 4–8 ticks em 1,5 s nas duas versões
(a fonte de teste do benchmark é cara e o 4K não é o alvo); não há regressão.

Custo de decidir "o quadro da UI não mudou" (hash do quadro já reduzido): 0,016 ms (320x180), 0,035 ms (480x270),
0,058 ms (640x360), 0,138 ms (960x540).

## O que foi feito (e o que não valeu a pena)

Mantido, com teste de igualdade byte a byte contra a implementação antiga (`tests/video_ref_*.hpp` guardam o código
original; `tests/perf_*_test.cpp` comparam em quadros aleatórios e casos de borda):

- **`fitFrame`** (entrada de câmeras/NDI → formato do Program): só as barras pretas são limpas (antes o quadro todo era
  preenchido e depois sobrescrito), 1:1 vira cópia de linha com alfa 255, linhas repetidas no upscale são copiadas da linha
  anterior, mapa de colunas em cache por thread (sem alocação por quadro). 3–6,5x.
- **`downscaleBox`** (monitor PROGRAM da UI e miniaturas): soma vertical vetorizável por linha de destino + soma
  horizontal por coluna com mapas pré-calculados e divisão exata por multiplicação/deslocamento
  (`floor(n/d) == (n*ceil(2^32/d))>>32` se `n*d < 2^32`; caixas maiores usam a divisão normal). 2–4x. O custo do
  monitor PROGRAM no preset "alta" (1080p → 640x360) caiu de ~6,3 ms para ~1,7 ms por quadro.
- **YUY2/UYVY/NV12 → BGRA**: SSE2 (linha de base do x86-64 e do MSVC x64; fora disso, caminho escalar) com
  `_mm_madd_epi16` (aritmética exata em 32 bits) e `packus` para o clamp. Resultado idêntico a `yuvToBgra()` para as
  2^24 combinações (y,u,v), testado pelos dois caminhos (vetor e resto escalar). 3–6,7x.
- **Overlays** (`blendBitmap`/`fillRect` sobre o quadro do Program, sempre opaco): linha opaca usa a fórmula sem
  ramificações (para alfa 0 e 255 ela coincide com o caso especial do original) em SWAR de 32 bits. 1,3–1,7x; ganho
  modesto porque o benchmark usa alfa aleatório (pior caso).

Revertido por não ser mensuravelmente melhor (a regra é manter só o que mede melhor):

- `mixFrames` com lanes de 16 bits: o loop já é limitado pela memória (~25 GB/s); sem diferença → código original.
- Composição de cenas (caminho de cópia para linhas opacas sem máscara): dentro do ruído → revertido. O custo vem
  da renderização das fontes e dos blends com máscara/sombra, que já são lineares.
- Multithreading: não adotado. Os ganhos acima já tiram o custo do relógio de 30/60 fps de ordem de grandeza, e threads
  extras competiriam com o relógio do Program, a captura e o encoder. Nada de pool foi criado.

Não medido aqui (honestidade): o codificador JPEG da UI usa WIC (Windows) e não existe no Linux; o custo de
JPEG/base64 não é comparável com o do Linux e não foi medido. O build MSVC das mudanças só é verificado pela CI
(`tools/check_windows_syntax.sh` cobre o `app_main.cpp` com mingw).

## Governador automático de qualidade (CONFIG > Desempenho)

`perf::QualityGovernor` (`src/perf/quality_governor.*`, testes em `tests/perf_governor_test.cpp`): lógica pura, relógio
injetável, thread-safe. Observa o fps real do Program e os ticks perdidos (`Engine::snapshot()`: `fps`, `skipped`,
`targetFps`) a cada 1 s:

- amostra **ruim**: fps < 90 % do alvo ou ≥ 1 tick perdido; amostra **boa**: fps ≥ 97 % e nenhum tick perdido; a faixa
  entre as duas zera as sequências (histerese);
- **desce um degrau** (baixa → ... → máxima, via `Engine::setPreviewQualityIndex`) após 2 amostras ruins seguidas e 3 s
  desde a última mudança; **sobe um degrau** após 10 amostras boas seguidas e 15 s desde a última mudança, nunca acima
  do preset que o operador escolheu;
- se o problema volta em até 45 s depois de uma subida, a exigência de estabilidade dobra (até 8x) até 180 s de calma;
- fps 0 (relógio parado) não conta como evidência.

Padrão **ligado** (só o preview); a chave fica em `perfgov.txt` (pasta de mídia) e a UI mostra, no cartão "Qualidade
do Preview", o selo **reduzido automaticamente** com o nível em uso. Escolher um preset manualmente define o novo teto
e aplica na hora; desligar a chave devolve a escolha do operador imediatamente. O governador **nunca** toca na
resolução do Program: ele só devolve um índice de preset do preview.

## Redução do custo dos quadros da UI

- `perf::UiFrameGate`: não codifica/envia um quadro de UI idêntico ao último enviado para o mesmo id (hash de 64 bits
  do quadro já reduzido, 0,06 ms em 640x360); reenvia mesmo assim a cada 1 s (a página recarregada nunca fica em
  branco). Câmeras reais têm ruído de sensor, então o ganho real é em conteúdo estático (SEM SINAL, padrão de teste,
  cenas paradas, miniaturas ociosas) — não foi medido em hardware real.
- `perf::UiFramePolicy`: a UI avisa a aba ativa (`tab|<id>`); fora de PRODUÇÃO, GRÁFICOS e CÂMERAS (as que mostram
  monitores/miniaturas) os monitores caem para ~3,75 fps e cada miniatura para ~1,9 fps. Ao trocar de aba, o portão é
  zerado e os quadros chegam na hora.
