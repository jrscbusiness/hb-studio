# Gravação do Program Output (módulo `recording`)

Núcleo portátil (GCC/MSVC, sem dependências externas) que grava o Program Output (vídeo BGRA + áudio float
estéreo) em vários formatos. Os escritores do Windows (Media Foundation: MP4/H.264/HEVC/AAC, WMV, MP3, M4A) são
registrados por fora deste módulo no `WriterRegistry`; o `Recorder` só conhece formatos pelo `id` do perfil
(`WriterRegistry::find`) e funciona com qualquer `IMediaWriter`.

```
src/recording/
  media_writer.hpp      contrato CONGELADO (IMediaWriter, RecordingProfile, WriterRegistry)
  wav_writer.*          WAV PCM 16/24 bits e float32, WAVE_FORMAT_EXTENSIBLE
  flac_writer.*         codificador FLAC próprio (lossless) + md5.*
  jpeg_encoder.*        JPEG baseline próprio (4:2:0 / 4:4:4, qualidade 1-100)
  avi_writer.*          AVI Motion-JPEG + PCM, índice idx1
  jpeg_seq_writer.*     pasta com frame_000001.jpg... + audio.wav
  portable_writers.cpp  registerPortableWriters()
  recorder.*            Recorder (fila, relógio de áudio, segmentação, guarda de disco, eventos)
  file_naming.*         modelo de nome, sanitização, nunca sobrescrever
```

## Formatos registrados por `registerPortableWriters()`

| id | extensão | conteúdo | observações |
|---|---|---|---|
| `wav` | `.wav` | PCM 24 bits | usa `WAVE_FORMAT_EXTENSIBLE` (24 bits) |
| `wav-16` | `.wav` | PCM 16 bits | `fmt` clássico até 2 canais |
| `wav-f32` | `.wav` | float 32 bits | chunk `fact`; sem conversão nem limitação de nível |
| `flac` / `flac-16` | `.flac` | FLAC 24 / 16 bits | sem perdas, 1 a 8 canais |
| `avi-mjpeg` | `.avi` | Motion-JPEG + PCM 16 bits | todos os quadros são chaves |
| `jpeg-seq` | (pasta) | JPEG por quadro + `audio.wav` | a "extensão" é vazia: o caminho é uma pasta |

Regras comuns a todos: nunca lançam exceção (erro de E/S vira `false` + mensagem em pt-BR/ASCII), `close()` é
idempotente e deixa o arquivo válido mesmo após erro de escrita (melhor esforço), caminhos são UTF-8
(`std::filesystem` com `std::u8string`), `stats()` reflete exatamente o que foi escrito. O escritor nunca
reamostra: grava a taxa do `AudioFormat` recebido em `open()`.

### WAV
Cabeçalho `RIFF/WAVE`, `fmt ` e `data` corrigidos no `close()`. Usa `WAVE_FORMAT_EXTENSIBLE` para 24 bits ou mais
de 2 canais (máscara de canais para 1 a 8 canais; GUID `KSDATAFORMAT_SUBTYPE_PCM`/`IEEE_FLOAT`). Dados com tamanho
ímpar recebem byte de preenchimento. Limite de ~4 GB do RIFF: o escritor devolve erro "limite de 4 GB" e o
`Recorder` segmenta antes disso (veja Segmentação).

### FLAC
Codificador próprio, FLAC válido: quadros de 4096 amostras (último menor), subquadros *constant*, *fixed* (ordens 0 a
4) e *verbatim* (escolhe o menor), resíduo com partições Rice (ordem 0 a 8, parâmetros de 4 ou 5 bits), decorrelação
estéreo por quadro (independente, L/S, S/R, M/S), CRC-8 e CRC-16, `STREAMINFO` com MD5 das amostras originais,
atualizado no `close()`. 16 ou 24 bits (32 vira 24). Não implementa predição LPC (por isso comprime menos que o
`flac -8`) nem *wasted bits*.

### JPEG baseline (`jpeg_encoder`)
Sequencial, 8 bits, Huffman com as tabelas padrão do Anexo K, quantização IJG escalada pela qualidade (1 a 100),
4:2:0 (padrão) ou 4:4:4, JFIF, conversão BGRA para YCbCr BT.601 (faixa total), DCT em ponto flutuante, preenchimento
das bordas por replicação. Aceita qualquer largura/altura de 1 a 65535. Não há otimização de Huffman nem
progressivo.

