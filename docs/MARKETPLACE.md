# Marketplace de plugins

O Marketplace deixa a comunidade publicar extensões (temas, fontes de vídeo, efeitos de áudio, gráficos e
templates) e deixa o operador encontrá-las e instalá-las dentro do próprio HB Studio, em
**PLUGINS → MARKETPLACE**.

Não existe servidor para ninguém manter: o catálogo é **um arquivo estático**, [`marketplace/index.json`](../marketplace/index.json),
guardado neste repositório público. Publicar = abrir um pull request que acrescenta uma entrada nele.

## Em uma frase: o que protege o operador (e o que não protege)

**Existe:** (1) o catálogo fixa o **SHA-256 e o tamanho** do pacote, e o app recusa qualquer arquivo diferente;
(2) o download só é feito por **HTTPS com verificação de certificado**, com teto de tamanho e de tempo;
(3) o arquivo baixado passa pela **mesma tela de revisão e consentimento** de qualquer plugin instalado
à mão: para código nativo o operador precisa marcar "entendo o risco" e o plugin continua **desativado**
até ele ativar; (4) o app confere que o pacote baixado é **o que o catálogo anunciava** (mesmo `id`, tipo e
versão); (5) revisão humana dos mantenedores antes de qualquer entrada entrar no catálogo.

**Não existe:** **sandbox**. Um plugin nativo (`.dll`) roda dentro do processo do HB Studio com os mesmos poderes dele
(ver [PLUGINS.md](PLUGINS.md)). Hash e consentimento garantem *qual arquivo* você instala, não que o código seja
bom. Também **não há** avaliações/estrelas nem pagamentos nesta versão.

## Como funciona (operador)

1. **PLUGINS → MARKETPLACE.** O app baixa o catálogo (com cache por ETag; sem internet, mostra a última cópia boa).
2. Busque por nome, autor ou etiqueta, filtre por categoria. Os cartões mostram autor, versão, tamanho,
   **VERIFICADO / NÃO VERIFICADO**, **CÓDIGO NATIVO** e se já está **instalado** ou tem **atualização**.
3. Clique no cartão para ver detalhes (permissões, licença, site, código-fonte, capturas de tela).
4. **INSTALAR** (ou **ATUALIZAR**) baixa o pacote, confere o SHA-256 e abre a **tela de revisão**. Plugins de
   um publicador não verificado mostram um aviso em destaque. Só depois do seu "INSTALAR" na revisão algo é gravado.
5. **REMOVER** usa o mesmo caminho da aba INSTALADOS.
6. Templates (categoria `template`) são só dados (`hb-templates` v1, veja [TEMPLATES.md](TEMPLATES.md)): depois de
   conferido o hash, são importados direto, sem executar código.

O endereço do catálogo é configurável em **CONFIG → Marketplace de plugins** (qualquer URL `https://`). Use apenas catálogos
de quem você confia: o catálogo decide o que é listado e o que é marcado como verificado.

## Esquema do `index.json` (schemaVersion 1)

Arquivo único, JSON, até 2 MiB e 500 entradas. Veja o [`index.schema.json`](../marketplace/index.schema.json) (para o editor)
e o exemplo em [`marketplace/index.json`](../marketplace/index.json).

```json
{
  "schemaVersion": 1,
  "plugins": [{
    "id": "com.exemplo.meuplugin",
    "name": "Meu Plugin",
    "author": "Seu Nome",
    "description": {"pt-BR": "O que faz.", "en": "What it does."},
    "category": "video-source",
    "type": "source",
    "tags": ["captura"],
    "license": "MIT",
    "homepage": "https://exemplo.com",
    "repository": "https://github.com/voce/meuplugin",
    "screenshots": ["https://exemplo.com/print1.png"],
    "permissions": ["native-code", "hardware-capture"],
    "versions": [{
      "version": "1.2.0",
      "minAppVersion": "0.3.0",
      "platforms": ["windows-x64"],
      "downloadUrls": ["https://github.com/voce/meuplugin/releases/download/v1.2.0/meuplugin.hbplugin"],
      "sha256": "<64 hex minúsculos>",
      "size": 123456,
      "releasedAt": "2026-11-24",
      "notes": "O que mudou."
    }]
  }]
}
```

Regras (as mesmas no app, em `src/marketplace/catalog.cpp`, e no validador de CI):

