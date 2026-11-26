# Atualização automática

CONFIG > **Atualizações**. Código: `src/updater/` (parser, comparação e download verificado, portáveis e testados em
`tests/updater_*`) e o trecho `FEATURE: updates` do `app_main.cpp` (diálogo nativo e execução do instalador, só Windows,
**não validado em máquina real**).

## Como funciona
1. O app compara o hash do seu commit (`0.3.0+<commit7>`) com o commit alvo da release `latest` do GitHub
   (`GET https://api.github.com/repos/jrscbusiness/hb-studio/releases/tags/latest`, anônimo). O endereço pode ser trocado
   em "Avançado" (somente https e somente `api.github.com`, para forks). Builds sem hash (`0.3.0` puro) ficam como "não verificado".
2. Verificação sozinha: no máximo a cada **6 h** (falha tenta de novo após 1 h), desligável. O botão VERIFICAR AGORA ignora
   o intervalo, mas tem trava de 20 s. HTTP 403/429 (limite de 60 consultas/h sem login) vira mensagem amigável.
3. "Há uma versão nova": mostra um trecho das notas, o tamanho e os botões **BAIXAR E INSTALAR** / **DEPOIS** (DEPOIS só
   esconde o ponto discreto ao lado de CONFIG até sair outra versão).
4. BAIXAR E INSTALAR: baixa `HBStudio-Setup.exe` com o downloader endurecido do Marketplace (https, sem rebaixar para http,
   teto = tamanho declarado na release, aplicado durante o recebimento) para `%LOCALAPPDATA%\HBStudio\updates\downloads\`
   (caminho definido pelo app, nunca pela página). Confere tamanho exato, cabeçalho `MZ` e **SHA-256**, que vem do campo
   `digest` da API e/ou do arquivo `HBStudio-Setup.exe.sha256` publicado pela CI (se os dois existirem e divergirem, recusa;
   **se nenhum existir, recusa**). Recusa também com transmissão ou gravação em andamento.
5. Só então o **host** (não a página) mostra uma caixa de diálogo nativa do Windows (padrão = Não). Se confirmar, o arquivo é
   re-verificado, o instalador é aberto (UAC) e o HB Studio fecha normalmente; o instalador pede o clique em INSTALAR.
   Se o UAC for recusado, o app continua aberto.

## O que isto protege — e o que NÃO protege (leia)
O instalador **ainda não tem assinatura digital** (Authenticode; veja BUILD.md). O SHA-256 vem **da mesma release** que o
instalador. Portanto ele protege contra **arquivo corrompido ou incompleto**, mas **não** contra uma release
comprometida (quem publicasse um instalador malicioso publicaria também o hash dele) nem contra a conta do GitHub
invadida. Só uma assinatura de código verificada pelo app (ou um hash fixado fora da release) mudaria isso; fica como
trabalho futuro. O mesmo vale para o download manual. Por isso a confirmação nativa avisa isso a cada atualização.

Também: https obrigatório, URLs de download só em `github.com`, JSON hostil tratado (limites de tamanho/profundidade,
UTF-8 validado, notas sanitizadas), nenhum caminho ou comando vindo da mensagem da página.

## Limitações
* Releases publicadas antes deste recurso não têm `.sha256` (mas podem ter `digest`); sem hash, a atualização automática é recusada.
* A comparação é por igualdade de commit: um build local à frente da `main` também aparecerá como "nova versão".
* Não validado em Windows real (UAC, diálogo, fechamento limpo, instalação por cima).
