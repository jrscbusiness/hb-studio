# Gravação multipista (ISO)

Grava cada câmera e cada canal do mixer em arquivos isolados, para editar depois, **junto** com a gravação normal do
Program. Módulo portátil: `src/multitrack/` (testado em Linux/GCC; o escritor H.264 do Windows está em
`src/recording/windows/mf_writers.cpp`). Camada do aplicativo: `src/app/iso_service.*`. Aba GRAVAÇÃO > "Multipista (ISO)".

> **Não validado em hardware real.** Tudo abaixo foi verificado com testes unitários, escritores falsos e o escritor
> WAV real. Nada foi gravado com câmeras, placas de som ou codificadores de verdade (veja "Limites e honestidade").

## Modelo da sessão

Se **nenhuma** câmera/canal estiver marcado (padrão), a gravação é exatamente a de sempre (um arquivo em
`<Vídeos>\HB Studio`). Com algo marcado, a gravação vira uma **sessão** = uma pasta:

```
<pasta de gravação>\2026-11-24_14-05-33\
  program.mp4            (ou .wav/.avi..., o formato escolhido; "program_part002" se dividir)
  iso\CAM01_<dispositivo>.mp4      H.264 sem áudio, câmera LIMPA (sem overlays/transições)
  audio\CH01_<canal>.wav           um arquivo por canal do mixer (estéreo, 48 kHz)
  edl\CAM01.edl  edl\CH01.edl      listas CMX 3600 (uma por trilha)
  session.json                     manifesto de sincronismo
```

- **Câmeras**: cada câmera ao vivo marcada é amostrada diretamente do *slot* (a câmera, antes de overlays, chroma key
  e transições), na resolução nativa (padrão) ou 1080p/720p/540p, em taxa constante (igual ao Program ou 24/25/30/50/60).
  Câmeras sem sinal (padrão de teste) **nunca** são gravadas; a seleção delas é recusada/avisada.
- **Áudio**: cada faixa do mixer marcada é capturada **antes do fader** (padrão: depois de ganho, fase, mono, atraso e
  efeitos; antes de fader/mute/solo/pan) ou **depois do fader** (aplica o fader e o gate mute/solo, suavizado por
  bloco; ainda antes de pan, master e limitador). Formatos: WAV 24 bits (padrão), 16 bits, float 32, FLAC, M4A/AAC
  (quando disponível). O ponto de captura é uma tomada nova no `AudioEngine` (`addStripConsumer`).

## Sincronismo (o núcleo)

Todas as trilhas usam **um único relógio monotônico de sessão** (`SessionClock`, `steady_clock`; tempo zero = início
da sessão). O relógio de parede é registrado uma vez, só para informar no manifesto. A lógica pura está em
`track_timeline.*` (sem E/S, sem threads) e é testada com tempos injetados:

- **Vídeo (`VideoTimeline`)**: o carimbo vira um *slot* de taxa constante, com aritmética exata (`30000/1001` não
  deriva; testado até 48 h). Quadro para slot já ocupado: descartado e contado. Lacuna: até 2 s é preenchida repetindo
  o quadro anterior (taxa constante); acima disso o pts salta (contado em `jumped`). O arquivo começa no primeiro quadro
  (pts 0) e `startOffsetNs` no manifesto diz onde ele cai na sessão.
- **Áudio (`AudioTimeline`)**: alinhamento no nível da amostra. O primeiro bloco gera silêncio inicial (ou recorte) para que a
  **amostra 0 do WAV seja o início da sessão** (`leadingSilenceFrames`). Depois: (a) lacuna/sobreposição > 200 ms é
  corrigida na hora (silêncio/corte); (b) abaixo disso os carimbos têm jitter (são tomados na chegada do bloco), então só o
  **mínimo** do desvio numa janela de 1 s é confiável; acima de 2 ms o desvio é absorvido **gradualmente** (repete ou
  descarta até 8 amostras por bloco de 10 ms), sem estalo audível. `driftPpm` no manifesto é a correção média. Perda por
  fila cheia vira silêncio exato (a duração se mantém). Teste: relógio 200 ppm lento por 10 min termina com desvio
  residual < 6 ms.
- **Deriva e `drift_compensator`**: cada faixa de entrada do mixer já passa pelo `DriftCompensator` (clock do
  dispositivo → relógio do engine). O que sobra é engine × relógio da sessão, que é o que `AudioTimeline` absorve.
- **Programa**: `startOffsetNs` = primeiro quadro do Program (`Recorder::Status::t0Ns`, mesmo relógio monotônico).

### Limitação importante de sincronismo de vídeo
Como a API das câmeras (`LiveSource::render`) devolve "o último quadro recebido", a câmera é **amostrada** no relógio da
sessão; o carimbo é o instante da amostragem, não o da captura no sensor. Uma câmera de 25 fps gravada a 30 fps repete
quadros sem que isso seja detectável como perda; o `fps` medido da câmera aparece na UI. A precisão é ~1 quadro.

## Governança de recursos

- **Estimativa antes de gravar** (`resource_estimator.*`, mostrada na UI): taxa de disco (bitrate × N + áudio +
  Program), horas de disco restante e **carga de codificação** em "equivalentes de 1080p30 em software" comparada a um
  teto por nível (`core::Tier`): LOW 1,5 · MEDIUM 3 · HIGH 6 · PRO 10 (inclui o Program). Codificador por hardware pesa
  0,35. **Os números são heurísticas iniciais, a calibrar em máquinas reais.** `hwEncode` hoje vem do
  `detectHardware()` (falso até a camada de GPU preencher), então a estimativa é conservadora.
- Veredito: OK até o teto; **Atenção** até 1,5×; **Bloqueado** acima (a gravação é recusada), salvo "modo avançado".
  Disco: bloqueado se couber < 5 min, aviso se < 60 min.