- `id`: minúsculas/dígitos/`.`/`-`/`_`, 3 a 64 caracteres, igual ao `id` do `manifest.json` do pacote, único no catálogo, nunca muda.
- `category`: `theme`, `video-source`, `audio-effect`, `graphic` ou `template` (tema, fonte de vídeo, efeito de áudio, gráfico, template).
- `type` (o que o pacote é): `theme`/`source`/`effect` = pacote `.hbplugin` (o `type` do manifesto); `template` = arquivo JSON `hb-templates`.
  `theme`→`theme`, `video-source`→`source`, `audio-effect`→`effect`, `template`↔`template`; `graphic` aceita `theme`, `source` ou `effect`
  (hoje não existe um tipo de pacote próprio para gráficos).
- `permissions` (obrigatório, pode ser `[]`): `native-code` **tem que** constar exatamente quando `type` é `source` ou `effect`
  (modelo de risco do app: tema/template não executam código; o resto é código nativo). Outros valores informativos: `network`, `filesystem`, `hardware-capture`.
  O que vale de verdade é a tela de revisão, que lê o manifesto do pacote.
- `versions`: 1 a 50. `version` semver; `minAppVersion` (padrão `0.0.0`); `platforms` (hoje `windows-x64`);
  `downloadUrls` 1 a 4 URLs **https** (espelhos tentados em ordem; sem usuário/senha, sem endereços IP, sem `localhost`);
  `sha256` do **arquivo `.hbplugin` inteiro** (64 hex minúsculos); `size` em bytes (1 a 64 MiB; templates até 1 MiB).
- O app oferece a **maior versão estável compatível** (ignora pré-lançamentos como `1.0.0-beta`, versões que exigem app mais novo ou outra plataforma).
  "Atualização disponível" = essa versão é maior que a instalada (semver).
- `verified`: **só os mantenedores** colocam `true`. Omitido = `false`.
- `screenshots`: só URLs https; o app só as carrega quando você abre os detalhes.
- Campos desconhecidos são ignorados (compatibilidade futura). Uma entrada inválida é **descartada** (as outras continuam); um `schemaVersion`
  maior que o suportado faz o app pedir para você atualizá-lo.

## Para desenvolvedores: publicar um plugin passo a passo

1. **Escreva e teste** o plugin conforme [PLUGINS.md](PLUGINS.md) (manifesto + ABI em C para fonte/efeito; só cores para tema).
2. **Empacote** com a ferramenta do projeto, que também valida o pacote com o mesmo código do app:

   ```
   hb_pack <pasta-com-manifest.json-e-a-dll> meuplugin-1.2.0.hbplugin
   ```
3. **Publique o arquivo em um link permanente https.** O mais simples: uma *Release* no GitHub com tag (`v1.2.0`); o link
   `https://github.com/<voce>/<repo>/releases/download/v1.2.0/meuplugin-1.2.0.hbplugin` não muda. Não use um branch (`main`)
   que muda: o hash deixaria de conferir.
4. **Calcule o SHA-256 e o tamanho do arquivo exato** que está nesse link:

   ```powershell
   Get-FileHash -Algorithm SHA256 .\meuplugin-1.2.0.hbplugin      # copie o Hash em MINÚSCULAS
   (Get-Item .\meuplugin-1.2.0.hbplugin).Length                    # size
   ```
   ```bash
   sha256sum meuplugin-1.2.0.hbplugin && stat -c %s meuplugin-1.2.0.hbplugin
   ```
5. **Faça um fork**, edite `marketplace/index.json` e acrescente a sua entrada (ou uma nova `versions[]` na sua entrada existente).
   Não mexa nas entradas dos outros. Deixe `verified` de fora.
6. **Valide localmente** (Python 3, sem dependências):

   ```
   python3 marketplace/validate_index.py
   ```
   Ele confere o esquema e, para cada link, baixa o arquivo e compara SHA-256, tamanho e o `id`/`type`/`version` do manifesto.
   Link ainda inacessível gera só um aviso (`--strict-network` transforma em erro; `--no-network` não baixa nada).
7. **Abra o pull request** usando o modelo [`marketplace_entry.md`](../.github/PULL_REQUEST_TEMPLATE/marketplace_entry.md)
   (acrescente `?template=marketplace_entry.md` à URL de criação do PR). O workflow `marketplace-validate` roda o mesmo validador.
