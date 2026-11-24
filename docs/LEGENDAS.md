# Legendas ao vivo

Reconhecimento de fala do áudio do **Program** (português primeiro; idioma selecionável) publicado como eventos
`caption.text` no EventBus. O módulo de GRÁFICOS (overlays) já assina esse tópico e desenha as legendas no
PROGRAM. A interface fica em **CONFIG → Legendas ao vivo** (nenhuma aba nova).

Código: `src/captions/`, testes `tests/captions_*.cpp`, build `cmake/modules/captions.cmake`.

## Contrato do evento

`caption.text` `{text, final:"1"|"0", lang}` — exatamente esses três campos.

* `final="0"`: hipótese parcial (substitui a linha em andamento). Só o último "bloco" (até 2 linhas de 42 caracteres,
  configurável) de uma hipótese longa é mostrado.
* `final="1"`: trecho confirmado. Frases longas viram vários eventos finais (um por bloco). `text` pode ter `\n`
  entre as linhas. Um final com `text` vazio significa "limpe a hipótese parcial" (o reconhecedor devolveu vazio).

## Motores

| Motor | Estado | Observações |
|---|---|---|
| **Local — whisper.cpp** (MIT) | implementado, **primário** | offline, CPU, modelos ggml baixados sob demanda |
| Reconhecimento embutido do Windows (SAPI / `Windows.Media.SpeechRecognition`) | **não implementado** | ver abaixo |
| **Nuvem — API compatível com OpenAI** (`POST {base}/audio/transcriptions`) | implementado, opcional | envia o áudio para o servidor informado |

**Por que não o reconhecimento embutido do Windows.** `Windows.Media.SpeechRecognition` só escuta o *microfone
padrão* (não aceita áudio arbitrário) e depende da configuração "reconhecimento de fala online" do Windows. SAPI 5
aceitaria um fluxo de áudio próprio, mas exige COM/gramática de ditado, e o reconhecedor pt-BR do SAPI raramente está
instalado. Escrever isso sem compilador Windows seria arriscado e o resultado, de qualidade inferior; por isso só
whisper.cpp (e a nuvem opcional) foram feitos, ambos atrás da interface `ISpeechRecognizer`
(`src/captions/recognizer.hpp`), então um terceiro motor pode ser plugado depois sem mexer no resto.

**Nuvem.** Serve OpenAI, Groq ou um servidor próprio (faster-whisper-server, whisper.cpp server). **Muitos provedores
não oferecem transcrição de áudio — o OpenRouter, por exemplo, não oferece**; confirme na documentação do provedor
antes de usar. Só `https://` é aceito (e `http://` apenas para `localhost`). Só trechos finais são enviados (sem
parciais) para limitar custo e número de requisições. A chave fica em `captions.json` (texto simples, como as
chaves de transmissão), nunca vai para o registro nem de volta para a interface.

## Como funciona (pipeline)

```
AudioEngine (thread do motor) --push--> AudioTapQueue (anel limitado, descarta o MAIS ANTIGO) 
   ... thread dedicada: ProgramDownmixer (48 kHz estéreo -> 16 kHz mono) -> Segmenter (VAD por energia)
   -> ISpeechRecognizer -> CaptionCommitter (anti-flicker) -> TextNormalizer -> EventBus "caption.text"
                                                         \-> SrtBuilder (um cue por bloco final)
```

* **Nunca bloqueia o áudio/vídeo**: a thread do motor só faz um `memcpy` curto sob mutex num anel pré-alocado. Se o
  reconhecedor atrasar, o áudio mais antigo é descartado (as legendas "pulam" para frente); o total descartado aparece
  na dica do indicador de latência. Enquanto o modelo carrega, o áudio acumulado é jogado fora.
* **Segmentação (VAD)**: porta de energia RMS com piso de ruído adaptativo (estatística de mínimo em janela de 6 s).
  Abre o trecho após 200 ms de voz (com 240 ms de pré-roll), fecha após 700 ms de silêncio, corta em pausa curta depois
  de 8 s e à força em 12 s. A cada 1,5 s de fala emite uma hipótese parcial. **Não é um VAD neural**: ruído constante
  forte, música ou um tom sustentado por mais de 6 s passam a ser tratados como ruído de fundo, e música com variações
  pode ser tratada como fala.
* **Whisper não é streaming**: cada parcial reconhece de novo o trecho inteiro até ali (até 12 s). Parciais são
  puladas quando a fila atrasa mais de 0,6 s, para o computador se recuperar.
* **Anti-flicker** (`CaptionCommitter`): comparação palavra a palavra ignorando maiúsculas e pontuação; palavras já
  mostradas e confirmadas mantêm a grafia; uma hipótese que é só prefixo mais curto do que já está na tela é ignorada.
  Finais multi-palavra idênticos ao anterior são suprimidos (laços de alucinação do Whisper).
* **Normalização**: remove anotações `[MÚSICA]`/`(aplausos)`/♪, ajusta maiúscula inicial e ponto final, filtro opcional de
  palavrões (lista pequena pt-BR/inglês, palavra inteira; é básico e não substitui moderação humana), quebra em linhas
  por caracteres.