### AVI Motion-JPEG
`RIFF 'AVI '` com `LIST hdrl` (`avih`, `strl` de vídeo `MJPG` e de áudio PCM), `LIST movi` e `idx1` (todas as entradas
com `AVIIF_KEYFRAME`, offsets relativos ao fourcc `movi`). O áudio é intercalado depois de cada quadro de vídeo com
exatamente as amostras daquele período (`round(n * taxa * den / num)`), então NTSC (30000/1001) não desliza. O áudio
no AVI é sempre PCM 16 bits. Taxa de quadros constante (os `pts` são ignorados; o `Recorder` garante um
`writeVideo` por intervalo). OpenDML não é usado: o arquivo é limitado a ~4 GB (o `Recorder` segmenta).

### Sequência JPEG
Pasta (criada) com `frame_000001.jpg`, `frame_000002.jpg`... e `audio.wav` ao lado. Quadros repetidos pelo
`Recorder` viram arquivos repetidos, então `ffmpeg -framerate 30 -i frame_%06d.jpg -i audio.wav` remonta o programa
em sincronia.

## Recorder

```cpp
RecorderConfig cfg;               // profileId, folderUtf8, nameTemplate, profile, video, audio, split*, minFreeBytes
Recorder rec(cfg, &bus, &logger);
std::string erro;
if (!rec.start(erro)) { /* erro em pt-BR, state == Error */ }
// consumidor do ProgramOutput:  rec.pushVideo(frame);   // shared_ptr<const Frame>
// fonte de áudio do Program:    rec.pushAudio(ptr, frames);   // float intercalado
rec.stop();                       // bloqueia até finalizar o(s) arquivo(s)
auto s = rec.status();            // state, file, seconds, contadores, bytes, freeBytes, segment, error
```

### Modelo de threads e filas
- Uma thread de escrita própria. `start()` abre o primeiro arquivo de forma síncrona (erros voltam em `start`).
- `pushVideo`/`pushAudio` **nunca bloqueiam**: trabalham em O(1) sob um mutex curto (cópia de ponteiro / `memcpy`
  de um bloco de áudio) e descartam contando quando o buffer limitado enche. Fila de vídeo: 8 quadros
  (`videoQueueFrames`), descarta o mais antigo (`droppedVideo`). Anel de áudio: 2 s (`audioRingSeconds`), descarta
  o áudio mais antigo (`droppedAudio`) e **substitui por silêncio** para manter a duração e a sincronia
  (`silenceFrames`).
- Quadros com tamanho diferente de `cfg.video`, stride inválido ou dados curtos são descartados e contados em
  `droppedVideo`.
- Falha do escritor (disco removido, erro de E/S, exceção) leva a `State::Error`, fecha o arquivo se possível e
  publica `recording.error`; chamadas de `push*` continuam seguras e instantâneas (são ignoradas). `stop()` depois de
  um erro apenas junta a thread e mantém `Error`.
- `stop()` chamado de dentro de um handler do EventBus (na thread de escrita) não faz `join`: só pede a parada.

### Relógio, taxa constante e alinhamento A/V
- **O áudio é o relógio**: `pts` de áudio = amostras acumuladas / taxa. O vídeo sai em taxa CONSTANTE: o intervalo
  `k` (de `k/fps` a `(k+1)/fps`) é escrito quando o áudio já cobre seu fim. Se não há quadro novo na fila, o
  último quadro é repetido (`repeatedVideo`); se sobram quadros, eles esperam na fila (e são descartados se ela
  encher). `videoFrames` conta os intervalos escritos, incluindo repetidos.
- **Alinhamento no início**: o instante zero é a chegada do **primeiro quadro de vídeo**. O áudio que chegou antes
  dele (pré-roll) é descartado e não conta como perda; o primeiro intervalo de áudio começa na primeira amostra
  empurrada depois dele. A precisão do alinhamento é a de um `pushAudio` (tipicamente 10 a 20 ms).
- Formatos sem vídeo: o zero é o `start()`. Formatos sem áudio (apenas vídeo): o relógio é o relógio monotônico
  (injetável em `cfg.nowNs`).
