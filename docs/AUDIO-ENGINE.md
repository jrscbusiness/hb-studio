# Motor de áudio (`src/audio_io/`)

Motor de áudio do aplicativo: entradas reais (microfone, linha, loopback do sistema), até 8 faixas
(*channel strips*) com cadeia de efeitos, barramento **Program** com medidores, monitoramento em fones
e persistência. Código em `namespace hb::audio_io`, registrado por `cmake/modules/audio_io.cmake`
(biblioteca `hb_audio_io`, testes `hb_audio_io_tests`). Reutiliza `src/audio/` (`Mixer`, medidores,
`DriftCompensator`, `Resampler`, `DelayLine`, `Limiter`, `RingBuffer`) e o contrato `src/audio/effect.hpp`.

Formato interno do Program: **float32, estéreo intercalado, 48 kHz**, blocos de **480 quadros (10 ms)**.

## Visão geral

```
 dispositivo (taxa NATIVA) ─► CaptureStream ─► anel SPSC ─► DriftCompensator ─┐
 fonte externa AudioSource (48 kHz ou outra taxa → Resampler) ────────────────┤
                                                                               ▼
 FAIXA:  trim → inverter fase → mono → atraso (lip-sync) → cadeia de efeitos (≤ 8)
                         │                                        │
                         │                       medidores da faixa (pico/hold, RMS, LUFS-S, GR)
                         ├─► monitor (se "toMonitor", pré-fader) ──► ganho/mute ──► PlaybackStream (fones)
                         ▼
        fader → mute/solo (solo-in-place) → pan (potência constante)   [hb::audio::Mixer]
                         ▼
       soma → master (fader) → limitador (liga/desliga, teto) → PROGRAM ──► medidores (pico, RMS, LUFS M/S, True Peak)
                                                                        └─► consumidores (gravador) em blocos de 480 quadros
```

Latências fixas: bloco de 10 ms; limitador do Program 47 amostras (≈ 1 ms, constante mesmo desligado, para
a troca não gerar salto); captura = *target* do compensador de deriva (40 ms por padrão, `captureTargetMs`) +
período do dispositivo; monitoramento = 40 ms de anel (`monitorLatencyMs`) + período do dispositivo.

## Dispositivos (miniaudio 0.11.22, MIT)

- `DeviceEnumerator`: lista **captura**, **loopback do sistema** (só WASAPI) e **reprodução**; `refresh()` sob
  demanda; tolerante a falhas (sem sistema de áudio → lista vazia + `lastError()` em pt-BR, nunca exceção).
- **IDs estáveis** (string): `"<tipo>:<backend>:<identificador>"`, tipo = `cap` | `loop` | `play`, backend =
  `wasapi` | `pulse` | `alsa` | `coreaudio` | `null`..., identificador = o ID do próprio backend (GUID do endpoint no
  WASAPI, nome no Pulse/ALSA). Ex.: `cap:wasapi:{0.0.1.00000000}.{...}`. Vazio ou `cap:default` = padrão do sistema.
- `CaptureStream`: abre na **taxa e no formato nativos** (miniaudio só converte canais → estéreo) e empurra float32
  num anel SPSC (500 ms). Cada (re)abertura cria uma `CaptureSession` nova (anel + taxa), então áudio velho nunca
  vaza para um dispositivo novo e a taxa pode mudar.
- Estados (`StreamState`) com mensagem pt-BR: `Connecting` → `Online` ⇄ `NoSignal`; `Error` (com backoff
  exponencial 250 ms → 4 s, mensagem "Nova tentativa em N s"). `NoSignal` = o dispositivo está aberto mas não
  entrega áudio (por exemplo, **loopback WASAPI não entrega pacotes enquanto nada toca**); não reabre. Dispositivo
  que some (`alive() == false`) → `Error` + reabertura automática. Nada bloqueia nem lança: um supervisor de 50 ms
  faz o `tick()`; testes dirigem `tick()` com relógio injetado.
- `PlaybackStream` (monitoramento): o motor escreve 48 kHz; o callback do dispositivo reamostra para a taxa nativa
  com controlador PI (mesmos ganhos do `DriftCompensator`) guiado pelo nível do anel → absorve deriva entre o
  relógio do motor e o do dispositivo. Dispositivo fechado descarta sem contar overrun.
- A camada do SO fica atrás de `RawDevice`/`DeviceOpener` (`raw_device.hpp`): testes injetam dispositivos falsos e
  usam também o backend **null** do miniaudio (sem hardware).

## `AudioEngine`

