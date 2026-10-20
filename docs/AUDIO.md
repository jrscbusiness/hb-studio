# Áudio (Fase 12)

Núcleo portátil de áudio do HB Studio: mixer, medidores, conversão de taxa, buffers e sincronia.
Código em `src/audio/` (`namespace hb::audio`), registrado por `cmake/modules/audio.cmake`
(biblioteca `hb_audio`, testes `hb_audio_tests`). **Nada aqui abre dispositivo de áudio**: a
captura/reprodução real (WASAPI, CoreAudio, NDI, RTSP) é feita por quem implementar `AudioSource`.

Formato interno do caminho de Program: **float32, estéreo intercalado, 48 kHz**.

## Fluxo A/V

```
CAM n (RTSP/NDI/UVC) → decode → [Resampler / DriftCompensator] → RingBuffer SPSC ─┐
MIC 1..2, SYSTEM (dispositivo do SO)  → idem ──────────────────────────────────────┤
                                                                                    ▼
                        AudioSource::pull() → DelayLine (lip-sync) → Mixer ──► PROGRAM bus
                                                                    │            │
                         gain → fader → mute/solo → pan             │            ├─► Recorder / Streamer / saída do SO
                         master → Limiter/SoftClip → medidores      │            └─► eventos "audio.levels" (UI)
```

- Thread de captura (produtor) escreve no `RingBuffer`; a thread de áudio (consumidor) puxa blocos.
- O `Mixer` roda **só** na thread de áudio; a UI altera parâmetros por setters atômicos.

## Política de sincronia

1. **O áudio é o relógio do Program.** O ritmo do Program Audio é o do dispositivo/clock de áudio de
   48 kHz; o vídeo do Program se adapta (repete/descarta quadros), nunca o contrário.
2. Cada fonte tem timestamp de apresentação (PTS). Diferenças fixas entre áudio e vídeo de uma câmera
   são corrigidas com `DelayLine` (offset de lip-sync, 0–2000 ms, ajustável ao vivo, crossfade de 20 ms
   sem cliques). O offset é por fonte e configurado pelo operador.
3. Diferença de clock entre a fonte e os 48 kHz do Program (deriva, tipicamente dezenas de ppm) é
   absorvida pelo `DriftCompensator`: um controlador PI mede o nível do buffer e ajusta a razão do
   `Resampler` em ppm (limite padrão ±500 ppm; não audível). Nível do buffer = orçamento de latência.
4. Underrun gera silêncio e incrementa contador (`RingBuffer::underruns()`); overrun descarta e conta.

## Componentes

| Arquivo | Função |
|---|---|
| `mixer.*` | N canais estéreo: ganho, fader (−∞…+10 dB), mute, solo-in-place, pan, master, limitador, medidores |
| `limiter.*` | `Limiter` look-ahead (1 ms, ceiling −1 dBFS por padrão), `SoftClip` (tanh C1) ou `Off` |
| `meters.*` | Peak (hold/decay), RMS (300 ms), True Peak 4x (BS.1770-4 Anexo 2), LUFS momentâneo (400 ms) e short-term (3 s) com K-weighting |
| `ring_buffer.hpp` | SPSC lock-free de float, contadores de under/overrun |
| `resampler.*` | `Linear` ou sinc com janela Kaiser polifásico (`Low`/`Medium`/`High` = 16/32/64 taps, escalonado ao reduzir taxa) |
| `drift_compensator.*` | Resample adaptativo por ppm a partir do nível do buffer |
| `delay_line.*` | Atraso de 0–2000 ms por fonte |
| `sources.*` | `ToneSource` (seno), `PinkNoiseSource` (ruído rosa), `SilenceSource` |
| `level_publisher.*` | Publica `"audio.levels"` no `hb::core::EventBus` com limite de frequência |

### Mixer

- **Fader:** posição 0…1 → dB por segmentos: 0 → −∞; 0,10 → −60; 0,25 → −40; 0,50 → −20; 0,75 → 0 dB; 1,0 → +10 dB.
  Abaixo de 0,10 o ganho linear cai a zero. `setFaderDb()` aceita −∞ e satura em +10 dB.
- **Pan:** lei de potência constante `L = cos θ`, `R = sin θ`, θ = (pan+1)·π/4 → −3,01 dB no centro,
  `L² + R² = 1`. Para fonte estéreo aplica-se como balanço (um lado some em pan extremo).
- **Solo-in-place:** com qualquer solo ativo só canais em solo chegam ao bus; mute vence solo.
- **Anti-zipper:** todo ganho (canal e master) muda por rampa linear de 10 ms (configurável),
  independente do tamanho do bloco. Teste: fader a −∞ no meio de um seno gera salto máximo entre amostras
  igual ao do próprio seno (sem degrau).
- **Proteção contra clipping:** `Limiter` garante `|x| ≤ ceiling` (look-ahead, ganho suavizado, release 100 ms);
  `SoftClip` não tem latência de ganho mas curva suave; `Off` deixa passar. A latência (47 amostras ≈ 1 ms)
  é constante em todos os modos para que a troca não gere salto.
