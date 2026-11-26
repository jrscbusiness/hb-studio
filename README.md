# HB STUDIO

[English version / versão em inglês: README.en.md](README.en.md)

Central de produção e transmissão de vídeo ao vivo — mesa de corte (switcher), até 16 câmeras,
mixer de áudio, gravação, câmera virtual, legendas, gráficos e transmissão simultânea para várias
plataformas. Windows 11 primeiro, macOS depois. **Código aberto (MIT), gratuito, e aberto a
contribuições da comunidade.**

Autor: **Rafael Campos**. Licença: [MIT](LICENSE).

> **Estado do projeto: pré-1.0, validado em testes automatizados — quase nada foi validado em
> hardware real ainda.** Veja a tabela [Status de validação](#status-de-validação). Se você tem um
> PC com Windows 11 e câmeras de verdade, testar e relatar o que funcionou (ou não) é a melhor
> contribuição possível hoje.

## Por que este projeto existe

Soluções profissionais de transmissão costumam ser caras, fechadas ou exigir assinatura. O HB
Studio existe para que qualquer igreja, produtora, escola ou criador de conteúdo tenha uma central
de produção completa, gratuita, instalável com dois cliques — sem depender de licença paga.

## Baixar

**[Baixe o instalador na página de Releases](https://github.com/jrscbusiness/hb-studio/releases/tag/latest)**
→ `HBStudio-Setup.exe`, dois cliques, sem terminal. A versão (com o código do commit) aparece no
rodapé do aplicativo.

> O instalador ainda não tem certificado de assinatura de código (ver [BUILD.md](BUILD.md) —
> "Assinatura de código"), então o Windows pode mostrar um aviso de segurança na primeira
> execução. Isso é esperado para qualquer programa não assinado, não é um problema do instalador:
> clique em *Mais informações → Executar assim mesmo* (ou, se o "Controle inteligente de
> aplicativos" do Windows 11 bloquear por completo, veja a mesma seção do BUILD.md).

## Primeiros passos (para quem vai operar)

1. Instale com `HBStudio-Setup.exe` e abra o **HB Studio**.
2. Em **CÂMERAS**, ligue as entradas que vai usar (webcam, placa de captura, câmera virtual ou fonte
   NDI) e escolha resolução/fps. Sem câmera, as entradas "TESTE" mostram um padrão de teste — dá para
   conhecer a interface sem nenhum equipamento.
3. Na **PRODUÇÃO**, selecione uma entrada no **PREVIEW** e use **CUT** (ou `ESPAÇO`), **DISSOLVE**
   (`ENTER`) ou a barra T-bar para levá-la ao **PROGRAM**. Teclas `1–8` escolhem o preview.
4. Em **ÁUDIO**, ajuste ganho/mudo/solo dos canais. Em **GRAVAÇÃO**, escolha o formato e grave.
5. Para transmitir, abra **TRANSMISSÃO**, escolha um preset (YouTube, Twitch...) ou "Personalizado",
   cole a **chave de transmissão da sua própria conta** (como no OBS Studio — não há login/OAuth) e
   inicie. Detalhes e limitações em [docs/TRANSMISSAO.md](docs/TRANSMISSAO.md).
6. Algo deu errado? Em **CONFIG → Registro (logs)** use **COPIAR REGISTRO** e cole o texto numa
   [issue](https://github.com/jrscbusiness/hb-studio/issues/new/choose).

## Funcionalidades

| Área | O que tem |
|---|---|
| **Produção** | Preview/Program, CUT/DISSOLVE com durações configuráveis, fader manual (T-bar), até 16 entradas de câmera em 2 fileiras de 8, menu de contexto por câmera, tela cheia do Program, atalhos de teclado |
| **Câmeras** | Webcams e placas de captura (DirectShow), câmeras virtuais (OBS etc.), câmeras e fontes **NDI** na rede, fontes de vídeo vindas de plugins; ligar/desligar e escolher resolução/fps por câmera |
| **PTZ** | Controle por câmera com presets nomeáveis; protocolos ONVIF, VISCA (IP/serial via conversor) e Pelco-D implementados e testados com transporte simulado. **A conexão com equipamento PTZ real ainda não está ligada na interface** (ver "O que ainda não está pronto") |
| **Cenas e templates** | Compositor de cenas (várias câmeras/fontes num layout, com fundo, bordas, sombra) que entra na mesa como se fosse uma câmera; templates prontos para **culto, aula, podcast e evento corporativo**, editor de cena e "salvar como meu template" ([docs/TEMPLATES.md](docs/TEMPLATES.md)) |
| **Gráficos (overlays)** | Cronômetro/contagem regressiva, placar, QR Code (doação/PIX/link), nome (lower third), legendas ao vivo e mensagem de chat destacada, desenhados sobre o Program ([docs/OVERLAYS.md](docs/OVERLAYS.md)) |
| **Áudio** | Mixer por canal (ganho, pan, mudo/solo, fone), efeitos (compressor, filtro passa-alta etc.) e hospedagem de plugins **VST3** com a janela nativa do plugin |
| **Gravação** | MP4 (H.264/AAC, via Media Foundation) e formatos portáteis (AVI/Motion-JPEG, WAV, FLAC); **gravação multipista (ISO)**: um arquivo por câmera (limpa, sem overlays) e por canal de áudio, junto com a gravação normal, com manifesto de sessão (`session.json`) e listas EDL para editar depois ([docs/GRAVACAO_MULTIPISTA.md](docs/GRAVACAO_MULTIPISTA.md)) |
| **Transmissão** | Vários destinos simultâneos via **RTMP**, cada um com a sua chave; presets com o endereço já preenchido para YouTube e Twitch (Facebook, Kick, Vimeo, LinkedIn e X: você informa o endereço) e servidor personalizado; preferência de codificador (automático, x264, NVENC, Quick Sync, AMF) e ajuste de baixa latência. RTMPS (TLS) e SRT ainda não ([docs/TRANSMISSAO.md](docs/TRANSMISSAO.md)) |
| **Câmera virtual** | Expõe o Program como câmera virtual do Windows 11 (Frame Server) — aparece no Zoom, Teams, OBS, navegador etc. |
| **Saídas** | Janelas de PROGRAM/PREVIEW para segundo/terceiro monitor (com tela cheia), saída **NDI** na rede local, resolução de saída configurável (720p a 4K) |
| **Legendas ao vivo** | Reconhecimento de fala **local** (whisper.cpp, modelo baixado sob demanda) com opção de motor em nuvem (API compatível com OpenAI), exibidas no Program e gravadas em `.srt` ([docs/LEGENDAS.md](docs/LEGENDAS.md)) |
| **Chroma key** | Chroma verde/azul clássico por câmera e **remoção de fundo por IA** local (ONNX Runtime — recurso opcional, ver [docs/CHROMA.md](docs/CHROMA.md)) |
| **Teleprompter** | Roteiros com janela de apresentador (segundo monitor/tablet), velocidade, espelhamento, anotações do operador ([docs/TELEPROMPTER.md](docs/TELEPROMPTER.md)) |
| **Controle remoto** | Celular/tablet na mesma rede Wi-Fi como segunda tela: corte, áudio, gravação, destinos e teleprompter, com PIN de pareamento ([docs/CONTROLE_REMOTO.md](docs/CONTROLE_REMOTO.md)) |
| **Chat unificado** | YouTube, Facebook e Twitch numa lista só, com destaque da mensagem no ar; Instagram é **limitado/indisponível** por restrição da Meta ([docs/CHAT.md](docs/CHAT.md)) |
| **IA (opcional)** | Você usa a sua própria chave: OpenRouter (inclui modelos gratuitos), 9router local ou qualquer provedor compatível com OpenAI; sem provedor configurado, nada é enviado a lugar nenhum ([docs/IA.md](docs/IA.md)) |
| **Backup em nuvem** | Envio das gravações em segundo plano para S3-compatível (AWS, Backblaze, Cloudflare R2, Wasabi, MinIO) e Google Drive, com fila persistente, limite de banda e pausa durante a transmissão ([docs/BACKUP_NUVEM.md](docs/BACKUP_NUVEM.md)) |
| **Plugins e Marketplace** | Pacotes `.hbplugin` (temas, fontes de vídeo, efeitos de áudio, gráficos, templates) com revisão de permissões e aviso de risco; catálogo comunitário em **PLUGINS → MARKETPLACE** ([docs/MARKETPLACE.md](docs/MARKETPLACE.md)) |
| **Interface** | 5 temas (claro, escuro, cinza, OBS e sistema), ajuste da qualidade do preview (para PCs modestos), painel de logs com cópia em um clique, versão com código do commit no rodapé |

## Status de validação

O projeto separa com honestidade o que **o computador do autor e o CI provaram** do que **só um
equipamento real prova**. "Teste automatizado" aqui significa a suíte do repositório (mais de 1.000
casos de teste GoogleTest + testes de interface num navegador real), rodando no Linux e no runner
Windows do GitHub (Windows Server 2022, **não** é um Windows 11 de verdade, e não tem câmera nem GPU
dedicada).

| Área | Teste automatizado (Linux / CI Windows) | Validado em hardware real |
|---|---|---|
| Núcleo, mesa de corte, motor de vídeo | Sim | Parcial: o app abre e renderiza no runner de CI; uso ao vivo não |
| Câmeras DirectShow / placas de captura | Só enumeração (CI não tem câmera) | Não |
| Câmera virtual (Frame Server) | Autoteste da DLL no CI (carregada no próprio processo) | Não (não vista no App Câmera/OBS/Zoom reais) |
| NDI (entrada e saída) | Sim, com backend simulado | Não (nunca falou com o NDI Runtime/fonte real) |
| PTZ (ONVIF/VISCA/Pelco) | Sim, com transporte simulado | Não; e ainda não ligado à interface |
| Áudio, efeitos, mixer, VST3 | Sim | Não (nenhum plugin VST3 comercial, interface de áudio ou fone testados) |
| Gravação MP4 / multipista ISO | Sim, com escritores simulados e formatos portáteis | Não |
| Transmissão RTMP (protocolo) | Sim, contra servidor RTMP de teste em loopback | Não (nenhuma plataforma real; encoders de GPU não testados) |
| Legendas (whisper.cpp), chroma por IA | Lógica portátil sim | Não |
| Chat, backup em nuvem, IA | Sim, com transporte simulado | Não (nenhuma chamada a serviço real) |
| Controle remoto | Sim (sockets reais em 127.0.0.1, navegador real) | Não (nenhum celular/roteador/firewall real) |
| Instalador | Renderiza no CI | Não (instalação completa em PC real não executada) |
| Plugins / Marketplace | Sim (validação do catálogo no CI) | Não |

Detalhes por módulo estão nos `docs/*.md` (procure "Estado de validação"). Roteiro do que falta em
[docs/ROADMAP.md](docs/ROADMAP.md). Defeitos conhecidos em [docs/BUGS.md](docs/BUGS.md).

## O que ainda não está pronto (sendo honesto)

- **Conexão real com câmeras PTZ (ONVIF/VISCA/Pelco)**: os protocolos estão implementados e testados
  isoladamente, mas as câmeras de rede ainda não aparecem como fontes reais na aba CÂMERAS.
- **RTMPS (TLS) e SRT**: ainda não implementados; hoje a transmissão é RTMP puro (o que os presets de
  plataforma já aceitam).
- **Instagram (chat e transmissão)**: sem suporte útil por restrição da plataforma (ver docs/CHAT.md).
- **WebDAV/Nextcloud** no backup em nuvem: não implementado.
- **macOS**: ainda não iniciado.
- Tudo que os `docs/` marcam como **"não validado em hardware real"** — ou seja, quase tudo que
  depende de equipamento (ver a tabela acima).

## Compilar o código

Veja [BUILD.md](BUILD.md) (inclui opções como `HB_CAPTIONS_WHISPER`, `HB_ORT_DIR` e ccache).

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Smoke test sem janela: `build/hb_probe 3 1080p30`.

## Arquitetura e documentação

- [ARCHITECTURE.md](ARCHITECTURE.md) — decisões de arquitetura aprovadas.
- `docs/*.md` — um documento por módulo (áudio, PTZ, NDI, transmissão, gravação, plugins, ONVIF,
  RTSP, câmera virtual, VST3, legendas, chat...), em português.
- [docs/ROADMAP.md](docs/ROADMAP.md) — o que está validado, o que está pendente e as próximas ideias.

## Contribuir

Toda contribuição é bem-vinda: código, testes, documentação, tradução, plugins para o Marketplace,
ou só testar numa máquina Windows 11 de verdade e relatar o que funcionou ou não.

- Leia [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md) antes de abrir um Pull Request: estrutura de
  módulos e regras (compila em GCC e MSVC, lógica pura testável sem rede/hardware, segredos nunca em
  log, honestidade sobre o que foi validado).
- Quer publicar um plugin? Siga o guia [docs/MARKETPLACE.md](docs/MARKETPLACE.md).
- Bug, ideia ou dúvida: abra uma [issue](https://github.com/jrscbusiness/hb-studio/issues/new/choose)
  (há modelos para bug, funcionalidade e pergunta).
- Vulnerabilidade de segurança: **não** abra issue pública, veja [SECURITY.md](SECURITY.md).
- Conduta: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## Licença

[MIT](LICENSE) — copyright © 2026 Rafael Campos. Dependências de terceiros e suas licenças estão
listadas em [LICENSES.md](LICENSES.md).
