# Licenças (rascunho — Fase 1)

Licença do HB Studio: **pendente**. Candidatas compatíveis com Qt/FFmpeg LGPL e libsrt MPL:
Apache-2.0 ou MPL-2.0 (decisão do mantenedor; não é aconselhamento jurídico).

| Dependência | Uso | Licença | Notas |
|---|---|---|---|
| GoogleTest 1.14 | testes | BSD-3 | só em desenvolvimento |
| Qt 6 (futuro) | UI | LGPLv3 | linkagem dinâmica |
| FFmpeg (futuro) | mídia | LGPL 2.1+ (build sem GPL/nonfree) | sem x264/x265 |
| libsrt (futuro) | SRT | MPL 2.0 | |
| NDI® SDK / Runtime | descoberta e recepção de câmeras NDI na rede (`src/ndi/`) | Proprietária (NDI SDK License) | **Não vai no repositório** nem é vinculado estaticamente: `src/ndi/` só declara a interface pública estável do NDI (estruturas/assinaturas, reconstruídas a partir da documentação pública do NDI em docs.ndi.video, nenhum código deles copiado) e carrega dinamicamente (`LoadLibraryW`, por símbolo via `GetProcAddress`, sem usar o header oficial) o NDI Runtime que o próprio usuário instala separadamente (download gratuito em ndi.video/tools), procurado via `NDI_RUNTIME_DIR_V6`/`NDI_RUNTIME_DIR_V5` (variáveis que o instalador da NDI define) e, em último caso, nas pastas padrão de instalação. Sem o Runtime instalado (ou fora do Windows), a detecção/recepção de câmeras NDI fica indisponível com um aviso claro — o resto do HB Studio funciona normalmente. O layout dos structs/símbolos foi conferido contra bindings open-source gerados por `bindgen`/`libloading` a partir do SDK real (ex. `ndi-sys`, `gst-plugin-ndi`), usados apenas como referência de ABI (nomes, ordem de campos, valores numéricos), nunca como código copiado. "NDI" é marca registrada da Vizrt NDI AB; aviso de marca obrigatório na tela correspondente |
| VST SDK 3.8.1 (steinbergmedia/vst3sdk, tag `v3.8.1_build_84`) | hospedagem de plugins de efeito VST3 com interface gráfica própria (`src/vst3/`) | MIT (desde a versão 3.8 do SDK; as licenças GPLv3 e "proprietária Steinberg" anteriores não estão mais disponíveis) | Copyright (c) 2026 Steinberg Media Technologies GmbH. Só os submódulos `base`, `cmake`, `pluginterfaces` e `public.sdk` são buscados (FetchContent; não `vstgui4`/`doc`/`tutorials`, que um host não precisa); nenhum código do SDK é modificado. "VST" é marca registrada da Steinberg Media Technologies GmbH — aviso de marca ("VST is a trademark of Steinberg Media Technologies GmbH") incluído onde o nome/logo aparecer na interface |
| OBS Studio | referência | GPL-2.0 | **nenhum código copiado** |
| microsoft/Windows-Camera (VirtualCamera sample) | referência de estrutura do media source | MIT | Código reescrito; estrutura de objetos inspirada no exemplo. Aviso MIT: Copyright (c) Microsoft Corporation |
| pugixml 1.14 (zeux/pugixml) | parser XML do módulo ONVIF/Discovery (FetchContent, tag v1.14) | MIT | Copyright (c) 2006-2023 Arseny Kapoulkine |
| MD5 (src/rtsp/md5.cpp) | Digest auth do RTSP | Implementação própria a partir do RFC 1321 | Nenhum código de terceiros copiado; testada com os vetores do RFC. Sem dependência externa nova no módulo RTSP |
| miniaudio 0.11.22 (mackron/miniaudio) | captura/reprodução/loopback de áudio do módulo `audio_io` (FetchContent, tag 0.11.22; só a implementação de dispositivos, sem decoders/engine) | MIT-0 / domínio público (escolha) | Copyright (c) David Reid. Compilado em um único .cpp com MA_NO_ENCODING/DECODING/GENERATION/RESOURCE_MANAGER/NODE_GRAPH/ENGINE |
| miniz 3.0.2 (richgel999/miniz) | leitura/escrita dos pacotes `.hbplugin` (zip) em `src/plugins/package.cpp` e na ferramenta `hb_pack` (FetchContent, tag 3.0.2) | MIT | Copyright (c) 2010-2014 Rich Geldreich, 2013-2014 Tenacious Software LLC |
