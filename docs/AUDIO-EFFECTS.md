# Efeitos de áudio por canal

Código em `src/audio/effects/` (biblioteca `hb_effects`), adaptador de plugins em `src/audio/plugin_effect.{hpp,cpp}`.
Todos implementam `hb::audio::Effect` (`src/audio/effect.hpp`, contrato congelado) e são registrados por
`hb::audio::registerBuiltinEffects()` no `EffectRegistry` com `source = "builtin"`.

## Garantias de tempo real

- `process()` não aloca, não usa locks, não lança exceções (verificado por teste que substitui `operator new` e conta
  alocações; `prepare()` é o único lugar que aloca).
- `setParam()` / `getParam()` usam `std::atomic<float>` (qualquer thread). Cada valor é limitado à faixa; listas
  (`choices`) são arredondadas para o índice. Ids desconhecidos e `NaN` são ignorados.
- Suavização: ganhos/frequências/Q são suavizados (polo único, constante de tempo 20 ms) a cada sub-bloco de 32 amostras;
  coeficientes de filtro são recalculados só quando mudam. Sem "zipper noise". Tempos de ataque/release das dinâmicas
  não precisam de suavização (o ganho já é suavizado por eles).
- Entrada com `NaN`/`Inf` é substituída por 0 antes do processamento; a saída é verificada de novo (se algo não-finito
  surgir, o bloco é zerado e o estado é reiniciado). Denormais: estados dos filtros em `double` com flush
  (`|x| < 1e-30 → 0`) e saída com flush.
- 1 a 8 canais (intercalados), 44,1 / 48 / 96 kHz testados (aceita 8 kHz a 384 kHz). `reset()` limpa o estado.
- Mono/estéreo: detectores de dinâmica são **linkados** (mesmo ganho para todos os canais, mantém a imagem).
- `process()` aceita qualquer tamanho de bloco (1 amostra em diante).

## Efeitos

### Utilitários

| id | Parâmetros (id: faixa, padrão) | Notas |
|---|---|---|
| `hb.gain` Ganho (trim) | `gain_db`: −24…+24 dB, 0 | rampa linear dentro de cada sub-bloco |
| `hb.phase` Inverter polaridade | `invert`: Normal / Invertida | troca em ~5 ms (sem estalo); Normal é bit a bit idêntico |
| `hb.mono` Mono | `mode`: L+R / Somente L / Somente R | os canais 1 e 2 recebem a mesma mistura (L+R = média); com 1 canal não faz nada; canais 3+ intactos |
| `hb.delay` Atraso (lip-sync) | `time_ms`: 0…2000 ms, 0 | atraso exato em amostras (arredondado); ao mudar, crossfade de 10 ms entre as duas posições (sem clique e sem glissando de pitch) |

### Equalização

| id | Parâmetros | Notas |
|---|---|---|
| `hb.highpass` Passa-altas | `freq`: 20…500 Hz (log), 80; `slope`: 12 / 24 dB/oitava | Butterworth; 12 dB = 1 biquad (Q 0,7071), 24 dB = 2 biquads (Q 0,5412 e 1,3066). Troca de inclinação em crossfade de 5 ms |
| `hb.lowpass` Passa-baixas | `freq`: 2…20 kHz (log), 16 kHz; `slope`: 12 / 24 | idem; corte limitado a 0,45·fs |
| `hb.eq3` Equalizador de 3 bandas | `low_freq` 40…400 Hz (100), `low_gain` ±15 dB; `mid_freq` 200…8000 Hz (1000), `mid_gain` ±15 dB, `mid_q` 0,3…10 (1); `high_freq` 2…16 kHz (8000), `high_gain` ±15 dB | shelf grave e agudo (RBJ, S = 1), sino médio (RBJ). Com ganhos 0 é transparente |

### Dinâmica

| id | Parâmetros | Notas |
|---|---|---|
| `hb.gate` Gate | `threshold_db` −80…0 (−50); `range_db` 0…80 (60) = redução máxima; `attack_ms` 0,1…100 (1); `hold_ms` 0…2000 (50); `release_ms` 5…2000 (150) | detector de pico (decaimento 10 ms) linkado; histerese de 3 dB (fecha em limiar −3 dB); sem look-ahead. `gainReductionDb()` = redução atual |
| `hb.compressor` Compressor | `threshold_db` −60…0 (−18); `ratio` 1…20 (3); `attack_ms` 0,1…200 (10); `release_ms` 5…2000 (120); `knee_db` 0…24 (6); `makeup_db` 0…24 (0); `detect`: RMS / Pico | feed-forward, joelho quadrático, GR suavizada em dB. O nível do detector é "equivalente de pico": uma senoide de amplitude A lê 20·log10(A) nos dois modos (RMS com janela de 10 ms). `gainReductionDb()` = maior GR do bloco |
| `hb.limiter` Limitador | `ceiling_db` −20…0 dBFS (−1); `release_ms` 10…1000 (100) | look-ahead de pico (não *true peak*). **Latência = `limiterLatencyFrames(sr)` = 1,5 ms** (72 amostras a 48 kHz, 66 a 44,1 kHz, 144 a 96 kHz); o efeito atrasa o áudio por esse tempo (o mixer deve compensar se alinhar com outros canais). Nunca ultrapassa o teto (um *clamp* final garante). `gainReductionDb()` = maior GR do bloco |
| `hb.deesser` De-esser | `freq` 3…10 kHz (log, 6500); `threshold_db` −60…0 (−30); `ratio` 1…20 (4) | split-band: passa-faixa (Q 1,5) alimenta o detector; só a banda é atenuada (`y = x − (1−g)·bp(x)`), graves ficam intactos. Redução máxima 24 dB. Ataque 0,5 ms, release 40 ms |