* **SRT**: opção "Salvar .srt da sessão". Se uma gravação do Program for fechada (`recording.fileClosed`,
  `kind="program"`) durante as legendas, o `.srt` é gravado ao lado dela com o mesmo nome; senão vai para a pasta de
  gravações como `HBStudio_legendas_AAAAMMDD_HHMMSS.srt`. Os tempos contam a partir do **instante em que as legendas
  foram ligadas**; ligue as legendas junto com o início da gravação para o `.srt` alinhar com o vídeo. Gravações
  divididas: o `.srt` fica ao lado do último arquivo e contém a sessão inteira.

## Modelos (whisper.cpp)

Baixados sob demanda, **nunca** vão no repositório nem no instalador, para `%LOCALAPPDATA%\HBStudio\models`.
URL fixada na revisão `5359861c739e955e79d9a303bcbc70fb988958b1` do repositório `ggerganov/whisper.cpp` (Hugging Face),
tamanho e SHA-256 fixados em `src/captions/model_manager.cpp`; o arquivo só é usado depois de conferir tamanho e hash
(se não bater, é apagado). Os valores foram obtidos da API do Hugging Face e o SHA-256 do modelo leve foi conferido
baixando o arquivo.

| Escolha na tela | Arquivo | Disco | RAM (estimativa) |
|---|---|---|---|
| Modelo leve | `ggml-base-q5_1.bin` | 59,7 MB | ~300 MB |
| Modelo médio | `ggml-small-q5_1.bin` | 190 MB | ~700 MB |

## Custo de CPU e latência — o que foi medido e o que não foi

* **Medido** (sandbox Linux de 4 núcleos, whisper.cpp v1.7.6 compilado em Release, modelo leve,
  4 threads, 5 s de áudio sintético): ~1,5 s por reconhecimento (~0,3x tempo real) e ~90 ms para carregar o modelo.
  Isto só dá uma ordem de grandeza: **não foi medido em Windows, com o instalador final, nem com fala real**.
* **Estimado, não medido**: o modelo médio custa por volta de 3x o leve. Latência esperada para o final de uma frase
  ≈ 0,7 s (silêncio de fechamento) + tempo de reconhecimento do trecho ≈ 2–4 s no total em CPU de 4 núcleos modernos
  com o modelo leve. O indicador da tela mostra "tempo de reconhecimento + áudio acumulado na fila" do último trecho
  (não inclui os 0,7 s de silêncio de fechamento).
* O motor usa até 4 threads (metade dos núcleos lógicos, no mínimo 1) — durante o reconhecimento isso compete com a
  codificação de vídeo da transmissão/gravação. Se o indicador passar de ~4 s ou aparecer áudio descartado, use o
  modelo leve ou desligue as legendas. O build exige CPU x86-64 com **AVX2** (padrão do ggml; sem `-march=native` nem
  OpenMP para o instalador rodar em outras máquinas).

## Precisão (expectativas honestas)

Whisper base/small quantizados em português: bom com fala clara e um só locutor; erra mais com sotaque forte, vários
falando juntos, ruído, música de fundo e nomes próprios/termos incomuns, e pode **alucinar** frases em silêncio ou
música (ex.: "Obrigado por assistir"). Não há diarização (não separa quem fala). O modelo leve erra visivelmente mais que
o médio. **Nenhuma taxa de acerto foi medida aqui** — não existe corpus de fala real nos testes automatizados. Não use como
única fonte de acessibilidade em eventos que exijam legenda oficial; revise o `.srt` antes de publicar.

## Privacidade

* **Motor local: nada é enviado pela internet** durante o reconhecimento; o áudio é processado na memória e descartado
  (não é gravado em disco). A única conexão de rede do motor local é o **download do modelo** (Hugging Face), feito só
  quando o operador clica em BAIXAR.
* **Motor Nuvem: o áudio do Program (trechos de fala) é enviado ao servidor configurado** e está sujeito à política do
  provedor. Desligado por padrão.
* A transcrição aparece na tela do operador e no `.srt` (se ligado), que fica na pasta de gravações.

## O que NÃO foi validado

* **Nada do código Windows foi compilado ou executado**: `src/captions/windows/winhttp_client.cpp` (download com
  redirecionamentos do Hugging Face e POST da nuvem), a fiação em `app_main.cpp`, a janela de download na interface
  real (WebView2) e o carregamento de modelo com caminho contendo acentos (`%LOCALAPPDATA%` de usuário com acento depende
  de o ggml abrir caminhos UTF-8 no Windows). A CI do Windows é a primeira verificação real.
* A **compilação do whisper.cpp com MSVC** (estático, runtime estático) não foi tentada; só a compilação e a execução
  em Linux/GCC (`-DHB_CAPTIONS_WHISPER=ON`). Em Windows a opção vem ligada por padrão; desligue com
  `-DHB_CAPTIONS_WHISPER=OFF` se a build quebrar (o app continua, com o motor local indisponível e a nuvem opcional).
* Precisão e latência com fala real, vários idiomas, CPUs diferentes e carga simultânea de codificação.
* O motor de nuvem contra provedores reais (só testado com cliente HTTP falso: corpo multipart, WAV, tratamento de erro).
* O comportamento do VAD com música/ruído reais (só sinais sintéticos).

## Testes

`hb_captions_tests` (portável, Linux e Windows): normalizador, segmentador, committer, SRT, fila/downmix, sessão e serviço
com `FakeRecognizer` ponta a ponta (incluindo thread de trabalho), gerenciador de modelos com download falso (SHA/tamanho/
cancelamento), cliente de nuvem com HTTP falso, configurações e o controlador. UI: seção `[5g]` de `tools/ui/ui_test.js`.