- **Aviso de GPU**: placas GeForce limitam as sessões simultâneas de codificação por hardware (o limite depende do
  driver e já mudou entre versões); acima de ~3 codificações o aplicativo avisa. Não há detecção real do limite.
- **Disco baixo**: as trilhas ISO param **primeiro**, quando o espaço livre cai abaixo de `mínimo do Program + 1,5 GB`;
  o Program continua e só para no seu próprio mínimo (padrão 500 MB). As trilhas ISO são finalizadas e anunciadas.
- **Isolamento de falhas**: cada trilha tem fila limitada com descarte do mais antigo (contado), thread própria e
  estado próprio. Um escritor que falha põe **só aquela trilha** em erro; as demais e o Program seguem. Nada disso roda
  no relógio do Program: se a codificação atrasa, só a trilha ISO perde quadros (reportados em "Perdidos").

## Divisão em segmentos e segurança contra falhas

- Divisão opcional a cada N minutos (padrão desligada) no mesmo instante de sessão para todas as trilhas
  (`CAM01_x_001.mp4`, `_002`...). O Program usa o mesmo intervalo, se você não configurou outro.
- WAV acima de ~3,5 GB gira sozinho (limite de 4 GB do RIFF), mesmo sem divisão.
- **Cada arquivo finalizado publica `recording.fileClosed`** `{path, kind: "program"|"iso-video"|"iso-audio", index}`
  (`index` = número do segmento, 1-based; ISO também envia `track`). O `Recorder` do Program também passou a publicar
  (inclusive segmentos girados), só quando o arquivo fechou sem erro.
- **O que sobrevive a uma queda do aplicativo/energia**:
  - `session.json` é reescrito atomicamente a cada ~5 s (`state: "recording"`) e ao parar (`"finished"`).
  - **WAV**: o cabeçalho é regravado a cada 2 s; após uma queda o arquivo lê corretamente até a última sincronização
    (testado lendo o arquivo antes do `close()`).
  - **MP4 ISO**: tenta MP4 **fragmentado** (`MFTranscodeContainerType_FMPEG4`) e cai para MP4 clássico se o Windows
    recusar. Fragmentado: toca até o último fragmento. Clássico: o `moov` só é escrito no fim, então um MP4 aberto é
    perdido numa queda; a divisão em segmentos limita a perda ao segmento atual. **Não verificado no Windows real**
    (o teste `RecWin.Mp4H264IsoVideoOnly` só roda no CI do Windows e não distingue os dois modos).
  - Program (MP4 clássico/AVI/etc.): comportamento anterior, inalterado.

## session.json (esquema 1)

`session` (nome), `state`, `startWallClockUtc`/`startWallClockUnixMs`, `monotonicT0Ns`, `durationNs`,
`rotationSeconds`, `syncNote`, `warnings[]` e `tracks[]`: `id`, `kind` (`program`|`video`|`audio`), `name`,
`device`, `codec`, `sourceIndex`, formato (`width/height/fpsNum/fpsDen` ou `sampleRate/channels/bitsPerSample/tap`),
`state`/`error`, `startOffsetNs`, `leadingSilenceFrames`, contadores honestos (`units`, `repeated`, `droppedQueue`,
`droppedTimeline`, `silenceFrames`, `jumped`, `driftPpm`) e `segments[]` (`file` relativo, `startNs`, `startUnit`,
`units`, `bytes`, `closed`). Para ressincronizar: posicione cada arquivo em `segments[i].startNs` da linha do tempo.
Segmentos do Program depois do primeiro têm `startNs` aproximado (início + N × intervalo de divisão).

## Exportação EDL

`edl/<trilha>.edl` (CMX 3600, `FCM` drop/non-drop correto para 29,97/59,94, linhas CRLF), **uma lista por trilha**
(uma EDL só tem uma faixa de vídeo/áudio). Timecode de origem 00:00:00:00; timecode de gravação = instante da sessão do
primeiro quadro/amostra, então importar todas em t=0 as ressincroniza. A aritmética de timecode é testada; **a
importação em DaVinci Resolve, Premiere e Final Cut não foi testada** (sem acesso). FCPXML/XML não foi implementado:
se a EDL não servir, use o `session.json`.

## Limites e honestidade

- Nada foi validado com câmeras, placas de captura ou codificadores reais. Carga e taxas são estimativas.
- N codificações H.264 simultâneas são caras; o teto por nível é um palpite inicial. O codificador do Media Foundation
  prefere hardware quando existe, mas não há controle do número de sessões da GPU (GeForce de consumo tem limite).
- Qualidade de ISO: bitrate automático ≈ 0,13 bit/pixel/quadro (1080p30 ≈ 8 Mbps); ajustável.
- A câmera é amostrada (ver acima), não temos o carimbo de captura do sensor.
- O áudio ISO é estéreo (as faixas do mixer são estéreo); sem canal mono nativo.
- O áudio da câmera não é gravado no MP4 ISO (vídeo só); o som está nas faixas do mixer.
- A tomada pós-fader não inclui rampa sample-accurate do mixer, pan, master nem limitador.
- Os comandos `rec|iso|...` não estão no controle remoto (apenas `rec|start/stop` já existentes).

## Comandos da UI

`rec|iso|cam|<slot>|0/1`, `rec|iso|ch|<faixa>|0/1`, `rec|iso|cfg|<res|fps|vkbps|afmt|tap|rotmin|expert>|<valor>`,
`rec|iso|open`. Estado em `rec_iso` (~4×/s): configuração, câmeras, canais, estimativa e, na sessão, estado/tempo/perdas
por trilha. Seleção e ajustes são salvos em `multitrack.json` e ficam travados durante a gravação.
