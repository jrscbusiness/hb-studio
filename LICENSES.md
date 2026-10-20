# Licenças (rascunho — Fase 1)

Licença do HB Studio: **pendente**. Candidatas compatíveis com Qt/FFmpeg LGPL e libsrt MPL:
Apache-2.0 ou MPL-2.0 (decisão do mantenedor; não é aconselhamento jurídico).

| Dependência | Uso | Licença | Notas |
|---|---|---|---|
| GoogleTest 1.14 | testes | BSD-3 | só em desenvolvimento |
| Qt 6 (futuro) | UI | LGPLv3 | linkagem dinâmica |
| FFmpeg (futuro) | mídia | LGPL 2.1+ (build sem GPL/nonfree) | sem x264/x265 |
| libsrt (futuro) | SRT | MPL 2.0 | |
| NDI SDK (futuro) | NDI | Proprietária | **não** vai no repositório; usuário instala o Runtime; atribuição de marca obrigatória |
| OBS Studio | referência | GPL-2.0 | **nenhum código copiado** |
| microsoft/Windows-Camera (VirtualCamera sample) | referência de estrutura do media source | MIT | Código reescrito; estrutura de objetos inspirada no exemplo. Aviso MIT: Copyright (c) Microsoft Corporation |
| pugixml 1.14 (zeux/pugixml) | parser XML do módulo ONVIF/Discovery (FetchContent, tag v1.14) | MIT | Copyright (c) 2006-2023 Arseny Kapoulkine |