- **Medidores:** por canal (pré-fader, após trim) e no Program (pós-limitador), publicados em atômicos
  e lidos por `channelLevels()` / `programLevels()` de qualquer thread. Silêncio = `-inf`.
  O True Peak mantém o máximo até `resetMeters()`.

### Uso mínimo

```cpp
hb::audio::Mixer mixer(hb::audio::MixerConfig::broadcastDefault(4)); // CAM 1..4, MIC 1, MIC 2, SYSTEM
mixer.attachSource(0, &cam1Source);          // AudioSource*
mixer.setFaderPosition(0, 0.75f);            // UI thread
// thread de áudio, a cada bloco:
mixer.process(programOut, frames);           // ou process(inputs, programOut, frames)
```

### Integração por eventos

`LevelPublisher::tick(nowSeconds)` (chamar de timer da UI, **não** da thread de áudio) publica o tópico
`audio.levels`, no máximo `maxHz` vezes por segundo (padrão 10). Campos (strings, dB/LUFS com 1 casa,
`-inf` para silêncio): `program.peak.l/.r`, `program.hold.l/.r`, `program.rms.l/.r`,
`program.truePeak.l/.r`, `program.lufs.m`, `program.lufs.s`, `program.limiterGr`, `channels`,
`ch.<i>.name`, `ch.<i>.peak`, `ch.<i>.mute`, `ch.<i>.solo`.

## Validação numérica (testes `hb_audio_tests`)

| Medida | Resultado |
|---|---|
| Seno 1 kHz estéreo −23 dBFS/canal (estilo EBU Tech 3341, 20 s) | M = S = **−22,993 LUFS** (tolerância ±0,1) |
| Idem −33 dBFS / só canal esquerdo | −33,0 / −26,0 LUFS (−3,01 LU, esperado) |
| LUFS vs. cálculo analítico (resposta dos biquads) em 100 Hz–10 kHz | erro < 0,05 LU |
| Coeficientes K-weighting a 48 kHz | tabelados BS.1770-4; projeto analítico reproduz a tabela (< 2e-6) |
| True Peak, seno fs/4 com fase 45° (pico de amostra −3,01 dBFS) | **+0,08 dBTP** (verdadeiro 0 dB) |
| Resampler 44,1→48 kHz, seno 1 kHz, SNR | High 108 dB · Medium 95 dB · Low 70 dB · Linear 62 dB |
| Resampler 32→48 e 96→48 kHz (High) | 124 dB |
| Resampler 44,1→48, 15 kHz (High) | 115 dB |
| Aliasing 96→48 kHz, tom de 30 kHz | rejeição de 101 dB (High) |
| Deriva de +150 ppm, 200 s simulados | ppm estimado 149,9; buffer estável (±32 quadros), 0 underruns/overruns |
| Limitador, 4 canais quentes (~+8 dBFS de soma) | pico 0,8913 = teto (−1 dBFS) |

Os testes não usam dispositivo de áudio, rodam em < 1 s cada e foram executados também com
ThreadSanitizer (sem avisos).

## Limitações

- **A câmera virtual de vídeo NÃO leva áudio.** O dispositivo "HB Studio Virtual Camera" (Media Foundation)
  só entrega vídeo. Para o **OBS** receber o áudio do Program será preciso **NDI Output** (vídeo + áudio,
  recomendado) ou um **dispositivo de áudio virtual** (driver; complexo, fora do MVP) ou um cabo virtual
  de terceiros. Até lá, o áudio do OBS vem dos dispositivos do próprio OBS.
- **Não validado em hardware real:** nenhuma captura de câmera/microfone, saída para o SO, nem latência
  ponta a ponta foi medida; o módulo só foi testado com sinais sintéticos (Linux/GCC 13; MSVC não testado).
- Não há conversão de canais (mono/5.1 → estéreo) nem de taxa dentro do `Mixer`: as fontes devem entregar
  estéreo a 48 kHz (use `Resampler`/`DriftCompensator` antes do ring buffer).
- LUFS **integrado** (com gating) e LRA não foram implementados; apenas momentâneo e short-term.
  Antes de 400 ms/3 s de sinal, a janela usa os blocos de 100 ms disponíveis.
- O True Peak usa o filtro de 48 taps do Anexo 2 da BS.1770-4 (coeficientes reproduzidos de memória da
  norma e verificados por teste de ganho/seno; conferir contra o texto oficial antes de certificação).
- `DelayLine` usa atrasos de amostra inteira com crossfade; mudar o atraso não gera clique, mas o
  conteúdo salta no tempo (esperado). Com atraso máximo e estéreo float, usa ~0,8 MB por fonte.
- `Limiter` introduz ~1 ms de latência fixa. Pan em fonte estéreo é balanço, não "pan de imagem".
- Ao trocar o modo do limitador ao vivo pode haver um degrau pequeno de ganho (troca rara).
- `DriftCompensator` assume que o ring tem a taxa da fonte e que `pull` é chamado em cadência regular.