Thread própria a cada 10 ms (relógio `steady`, *deadline* absoluto, recupera atrasos até 200 ms antes de
ressincronizar; Windows: `timeBeginPeriod(1)` e prioridade alta). Ela **nunca trava, aloca ou bloqueia**:
parâmetros por atômicos; entrada e cadeia de efeitos por *snapshots* imutáveis trocados atomicamente, com
reclamação por época (`EpochGate`): a thread de controle só libera um snapshot depois que o bloco que podia
vê-lo terminou. Efeitos compartilham a instância entre snapshots (estado preservado ao mover/inserir).

### Comandos (thread-safe, validados; retornam `bool`, motivo em `lastError()`)

| Método | Observações |
|---|---|
| `setStripParam(i, chave, valor)` | `trimDb` (−60…+24), `faderDb` (≤ −100 = −∞, máx +10), `mute`, `solo`, `pan` (−1…1), `phase`, `mono`, `delayMs` (0…2000), `toProgram`, `toMonitor`, `name` (texto, ≤ 32 bytes) |
| `setStripQuality(i, "linear"\|"medium"\|"high")` | qualidade do reamostrador / compensador de deriva da faixa |
| `setStripInput(i, "none"\|"device"\|"external", id)` | `device`: id do enumerador (`play:` é recusado); `external`: id registrado com `registerExternalSource(id, shared_ptr<AudioSource>)` |
| `addEffect(i, typeId, pos)`, `removeEffect`, `moveEffect(i, de, para)`, `setEffectBypass`, `setEffectParam(i, idx, paramId, valor)` | máx. 8 efeitos; criados por `EffectRegistry::create`; valores limitados a `[min,max]`; parâmetros de escolha arredondados |
| `setMasterParam("faderDb"\|"limiter"\|"ceilingDb", v)` | teto −12…0 dBFS |
| `setMonitor("level"\|"mute", v)`, `setMonitor("deviceId", id)` | `level` 0…1 na mesma lei do fader (0,75 = 0 dB); `deviceId` vazio = desligado |
| `refreshDevices()`, `resetMeters()` | |
| `addProgramConsumer(fn)` / `removeProgramConsumer(id)` | blocos de 480 quadros na thread do motor (entregar rápido!); remoção **síncrona** |

NaN é rejeitado; valores finitos fora da faixa são limitados; índices inválidos retornam `false`. Faixas padrão:
`MIC 1`, `MIC 2`, `LINHA`, `SISTEMA`, `FAIXA 5`…`FAIXA 8`. `version()` incrementa a cada mudança de configuração
(não de nível). `stats()`: blocos, `xruns` (motor acordou com atraso), underruns/overruns dos anéis, erros de
consumidor, CPU (média e pico).

### Medidores

Por faixa: **pós-cadeia e pré-fader** (pico, *hold* 1,5 s, RMS 300 ms, LUFS *short-term*, soma da redução de ganho
dos efeitos; cada efeito também individualmente). Program (pós-limitador): pico, *hold*, RMS, True Peak, LUFS
momentâneo e *short-term*, redução do limitador.

### JSON para a UI

`snapshotJson(false)` (níveis, ~15×/s) e `snapshotJson(true)` (configuração completa, quando `version()` muda):

```json
{"t":"audiolv","strips":[{"pk":-20.5,"hold":-18.0,"rms":-30.2,"ls":-24.0,"gr":[0.0,3.2]}],
 "master":{"pk":-6,"hold":-5,"rms":-20,"lm":-23,"ls":-23.5,"tp":-5.8,"gr":0},"xr":0}
```
```json
{"t":"audio","ver":N,"strips":[{"i":0,"name":"MIC 1","input":{"kind":"device|external|none","id":"","name":"","state":"online|connecting|nosignal|error|off","msg":""},
 "trimDb":0,"faderDb":0,"mute":false,"solo":false,"pan":0,"phase":false,"mono":false,"delayMs":0,"quality":"medium",
 "toProgram":true,"toMonitor":false,"fx":[{"type":"hb.compressor","name":"Compressor","bypass":false,"missing":false,
 "params":[{"id":"threshold","label":"Limiar","unit":"dB","min":-60,"max":0,"def":-18,"log":false,"choices":[],"value":-20}]}]}],
 "master":{"faderDb":0,"limiter":true,"ceilingDb":-1},
 "monitor":{"deviceId":"","level":0.75,"mute":false,"state":"off","msg":""},
 "devices":[{"id":"","name":"","kind":"capture|loopback|playback","isDefault":true}],
 "effects":[{"typeId":"hb.gate","name":"Gate","category":"Dinâmica","source":"builtin"}]}
```
Silêncio e −∞ são sempre `-120` (nunca `-inf`/`NaN`); `faderDb` ≤ −100 significa −∞. Números são escritos com
`std::to_chars` (independente de *locale*).