#### Algoritmo do limitador
`r = teto/pico` (≤ 1) por amostra; `m` = mínimo de `r` nas últimas L amostras; ganho = média móvel de `m` (a redução já
está totalmente aplicada quando o pico chega ao fim do atraso); release de um polo que nunca passa do ganho calculado.
O limitador existente em `limiter.*` (bus de programa, estéreo fixo) não foi reutilizado porque este precisa de N canais
e parâmetros por canal; usa o mesmo princípio.

## Usos recomendados (ao vivo)

Ordem típica numa faixa de voz: `hb.highpass` → `hb.gate` → `hb.eq3` → `hb.deesser` → `hb.compressor` → `hb.gain`.

| Fonte | Sugestão |
|---|---|
| Voz (locução/lapela) | Passa-altas 80–100 Hz, 12 dB. Gate: limiar ~ −45 dB, range 10–15 dB (gate suave, não corta a respiração), ataque 1–3 ms, hold 100 ms, release 200 ms. Compressor: 3:1 a 4:1, limiar −18 a −22 dB, ataque 10–15 ms, release 100–150 ms, joelho 6 dB, makeup para ~4 dB de GR média. De-esser: 6,5 kHz (voz masculina 5–6 kHz, feminina 7–8 kHz), limiar para ~3–6 dB de GR nos "s" |
| Voz com microfone de mão | Passa-altas 120 Hz, EQ: −2 a −3 dB em 250 Hz (Q 1), +2 dB shelf 8 kHz |
| Violão/guitarra acústica | Passa-altas 60–80 Hz, compressor 2:1 lento (ataque 25 ms, release 200 ms), sem gate |
| Instrumentos de música (bus) | Compressor 2:1, joelho 12 dB, ataque 30 ms; limitador a −1 dBFS no master |
| Entrada de linha com chiado | Passa-baixas 16 kHz; ganho de entrada (trim) para pico ~ −12 dBFS |
| Lip-sync | `hb.delay` no áudio: ~1 quadro = 33 ms (30 fps) / 40 ms (25 fps) por quadro de atraso de vídeo; HDMI/NDI tipicamente 2–6 quadros |
| Fonte mono em canal estéreo | `hb.mono` "Somente L" (ou R) para copiar o lado bom |
| Fase | `hb.phase` se dois microfones captam a mesma fonte e há cancelamento (som "oco") |

Estes valores são pontos de partida, não receitas validadas em sala real.

## Resultados medidos (testes automáticos, GCC 13 / Clang, Release)

Medidos passando senoides por `process()` (ver `tests/effects_*.cpp`, linhas `[MEASURE]` na saída do executável):

| Medida | Resultado | Critério do teste |
|---|---|---|
| Passa-altas 80 Hz (12 e 24 dB), ganho em fc | −3,010 dB (ideal −3,010) | −3 ± 1 dB (44,1/48/96 kHz, 40–500 Hz) |
| Passa-altas, inclinação medida (fc/4 → fc/8) | 12,03 dB/oit (12 dB) e 24,08 dB/oit (24 dB) | ± 1 dB |
| Passa-baixas 8 kHz, ganho em fc | −3,010 dB | −3 ± 1 dB (2–20 kHz) |
| EQ3 shelf grave +15 dB, ganho em DC | +15,00 dB | ± 0,15 dB (−15…+15, 3 taxas) |
| EQ3 shelf agudo +15 dB, ganho em Nyquist | +15,00 dB | ± 0,15 dB |
| EQ3 sino +10 dB, Q 2, no centro | +10,00 dB | ± 0,15 dB |
| Compressor (RMS) limiar −20, 4:1, joelho 0: entrada −6 dB / 0 dB | saída −16,52 / −15,02 dB (ideal −16,50 / −15,00) | ± 0,35 dB; modo pico ± 0,5 dB (ondulação do detector de pico numa senoide) |
| Compressor GR medido (entrada −10 dB, limiar −20, 4:1) | 7,52 dB (ideal 7,5) | ± 0,35 dB |
| Compressor ataque 10 ms: tempo até 63,2 % da GR final | 9,98 ms | ± 6 % |
| Compressor release 120 ms: tempo até 36,8 % da GR | 119,96 ms | ± 6 % |
| Gate fechado, sinal −60 dB com range 60 | atenuação −60,0 dB | ± 1 dB; abre em < 5 ms; hold respeitado |
| Limitador, senoide +12 dB, rajadas de ruído até 10×, impulsos ±8, onda quadrada; 1/2/8 canais, 44,1/48/96 kHz, tetos −0,1/−1/−6 dBFS | pico de saída ≤ teto + 1e−7 | |
| Limitador, GR para senoide +6 dB e teto −3 dBFS | 9,02 dB (ideal 9,0) | |
| Limitador latência | 72 amostras @ 48 kHz (1,5 ms); abaixo do teto a saída é exatamente a entrada atrasada | ± 1e−6 |
| De-esser 6,5 kHz −10 dB (limiar −30, 4:1) | −24,96 dB (ideal −25) | ± 1,5 dB |
| De-esser, graves 200 Hz junto | −20,00 dB (variação 0,002 dB) | ± 0,25 dB |
| Delay | exato em amostras para 0, 1, 10, 123,4 e 2000 ms em 3 taxas | igualdade exata |

