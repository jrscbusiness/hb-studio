<!-- Use este modelo para publicar ou atualizar um plugin no Marketplace do HB Studio.
     Para abrir um PR com ele: acrescente ?template=marketplace_entry.md à URL de criação do PR.
     Guia completo: docs/MARKETPLACE.md -->

## Plugin

- **id**: `com.exemplo.meuplugin`
- **Nome / versão**:
- **Tipo**: tema / fonte de vídeo / efeito de áudio / gráfico / template
- **Novo plugin ou atualização?**:

## Checklist do autor

- [ ] Alterei **somente** `marketplace/index.json` (e, se for o caso, `marketplace/packages/`).
- [ ] O `id` é igual ao `id` do `manifest.json` dentro do pacote e **não mudou** entre versões.
- [ ] Rodei `python3 marketplace/validate_index.py` e passou.
- [ ] `sha256` e `size` são do arquivo `.hbplugin` **exato** que está no link de download (https).
- [ ] O link de download é **permanente** (release com tag no GitHub, não um branch que muda).
- [ ] Informei `permissions` com honestidade (`native-code` é obrigatório para fonte/efeito).
- [ ] Informei a licença e o link do **código-fonte** (`repository`). Plugins nativos sem código-fonte público não serão aceitos.
- [ ] **Não** coloquei `"verified": true` (isso é decisão exclusiva dos mantenedores).
- [ ] Entendo que plugins nativos rodam com os mesmos poderes do HB Studio e que o catálogo não é um sandbox.

## Descrição para os revisores

O que o plugin faz, que arquivos/rede/dispositivos ele acessa, como foi compilado (compilador, comando) e como reproduzir o binário.

## Código nativo (preencher se tipo = fonte de vídeo ou efeito de áudio)

- Link do commit/tag exato que gerou o `.dll`:
- Dependências de terceiros (e licenças):
- Acessa rede? Quais endereços?
- Grava arquivos fora da pasta do plugin?
