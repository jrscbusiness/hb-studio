# Economia de tokens no Claude Code (sem perder eficácia)

O que realmente reduz o consumo, o que é só propaganda e como ligar cada item. Dados medidos neste projeto
estão marcados como **medido**; o resto vem de fontes externas e deve ser tratado com cautela.

## 1. O que já está ativo neste repositório
| Item | Efeito | Evidência |
|---|---|---|
| `graphify` (grafo de código local, sem LLM) | consultar um símbolo custa centenas de bytes em vez de ler o arquivo | **medido**: `explain` ≈ 0,3 KB; `grep` ≈ 2,6 KB; ler `dshow_capture.cpp` inteiro ≈ 25 KB |
| Não ler `ui/index.html` (330 KB) nem `GRAPH_REPORT.md` (90 KB) inteiros | evita ≈ 80 mil tokens por leitura acidental | **medido** (tamanhos) |
| `docs/AGENT_RULES.md` | briefings de agentes curtos: o prompt aponta para o arquivo em vez de repetir ~600 palavras | tamanho do prompt |
| `tools/check_all.sh`, `tools/ci_status.sh`, `tools/merge_branch.sh` | um comando, saída de 3–5 linhas em vez de logs inteiros | uso nesta sessão |
| `CLAUDE.md` curto | ele é carregado a cada turno; detalhes ficam em `docs/` e são lidos sob demanda | — |

## 2. O que você pode ligar (todos os seus projetos)
1. **Desconectar conectores que não usa** (Gmail, Calendar, Drive, Docs...) em *claude.ai → Configurações → Conectores*.
   Cada servidor MCP acrescenta nomes de ferramentas e instruções a toda sessão. Veja o custo real com `/context`.
2. **Compactar mais cedo**: `export CLAUDE_AUTOCOMPACT_PCT_OVERRIDE=50` (compacta a conversa aos 50 % do contexto).
3. **RTK** ([rtk-ai/rtk](https://github.com/rtk-ai/rtk), Apache-2.0): proxy que filtra/resume a saída de comandos de shell
   (`git`, `ls`, testes, builds...) antes de ela entrar no contexto. Telemetria **desligada por padrão**; o instalador
   confere o SHA-256 do binário. Limites honestos: só intercepta a ferramenta **Bash** (não `Read`/`Grep`/`Glob`), e o
   "até 90 %" refere-se à saída dos comandos, não à sua conta total. Instalação na sua máquina:
   `brew install rtk` (macOS/Linux), `winget install rtk-ai.rtk` (Windows) e depois `rtk init -g`;
   `rtk gain` mostra quanto economizou. **Não foi instalado automaticamente aqui**: o hook altera todos os comandos
   Bash, então o Claude Code pediu a sua decisão.
4. **Busca semântica de código** (claude-context, grepai, semble...): em avaliações independentes a economia ficou em
   ≈ 27–40 % de tokens/chamadas com a mesma precisão do grep; os números de 90 %+ vêm dos próprios autores e comparam
   com grep ingênuo. Exigem serviço de embeddings/banco vetorial (custo e privacidade). O `graphify` já cobre a navegação
   estrutural; só vale somar uma busca semântica se você sentir falta de perguntas em linguagem natural.

## 3. O que NÃO fazer
- Reduzir o nível do modelo ou o esforço de raciocínio só para gastar menos: piora a qualidade e muitas vezes aumenta
  retrabalho (e tokens).
- Instalar várias camadas de compressão ao mesmo tempo: uma ferramenta que reescreve a saída do Bash por cima de outra
  dificulta depurar o que o agente realmente viu.

## Fontes
- [RTK (Rust Token Killer)](https://github.com/rtk-ai/rtk)
- [9 Ways to Cut Token Consumption in Claude Code](https://composio.dev/blog/ways-to-cut-token-consumption-in-claude-code)
- [Claude Context — avaliação quantitativa](https://zc277584121.github.io/ai-coding/2025/09/12/claude-context-evaluation-and-casestudy.html)
- [grepai vs grep no Claude Code](https://yoanbernabeu.github.io/grepai/blog/benchmark-grepai-vs-grep-claude-code)
- [Custo de tokens dos servidores MCP](https://www.mindstudio.ai/blog/claude-code-mcp-server-token-overhead)