Testes de tempo real: contagem de alocações em `process()`/`setParam()` = 0 para os 11 efeitos × 1/2/8 canais × 44,1/96 kHz;
teste de 2 threads (`setParam` aleatório durante `process`) passa também compilado com `-fsanitize=thread`
(`cmake -DHB_EFFECTS_TSAN=ON` cria `hb_effects_tsan_tests`).

## Como escrever um efeito plugin

Um plugin de efeito é uma biblioteca compartilhada que exporta uma função C (ver `src/plugins/hb_plugin_api.h`,
seção "Audio effect plugins"):

```c
HB_PLUGIN_EXPORT int hb_plugin_get_effects_v1(hb_effects_v1* out);
```

O host preenche `out->struct_size` e `api_version`; o plugin preenche os ponteiros e retorna 0:

| Função | Papel |
|---|---|
| `count()` | número de tipos de efeito (≤ 64) |
| `describe(index, desc, params, max)` | `type_id` (`[A-Za-z0-9._-]`), `name`, e até 32 parâmetros (`id` `[a-z0-9_]`, `label`, `unit`, `min_value < max_value`, `default_value` dentro da faixa, `logarithmic`). Retorna 0 |
| `create(index, sample_rate, channels)` | cria a instância (ou NULL). Chamada de `prepare()` do host |
| `set_param(inst, param_index, value)` | índice na ordem de `describe`. O host só chama da thread de áudio, imediatamente antes de `process()`, e só quando o valor mudou. Suavize internamente |
| `process(inst, interleaved, frames)` | in-place, float32 intercalado em [−1, 1]. Tempo real: sem alocação, locks, I/O |
| `reset(inst)` / `destroy(inst)` | limpar estado / liberar |
| `shutdown()` | opcional |

Regras que o host aplica (`describePluginEffects`, tudo-ou-nada): ponteiros obrigatórios não nulos, strings com NUL dentro
do buffer, UTF-8 válido e sem caracteres de controle, ids únicos, valores finitos, `min < max`, padrão dentro da faixa,
`logarithmic` só com `min > 0`, no máximo 64 efeitos e 32 parâmetros por efeito.

Proteções em `PluginEffect` (`createPluginEffect`): entrada `NaN/Inf` é zerada antes de chegar ao plugin; exceções C++
são capturadas; se a saída tiver `NaN/Inf` o bloco é zerado e conta um erro (`errorCount()`); após
`kPluginMaxConsecutiveBadBlocks` (8) blocos inválidos seguidos o efeito passa a fazer *bypass* (`bypassed()`), até o
próximo `prepare()`. `PluginEffectLibrary::keepAlive` mantém a DLL carregada enquanto houver instâncias.
Ainda assim, código de plugin roda no processo do HB Studio com os mesmos privilégios: instale só plugins confiáveis
(uma violação de memória no plugin não pode ser contida).

Uso típico no host:

```cpp
auto lib = std::make_shared<PluginEffectLibrary>();   // api preenchida após dlopen + hb_plugin_get_effects_v1
std::vector<PluginEffectDesc> descs; std::string err;
if (describePluginEffects(*lib, descs, err))
  for (size_t i = 0; i < descs.size(); ++i)
    EffectRegistry::instance().add({descs[i].typeId, descs[i].name, "Plugin", pluginId},
        [lib, i, d = descs[i]] { return createPluginEffect(lib, int(i), d); });
```

## Limitações conhecidas / não validado

- Limitador é de pico de amostra (sem *oversampling*/true-peak): picos entre amostras podem passar do teto após D/A ou conversão.
- `hb.delay` arredonda para amostras inteiras (resolução 21 µs a 48 kHz).
- Compressor em modo "Pico" tem ondulação de ~0,3–0,4 dB na GR para senoides (o detector de pico segue cada ciclo).
- Mudança do teto do limitador é imediata (não suavizada), para manter a garantia de nunca ultrapassar o valor atual.
- Não medido: uso de CPU (não há benchmark), 8 canais só testado por estabilidade/alocação/comportamento básico,
  MSVC (não havia compilador disponível; o código usa só C++20 padrão e foi compilado sem avisos em GCC e Clang com
  `-Wall -Wextra -Wpedantic`), e nenhum ensaio de audição subjetiva em hardware real.