## Persistência

`saveSettings(path)` / `loadSettings(path)` (também `settingsJson()` / `applySettingsJson(texto)`): JSON com
`"schema":1`, gravação **atômica** (arquivo temporário + `rename`). Contém, por faixa: nome, entrada (tipo + id do
dispositivo), trim, fader, mute, solo, pan, `phaseInvert`, mono, `delayMs`, `quality`, `sendToProgram`, `monitor` e
cadeia de efeitos (`type`, `bypass`, `params`); além de master e monitoramento. Fontes externas são lembradas só pelo
id (precisam ser registradas de novo pelo aplicativo).

- **Arquivo ilegível, vazio, truncado, não-JSON, grande (> 4 MB) ou profundo demais**: `loadSettings` retorna `false`
  e o estado atual é **mantido** (num motor recém-criado isso equivale aos padrões); nunca derruba o processo.
- **JSON válido com conteúdo hostil**: cada campo é saneado (ausente/tipo errado → padrão; números limitados; textos
  cortados; ids de efeito/parâmetro validados; no máximo 8 efeitos por faixa).
- **Migração**: sem `schema` = protótipo (chaves curtas `trim`, `fader`, `delay`, `device`, `typeId`...) é migrado;
  `schema` maior que 1 é lido com tolerância (campos desconhecidos ignorados) e gera aviso em `settingsWarnings()`.
- **Efeito desconhecido/ausente** (plugin não instalado): a entrada é preservada como `missing` (passa o áudio sem
  processar, aparece na UI, e é gravada de volta com seus parâmetros) sem afetar o restante da cadeia.

## Números medidos (Linux, GCC 13, `-O3`, 1 núcleo)

| Medida | Resultado |
|---|---|
| Custo do motor, 1 faixa ativa (2 efeitos falsos, limitador, todos os medidores) | ≈ 125 µs por bloco de 10 ms = 1,25 % de um núcleo |
| Idem com 8 faixas ativas | ≈ 185 µs por bloco = 1,85 % de um núcleo (**≈ 9 µs ≈ 0,09 % por faixa adicional**; a base é o Program) |
| Deriva de ±150 ppm entre captura simulada (48 kHz / 44,1 kHz) e o relógio do motor, 60 s | 0 underruns, 0 overruns, sinal sem descontinuidades |
| Monitor: dispositivo 150 ppm mais rápido, 80 s | 0 underruns/overruns; ppm estimado ≈ −150 |
| Seno 1 kHz −23 dBFS: LUFS short-term da faixa | −22,99 LUFS (referência do `hb_audio_tests`) |
| 30 s simulados com consumidor | 3000 blocos de 480 quadros, contínuos, 0 xruns |

Os testes (`hb_audio_io_tests`, 49) usam tempo simulado (relógio e dispositivos falsos) e o backend *null*; cada um
roda em < 2 s. Foram executados também com ThreadSanitizer (sem avisos) e ASan/UBSan.

## Não validado / limitações

- **Nenhum hardware real foi testado**: nem WASAPI (captura, loopback, reprodução), nem PulseAudio/ALSA com
  dispositivos físicos, nem MSVC (o código foi escrito para compilar sem extensões, mas só GCC foi usado). O
  comportamento do loopback quando nada toca (sem pacotes → `NoSignal`) e a notificação de dispositivo removido
  vêm da documentação do miniaudio/WASAPI, não de medição.
- Latência ponta a ponta real (microfone → fones/Program) não foi medida; os números acima são de projeto.
- Loopback só existe no WASAPI (Windows). No Linux, fontes "monitor" do PulseAudio aparecem como capturas comuns.
- Duas faixas no mesmo dispositivo abrem o dispositivo duas vezes (modo compartilhado). Modo exclusivo não é usado.
- A taxa de um dispositivo que muda sozinha só é percebida na próxima reabertura (a faixa fica em silêncio ~50 ms
  enquanto o compensador é reconstruído).
- O monitoramento é "cue" por faixa (pré-fader, pós-efeitos); não há retorno do Program nos fones.
- Callbacks do OS e `consumers` não têm prioridade de tempo real garantida em Linux (sem `SCHED_FIFO`).
- Sob ASan, o `libpulse` do sistema vaza ~20 KB quando não há servidor PulseAudio (miniaudio/libpulse, não o nosso código).
- Plugins nativos: a troca de plugin em uso e o `Effect::reset()` na reativação do bypass não são tratados aqui.