8. Responda às perguntas dos revisores. Depois do merge, a entrada aparece no app (os usuários recebem o catálogo novo no próximo "Atualizar lista").

**Atualizar** um plugin = nova versão em `versions[]` (nunca altere hash/URL de uma versão já publicada: quem já instalou perderia a verificação).
**Retirar** = remover a entrada (ou a versão) por PR; quem já instalou continua com o plugin, que pode ser removido na aba INSTALADOS.

### Template como extensão (categoria `template`)

Exporte o template na tela TEMPLATES (formato `hb-templates` v1). Publique o `.json` como acima, com `"category": "template"`, `"type": "template"`,
`"permissions": []`. O app confere o hash e importa; o importador valida o conteúdo de novo (limite de 1 MB).

## Para mantenedores: checklist de revisão de um PR do Marketplace

**Todos os tipos**
- [ ] CI verde (`marketplace-validate`); o PR altera só `marketplace/` e não toca `verified` de entradas existentes.
- [ ] `id` não colide nem imita um plugin existente ou o nome de outro autor; nome e descrição honestos.
- [ ] Link de download permanente (tag/release), https, domínio do autor ou GitHub; `repository` aponta para o código.
- [ ] Licença declarada e compatível com redistribuição; sem conteúdo ilegal ou enganoso.
- [ ] `permissions` coerentes com o que o código faz.

**Temas e templates** (sem código): abrir o `manifest.json`/`.json`; só variáveis de cor / dados conhecidos; nada além disso no zip.

**Plugins nativos (fonte de vídeo / efeito de áudio): revisão de segurança**
- [ ] Código-fonte público do **commit/tag exato**; o `.dll` foi **reproduzido** a partir dele (mesmo hash, ou pelo menos o mesmo comportamento) por um mantenedor.
- [ ] Leitura do código: rede (para onde? por quê?), arquivos fora da pasta do plugin, processos filhos, `LoadLibrary`/injeção, registro do Windows, persistência, ofuscação, binários embutidos.
- [ ] Dependências de terceiros listadas, com versões e licenças; sem binários pré-compilados sem origem.
- [ ] O `render()`/`process()` respeita as regras de tempo real (sem bloquear, sem alocar no caminho quente).
- [ ] Teste manual no app: instala, ativa, remove, reinicia; o desligamento automático do app funciona se o plugin travar.
- [ ] Atualizações: **diff** do código entre a versão anterior e a nova (uma versão nova pode ser o ataque).

### Por que "verificado" é raro

`verified: true` significa "um mantenedor leu o código e reproduziu o binário". É caro, não escala, e **não é garantia**
(não há sandbox; revisão humana erra). Por isso: a maioria das entradas entra como **NÃO VERIFICADO**, claramente rotulada e com aviso
na tela de instalação; temas e templates são o caso fácil e mais comum de ficarem verificados; plugins nativos só são marcados quando
houver código aberto, build reproduzível e um revisor disposto a assinar embaixo, e **perdem** a marca a cada versão nova até serem revistos de novo.
O catálogo marca "verificado", mas quem controla o repositório controla essa marca: por isso a branch `main` deve exigir revisão obrigatória
e os mantenedores devem ser poucos e de confiança.

## Limitações conhecidas desta versão

- `downloader_win.cpp` (WinHTTP) foi escrito sem acesso a um compilador/ambiente Windows: **não validado**; a CI do Windows é a verificação real. A lógica (catálogo, semver, cache, hash, fluxo de instalação) é portátil e testada em Linux com transporte falso.
- Sem assinatura criptográfica de autor: a confiança vem do **repositório do catálogo** (HTTPS + revisão de PR) e do hash fixado.
  Quem consegue alterar `index.json` consegue trocar hash e link (por isso a proteção de branch).
- Só `windows-x64`. Sem avaliações, sem pagamentos, sem atualização automática (o app só avisa; você clica em ATUALIZAR).
- Capturas de tela e links de site são carregados de servidores de terceiros (que veem seu IP) apenas quando você abre os detalhes.
- O rótulo "verificado" e o tipo declarado no catálogo são informações do catálogo; a tela de revisão lê o **pacote** (autoridade) e o app recusa pacote que não confere com o catálogo.