- **Falta de áudio**: se o áudio atrasa mais que `audioStallToleranceMs` (1 s) em relação ao relógio de parede, o
  buraco é preenchido com silêncio para o vídeo não congelar (`silenceFrames`). Com áudio saudável isso nunca
  dispara. Se o perfil exige áudio mas ninguém chama `pushAudio`, o vídeo avança com 1 s de atraso e preenchimento
  de silêncio: use um perfil sem áudio nesse caso.
- Quadros de vídeo chegam a 30 fps e as amostras de áudio ficam em `pts = n/taxa`; os `pts` entregues ao escritor
  são relativos ao início do **arquivo atual** (reiniciam a cada segmento).

### Conversão de taxa de amostragem
O áudio do Program chega em 48 kHz, estéreo, float (`cfg.audio` descreve o que é empurrado). A taxa gravada é
`profile.audioSampleRate` (44100/48000/96000; vale o perfil registrado, ou o de `cfg.profile` se `cfg.profile.id`
não for vazio). Se for diferente da taxa de entrada, o `Recorder` converte com `hb::audio::Resampler` (qualidade
High, 64 taps) **antes** do `writeAudio` e passa ao `open()` um `AudioFormat{profile.audioSampleRate, canais}`
idêntico ao entregue. Se a taxa é a mesma, não há conversão. Limite: até 8 canais com conversão. Medido: 3 s de seno
de 1 kHz a 48 kHz gravado em 44,1 kHz: 132268 quadros (2,9993 s) e 1000,00 Hz na releitura. Os últimos ~32 quadros
de latência do filtro não são emitidos no `stop()`.

### Perfil e tunables
`cfg.profileId` escolhe o perfil registrado (id, rótulo, extensão, `hasVideo`/`hasAudio` sempre vêm do registro).
Se `cfg.profile.id` não for vazio, seus tunables (`videoBitrateKbps`, `videoQuality`, `audioBitrateKbps`,
`audioSampleRate`, `audioBitsPerSample`, `keyframeIntervalSec`) substituem os do registro.

### Segmentação
`splitSeconds` e/ou `splitBytes` (0 = desligado). Os arquivos seguintes se chamam `<nome>_part002.<ext>`,
`_part003`... (o primeiro não tem sufixo). O corte é feito entre intervalos de vídeo (com áudio, na amostra exata
do fim do intervalo) ou, em formatos só de áudio, na amostra exata de `splitSeconds * taxa`: **nenhuma amostra de
áudio se perde ou duplica entre arquivos** (testado somando os arquivos). O tamanho é verificado no
`WriterStats::bytesWritten` ao fim de cada intervalo. Para extensões `wav` e `avi` há um teto automático de 3,9 GB
(RIFF de 32 bits). Nunca é criado um segmento vazio no fim. Falha ao abrir o próximo segmento: o anterior já foi
finalizado, estado `Error`.

### Guarda de disco
No `start()` e a cada ~1 s (relógio injetável) o espaço livre da pasta é consultado
(`std::filesystem::space`, ou `cfg.freeSpaceBytes`). Abaixo de `minFreeBytes` (padrão 500 MB) o arquivo é
**finalizado com segurança** (`close()`), o estado vira `Error` com a mensagem "Disco cheio: restam X MB livres
(mínimo Y MB)" e é publicado `recording.error`. Se o espaço não puder ser consultado, não se gera alarme falso.

### Nomes de arquivo
`nameTemplate` aceita `{date}` (2026-10-21), `{time}` (14-05-33) e `{n}`. Se há `{n}`, usa-se o primeiro número
livre a partir de 1; sem `{n}`, se o arquivo existir acrescenta-se ` (2)`, ` (3)`... — **nunca se sobrescreve**.
O nome é sanitizado: caracteres de controle e `< > : " / \ | ? *` viram `_`, `..` é removido (sem *path
traversal*), pontos/espaços à direita e pontos à esquerda caem, nomes reservados do Windows (`CON`, `NUL`, `COM1`...)
ganham `_` na frente, comprimento máximo de 120 bytes em fronteira UTF-8, vazio vira `HBStudio`. A pasta
(`folderUtf8`) é criada se não existir.

### Eventos e logs
Publicados no `EventBus`: `recording.started` (`path`, `file`, `profile`), `recording.segment` (`path`, `file`,
`segment`), `recording.stopped` (`path`, `file`, `seconds`, `bytes`, `segments`), `recording.error` (`message`,
`file`). O `Logger` recebe apenas o **nome do arquivo**, nunca o caminho da pasta do usuário.

