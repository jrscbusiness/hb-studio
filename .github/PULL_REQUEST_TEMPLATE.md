## O que muda e por quê

<!-- Resumo curto. Link para a issue, se houver (Closes #123). -->

## Checklist

- [ ] **Testes**: adicionei/atualizei testes (lógica pura testável sem rede/hardware) e `ctest` passa localmente.
- [ ] **Documentação**: atualizei o `docs/*.md` do módulo (pt-BR) e, se mudou o que está validado, o "Estado de validação".
- [ ] **Convenções** ([docs/CONTRIBUTING.md](../docs/CONTRIBUTING.md)): código/comentários em inglês, compila em GCC e MSVC, sem `ssize_t`/`unistd.h` fora de `#ifdef`.
- [ ] **Sem segredos**: nenhuma chave, token, senha ou dado pessoal no código, nos testes, nos logs ou nas capturas de tela.
- [ ] **Dependências novas** (se houver): versão fixada via FetchContent, licença compatível, listada em `LICENSES.md`.
- [ ] **Honestidade**: não afirmei compatibilidade com equipamento que não foi testado.

## Windows / CI

<!-- O CI do Windows (windows-2022) é a única verificação de MSVC. Se você não tem Windows, diga
     aqui: "não testado no Windows; dependo do CI". -->

- [ ] Testado em Windows real (descreva a versão e o equipamento), ou
- [ ] Não testado no Windows; dependo do CI.

> Plugin para o Marketplace? Use o modelo próprio: acrescente `?template=marketplace_entry.md` à URL de criação do PR.
