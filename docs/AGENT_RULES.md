# Regras para agentes e colaboradores (leia antes de mexer no código)

Este arquivo existe para que briefings de agentes sejam curtos: o prompt aponta para cá em vez de repetir tudo.
Código e comentários em inglês; interface e documentação em pt-BR.

## Economia de tokens (sem perder qualidade)
- **Navegue pelo grafo antes de ler arquivos**: `graphify explain "Símbolo"`, `graphify path "A" "B"`,
  `graphify query "..." --seed Símbolo --budget 800`. Se `graphify-out/graph.json` não existir (ele é gerado
  localmente e não vai para o Git; cada worktree começa sem ele): `graphify update .` (~10 s, sem custo de API).
  Depois de mudar código: `graphify update .`.
- **Nunca leia inteiros** `ui/index.html` (~330 KB) nem `graphify-out/GRAPH_REPORT.md` (~90 KB). Use `Grep` para achar a
  linha e `Read` com `offset`/`limit`. Depois da divisão da UI, edite `ui/src/` (arquivos pequenos) e rode
  `python3 -I tools/build_ui.py`.
- **Saída de comandos sempre filtrada**: `| tail -5`, `| grep -E "error|FAIL"`. Prefira os scripts abaixo.
- Não releia o que acabou de escrever; não cole arquivos grandes na conversa; relatórios finais curtos.

## Scripts prontos (uma linha, saída mínima)
- `tools/check_all.sh` — compila, roda ctest e o teste de UI; imprime só o resumo e sai com erro se algo falhar.
- `tools/ci_status.sh [N]` — últimas N execuções da CI em uma linha cada; `tools/ci_status.sh --wait` espera a execução do
  commit atual terminar e imprime só o resultado (e os testes/erros que falharam).
- `tools/merge_branch.sh <branch>` — faz o merge; conflitos de blocos `FEATURE:` aditivos são resolvidos mantendo os dois
  lados e o script imprime o que merece revisão manual e o balanço de chaves/parênteses. Não faz commit.

## Convenções
- Módulo novo: `src/<modulo>/`, `tests/<modulo>_*.cpp`, `cmake/modules/<modulo>.cmake`, `docs/<MODULO>.md`.
- Edições em arquivos compartilhados (`CMakeLists.txt`, `app_main.cpp`, UI): mínimas, aditivas, entre marcadores
  `FEATURE: nome` — nunca reformatar nem reordenar código existente.
- Contratos entre módulos só via `hb::core::EventBus` (tópicos: `caption.text`, `recording.fileClosed`,
  `chat.message`, `overlay.chatPin`).
- Segredos nunca em log nem enviados à UI (só dica mascarada), criptografados em repouso (DPAPI).
- Honestidade: o que não foi validado em hardware/serviço real é documentado como **não validado**.

## Lições da CI do Windows (já quebraram antes)
- Código Windows só é verificado pela CI (15–19 min). Revise chaves/parênteses, includes e namespaces antes do push.
- Testes: sem caminhos fixos como `/tmp` (use `::testing::TempDir()`); feche `ifstream` antes de `remove()`;
  não assuma que `sleep_for(<15 ms)` é preciso (o Windows dorme ~15 ms); `std::function` não aceita lambda só-movível.
- Nunca use `secrets.X` em `if:` de step: o workflow inteiro é rejeitado.

## Ao terminar
- Rode `tools/check_all.sh`. Commits pequenos e descritivos. Agentes **não** fazem push nem merge: quem coordena integra.
- Depois de subir para o GitHub, avisar o usuário com: **PRONTO PODE TESTAR** (regra do `CLAUDE.md`).
