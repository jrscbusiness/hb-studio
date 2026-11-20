# Plugins do HB Studio

O HB Studio pode ser estendido por pacotes **`.hbplugin`** (um arquivo zip). Há três tipos:

| Tipo | O que faz | Executa código? |
|---|---|---|
| `theme` | Troca as cores da interface (variáveis CSS) | **Não** |
| `source` | Adiciona novas entradas de vídeo (placas de captura, protocolos, SDKs de câmera) | **Sim** — biblioteca nativa (C ABI) |
| `effect` | Adiciona efeitos de áudio ao mixer (aba ÁUDIO) | **Sim** — biblioteca nativa (C ABI) |

Esta página documenta o formato do pacote, a ABI em C que uma biblioteca nativa precisa
exportar, e o modelo de segurança. Para construir e assinar um pacote use a ferramenta
`hb_pack` (veja [`plugins/examples/`](../plugins/examples/) para dois exemplos completos:
`colorbars` é um plugin de fonte, `theme-midnight` é um tema).

## O pacote (`manifest.json`)

Todo `.hbplugin` é um zip com um `manifest.json` na raiz:

```json
{
  "schema": 1,
  "id": "com.exemplo.minhacamera",
  "name": "Minha Câmera XYZ",
  "version": "1.0.0",
  "author": "Seu Nome",
  "description": "Uma frase, em pt-BR, explicando o que o plugin faz.",
  "license": "MIT",
  "type": "source",
  "apiVersion": 1,
  "library": { "windows-x64": "minhacamera.dll" },
  "sha256":  { "minhacamera.dll": "<64 hex, preenchido pelo hb_pack>" }
}
```

Campos:

- **`id`**: identificador estável, `[A-Za-z0-9._-]`, recomenda-se notação reversa de domínio
  (`com.empresa.produto`). Nunca muda entre versões — é a chave usada para habilitar/remover o
  plugin e para lembrar em qual CAM/canal ele estava.
- **`version`**: semver (`MAJOR.MINOR.PATCH`).
- **`type`**: `"source"`, `"effect"` ou `"theme"`.
- **`apiVersion`**: `1` (a versão da ABI em C, ver abaixo). Só para `source`/`effect`.
- **`library`**: para `source`/`effect`, mapa de plataforma → nome do arquivo da biblioteca
  dentro do pacote. Hoje o HB Studio só é distribuído para Windows, então na prática é só
  `"windows-x64": "arquivo.dll"`. (Se um dia o app rodar em outra plataforma, acrescente a
  chave correspondente — mas só inclua no pacote a biblioteca que você realmente compilou e vai
  distribuir: o manifesto exige um sha256 para **cada** arquivo listado em `library`.)
- **`sha256`**: mapa nome do arquivo → hash SHA-256 em hex minúsculo. **Obrigatório** para todo
  arquivo executável de um plugin `source`/`effect` (é a verificação de integridade). O `hb_pack`
  calcula e preenche isso para você — nunca edite esse campo manualmente.
- **`theme`** (só `type: "theme"`): `{"colorScheme": "light"|"dark", "vars": {"--bg": "#RRGGBB", ...}}`.
  Só as variáveis da lista abaixo são aceitas, e só com valores de cor (`#rgb`, `#rrggbb`,
  `#rrggbbaa` ou `rgb()`/`rgba()`); qualquer outra coisa é recusada. Um tema **não pode conter
  nenhum arquivo executável** — só `manifest.json` (e, opcionalmente, nada mais).

Variáveis de tema aceitas: `--bg --panel --surface --surface-2 --glass --line --line-2 --ink
--ink-2 --muted --blue --blue-2 --sky --mist --navy --on-accent --warn --warn-ink --warn-bg
--warn-line`.

### Construindo um pacote

```
hb_pack <pasta-com-manifest.json-e-a-dll> <saida.hbplugin>
```

A ferramenta: lê `manifest.json` da pasta, calcula o sha256 de cada arquivo (se `type` for
`source`/`effect`), grava um `manifest.json` já com o campo `sha256` preenchido dentro do zip, e
por fim **valida o próprio pacote que acabou de gerar** com o mesmo código que o aplicativo usa
(o mesmo `inspectPackage()` que roda ao instalar) — se algo estiver errado, `hb_pack` falha em vez
de produzir um pacote inválido.

### Limites do pacote (zip)

Para que abrir um `.hbplugin` nunca seja perigoso por si só: no máximo 64 MB de pacote, no
máximo 200 entradas, no máximo 128 MB de conteúdo descomprimido no total, nenhum caminho pode
sair da pasta do plugin (`..`, caminho absoluto) nem usar um nome reservado do Windows (`CON`,
`NUL`, `COM1`...), nenhum arquivo duplicado só por causa de maiúsculas/minúsculas, e todo arquivo
que *parece* executável (`.dll`, `.exe`, `.so`, `.dylib`, ...) precisa estar listado em `sha256`.

