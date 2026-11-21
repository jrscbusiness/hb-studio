# HB STUDIO

Central de produção e transmissão de vídeo ao vivo — switcher, múltiplas câmeras, mixer de áudio,
gravação, câmera virtual e transmissão simultânea para várias plataformas (YouTube, Twitch,
Facebook, Kick, Vimeo, LinkedIn, X e qualquer servidor RTMP/RTMPS próprio). Windows 11 primeiro,
macOS depois. **Código aberto, gratuito, e aberto a contribuições da comunidade.**

Autor: **Rafael Campos**. Licença: [MIT](LICENSE).

## Por que este projeto existe

Soluções profissionais de transmissão costumam ser caras, fechadas ou exigir assinatura. O HB
Studio existe para que qualquer igreja, produtora, escola ou criador de conteúdo tenha uma central
de produção completa, gratuita, instalável com dois cliques — sem depender de licença paga.

## Baixar

**[Baixe o instalador na página de Releases](https://github.com/jrscbusiness/hb-studio/releases/tag/latest)**
→ `HBStudio-Setup.exe`, dois cliques, sem terminal.

> O instalador ainda não tem certificado de assinatura de código (ver [BUILD.md](BUILD.md) —
> "Assinatura de código"), então o Windows pode mostrar um aviso de segurança na primeira
> execução. Isso é esperado para qualquer programa não assinado, não é um problema do instalador:
> clique em *Mais informações → Executar assim mesmo* (ou, se o "Controle inteligente de
> aplicativos" do Windows 11 bloquear por completo, veja a mesma seção do BUILD.md).

## Funcionalidades

| Área | O que tem |
|---|---|
| **Produção** | Preview/Program, CUT/DISSOLVE com durações configuráveis, fader manual (T-bar), 16 entradas de câmera em 2 fileiras de 8, menu de contexto por câmera (ativar/desligar/configurar), tela cheia do Program |
| **Câmeras** | Webcams/placas de captura (DirectShow), câmeras virtuais (OBS etc.), câmeras NDI na rede, plugins de fonte de vídeo; ligar/desligar e escolher resolução/fps por câmera |
| **PTZ** | Controle por câmera com 10–64 presets nomeáveis; protocolos ONVIF/VISCA/Pelco modelados (conexão real com equipamento físico ainda não ligada à interface — ver "O que ainda não está pronto") |
| **Áudio** | Mixer por canal (ganho, pan, mudo/solo, fone), efeitos (compressor, filtro passa-alta) e hospedagem de plugins **VST3** com a interface gráfica nativa do próprio plugin |
| **Gravação** | MP4 (H.264/AAC, via Media Foundation) e formatos portáteis (AVI/Motion-JPEG, WAV, FLAC) |
| **Câmera virtual** | Expõe o Program como câmera virtual do Windows (Frame Server) — aparece no Zoom, Teams, OBS, navegador etc. |
| **Saídas** | Janelas de PROGRAM/PREVIEW para um segundo/terceiro monitor (com tela cheia), saída NDI na rede local, resolução de saída configurável (720p a 4K) |
| **Transmissão (TRANSMISSÃO)** | Transmita ao vivo e simultaneamente para vários destinos RTMP/RTMPS — presets para YouTube e Twitch, ou qualquer servidor próprio; sem login/OAuth, só a chave de transmissão da sua própria conta, exatamente como no OBS Studio (detalhes e limitações em [docs/TRANSMISSAO.md](docs/TRANSMISSAO.md)) |
| **Plugins** | Pacotes `.hbplugin` (temas, fontes de vídeo, efeitos de áudio) com instalação, revisão de permissões e aviso de risco antes de ativar código de terceiros |
| **Interface** | Tema claro/escuro/sistema nativo, visual minimalista azul/branco, pensado para operação ao vivo sob pressão |

## O que ainda não está pronto (sendo honesto)

- **Conexão real com câmeras PTZ por rede (ONVIF/VISCA/Pelco)**: os protocolos estão implementados
  e testados isoladamente, mas ainda não aparecem como fontes reais na aba CÂMERAS.
- **Codificação H.264/AAC da transmissão ao vivo**: implementada via Windows Media Foundation, mas
  ainda sem validação em hardware real (câmeras de verdade, GPU NVENC/Quick Sync/AMD).
- **RTMPS (TLS) e SRT**: ainda não implementados na transmissão; hoje é RTMP puro (o que todo
  preset de plataforma já aceita).
- **macOS**: ainda não iniciado.
- Qualquer coisa marcada **"não validado em hardware real"** nos documentos de `docs/` — a suíte de
  testes automatizados é extensa, mas só um Windows 11 de verdade, com câmeras e hardware reais,
  confirma 100%.

## Baixar e compilar o código

Veja [BUILD.md](BUILD.md). Smoke test sem janela: `build/hb_probe 3 1080p30`.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # 700+ testes automatizados
```

## Arquitetura e documentação

- [ARCHITECTURE.md](ARCHITECTURE.md) — decisões de arquitetura aprovadas.
- `docs/*.md` — um documento por módulo (áudio, PTZ, NDI, transmissão, gravação, plugins, ONVIF,
  RTSP, câmera virtual, VST3...), em português.
- [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) — convenções de código, estrutura de módulos e regras
  antes de abrir um Pull Request.

## Como contribuir

Toda contribuição é bem-vinda: código, testes, documentação, tradução, ou só testar numa máquina
Windows 11 de verdade e relatar o que funcionou ou não (isso já ajuda muito, veja "O que ainda não
está pronto" acima). Leia [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) antes de abrir um Pull
Request — ele explica a estrutura de módulos e as regras do projeto (compila em GCC e MSVC, lógica
pura testável sem rede/hardware, segredos nunca em log, honestidade sobre o que foi validado).

Dúvidas, ideias ou problemas: abra uma [issue](https://github.com/jrscbusiness/hb-studio/issues).

## Licença

[MIT](LICENSE) — copyright © 2026 Rafael Campos. Dependências de terceiros e suas licenças estão
listadas em [LICENSES.md](LICENSES.md).