## Testes e medições
`hb_recording_tests` (68 testes, GoogleTest). Cada escritor é verificado por **releitura com um leitor independente
escrito no teste** (`tests/recording_test_util.hpp`): parser RIFF/WAV, decodificador FLAC mínimo (confere CRC-8,
CRC-16, MD5 do STREAMINFO e compara amostra a amostra), parser AVI (confere tamanhos de listas, `idx1` entrada a
entrada contra os chunks, `avih`/`strh`/`strf`) e decodificador JPEG baseline (lê as tabelas do próprio arquivo).
O `Recorder` é testado com `FakeWriter` (falhas injetadas: `open`, escrita no meio, disco cheio, escritor bloqueado),
relógio sintético, duas threads produtoras, `stop()` durante a produção e TSan/ASan/UBSan; e com os escritores reais
gravando 3 s sintéticos de 1280x720@30 + 48 kHz e relendo.

Medições (Release, GCC 13, máquina de CI Linux):

| Item | Resultado |
|---|---|
| FLAC 16 bits estéreo (seno + ruído de -54 dB) | 56,3 % do tamanho do WAV |
| FLAC 24 bits estéreo (idem) | 61,5 a 64,9 % do WAV |
| Silêncio digital | < 600 bytes para 1 s estéreo |
| Ruído de escala cheia (incompressível) | até +5 % (cai em *verbatim*) |
| JPEG 1280x720, 4:2:0, q85, quadro difícil (barras, listras de 3 px) | 63 kB, PSNR 38,9 dB |
| JPEG 1280x720, 4:4:4, q95 | 96 kB, PSNR 59,3 dB |
| JPEG 640x360 4:2:0, q20 / q50 / q90 / q100 | 28,7 / 31,5 / 32,9 / 33,0 dB (o limite é a perda de croma nas barras) |
| AVI MJPEG q85 1280x720@30, 3 s, áudio 48 kHz PCM16 | 6,27 MB (~2 MB/s), PSNR mín. 38,6 dB, 90 quadros + 144000 amostras exatos |
| Codificação JPEG 720p 4:2:0 q85 (um núcleo, -O2) | ~18 ms/quadro no quadro de teste; 33 ms/quadro com ruído (pior caso, 627 kB) |

PSNR mínimo documentado: >= 30 dB (4:2:0, q85) e >= 38 dB (4:4:4, q95) no quadro sintético difícil do teste;
fotos reais costumam ficar melhores. Validação cruzada opcional (quando `ffmpeg`/`ffprobe` existem): o `ffmpeg`
decodifica o FLAC sem erro de CRC/MD5 com amostras idênticas e o JPEG sem erro.

## Limitações e o que NÃO foi verificado
- Não foram testados players reais (VLC, Windows Media Player, DaVinci, Premiere...), apenas os leitores de teste e,
  quando disponível, o `ffmpeg`. A compatibilidade com cada player é *não validada*.
- Sem MSVC neste ambiente: o código segue as regras de portabilidade (sem extensões, `std::u8string` nos caminhos)
  e compila sem avisos em GCC com `-Wall -Wextra -Wpedantic`, mas o build MSVC `/W4` não foi executado aqui.
- FLAC sem LPC e JPEG sem Huffman otimizado: arquivos maiores que os dos codificadores de referência (não
  comparados quantitativamente aqui). AVI sem OpenDML (limite de 4 GB por arquivo, resolvido por segmentação).
- Velocidade do JPEG: DCT em ponto flutuante simples, sem SIMD; 720p30 fica no limite de tempo real em um núcleo
  (33 ms/quadro no pior caso) e 1080p60 exige otimização/multithread ou um codificador de hardware (Media
  Foundation no Windows). Se a thread de escrita não acompanhar, a fila enche e o `Recorder` descarta/repete quadros
  (visível em `droppedVideo`/`repeatedVideo`) sem afetar quem chama.
- A sincronia A/V depende de `pushAudio` ser chamado no ritmo do dispositivo; a granularidade do alinhamento no início
  é a de um bloco de áudio. Deriva entre o relógio do dispositivo de áudio e o relógio de parede não é corrigida
  (o áudio manda).
- A guarda de disco e o erro de escrita são detectados na granularidade de buffer (256 kB do `OutFile` + cache do
  sistema): um disco removido pode ser notado só no próximo `flush`.