## A ABI em C (plugins `source` e `effect`)

A biblioteca nativa (`.dll` no Windows) é simples C, sem tipos de C++ cruzando a fronteira, para
funcionar com qualquer compilador. A definição completa e comentada está em
[`src/plugins/hb_plugin_api.h`](../src/plugins/hb_plugin_api.h); o resumo:

### Fonte de vídeo (`hb_plugin_get_v1`)

```c
HB_PLUGIN_EXPORT int hb_plugin_get_v1(hb_plugin_v1* out);
```

O host zera `out`, define `struct_size`/`api_version`, chama essa função uma vez ao carregar a
biblioteca; o plugin preenche os ponteiros de função e retorna `0`. As funções mais importantes:

- `list_sources(desc*, max)`: quais fontes de vídeo este plugin oferece (ex.: duas entradas de
  uma placa com duas saídas). Até 32.
- `open_source(id)` / `close_source(handle)`: abre/fecha uma fonte pelo seu `id` estável.
- `render(handle, bgra, w, h, stride, index)`: escreve o quadro mais recente, **já escalado**
  para `w`×`h`, em BGRA8. **É chamada no caminho de tempo real** (a cada quadro do Program ou do
  Preview) — precisa retornar em poucos milissegundos. Faça o trabalho lento (rede, decodificação)
  numa thread própria do plugin e deixe `render()` apenas copiar/escalar o quadro mais novo já
  pronto. Um plugin cujo `render()` for lento ou falhar repetidamente é desligado sozinho (ver
  "Proteções" abaixo).

### Efeito de áudio (`hb_plugin_get_effects_v1`)

```c
HB_PLUGIN_EXPORT int hb_plugin_get_effects_v1(hb_effects_v1* out);
```

Um plugin pode oferecer vários **tipos** de efeito (até 64), cada um com até 32 parâmetros.
`describe(index, desc, params, max)` descreve o tipo (nome, parâmetros com `min`/`max`/padrão);
`create(index, sampleRate, channels)` cria uma instância; `process(instance, interleaved, frames)`
processa o áudio **no próprio buffer**, em tempo real (sem alocação, sem bloqueio, sem E/S); cada
instância só é usada por uma thread por vez. `set_param` pode ser chamado de qualquer thread
enquanto `process` roda — precisa ser seguro para tempo real (atômico, sem lock).

## Proteções contra plugins quebrados ou maliciosos

Um plugin nativo roda **dentro do processo do HB Studio**, com os mesmos poderes dele — não há
isolamento de memória entre o aplicativo e o plugin (isso exigiria um processo separado, que não
existe nesta versão). Por isso:

- **Todo plugin novo, e toda atualização, começa desativado.** O operador precisa ativá-lo
  explicitamente depois de ver o que o pacote contém (tela de revisão antes de instalar).
- **Integridade**: o sha256 de cada biblioteca é conferido antes de **cada** carregamento (não só
  na instalação) — um arquivo trocado no disco depois de instalado é detectado e recusado.
- **Chamadas guardadas**: toda chamada para dentro do plugin é protegida contra exceções C++ e,
  no Windows, também contra falhas de acesso à memória (SEH) — uma violação não derruba o HB
  Studio inteiro, só desativa aquela fonte/efeito.
- **Desligamento automático**: uma fonte cujo `render()` fica lento (> 100 ms) 20 vezes seguidas,
  ou que falha 30 vezes seguidas, é desligada. Um efeito que devolve um bloco de áudio inválido
  (NaN/Inf, tamanho errado) 8 vezes seguidas é colocado em bypass até a próxima preparação.
- **Bibliotecas nunca são descarregadas em tempo de execução** (só ao sair do programa, ou após
  reiniciar quando um plugin é removido/desativado) — evita o problema clássico de uma DLL que
  ainda tem threads rodando ser descarregada embaixo delas.
- **Temas nunca executam código**: só trocam variáveis CSS, e são validados campo a campo (lista
  branca de variáveis, só valores de cor).

**Em resumo: instale apenas plugins de autores em quem você confia.** A verificação de
integridade garante que o arquivo não foi adulterado depois de instalado — ela não garante que o
código do plugin seja seguro ou bem-comportado.

## Onde ficam os plugins instalados

`%LOCALAPPDATA%\HBStudio\plugins\<id>\` — cada plugin em sua própria pasta, mais um
`state.json` na raiz (quais estão ativados, qual tema está ativo). A pasta é por usuário do
Windows; cada conta do sistema tem sua própria lista.
