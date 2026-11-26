# Diagnóstico e checklist antes de entrar ao vivo

Dois recursos no cartão **Diagnóstico e checklist** da aba CONFIG. Código portátil em `src/diagnostics/`
(testado em `tests/diagnostics_*.cpp`); só as sondas ficam em `src/diagnostics/windows/sys_win.cpp`.

## 1. Exportar diagnóstico
Botão **EXPORTAR DIAGNÓSTICO** → janela nativa "Salvar como" → um único `.txt` para anexar ao relato do problema.

Conteúdo: versão do app (com hash do commit), Windows, CPU/RAM/GPU, monitores, dispositivos de vídeo e áudio com estado,
recursos ligados, configurações sem segredos, destinos de transmissão (só `esquema://servidor`, chave apenas
"(definida)/(vazia)"), qualidade de preview e formato de saída efetivos, último erro por módulo (câmeras, transmissão,
nuvem, chat…) e as últimas 300 linhas do registro em memória.

### Privacidade (defesa em profundidade, `diagnostics/redactor.*`)
1. Chaves de transmissão salvas são removidas por valor exato (também codificadas em URL/JSON).
2. `hb::core::redactSecrets` linha a linha (senhas, tokens, `Authorization`, chaves RTMP, usuário:senha em URL).
3. Caminhos: `%LOCALAPPDATA%`, `%USERPROFILE%`; qualquer `C:\Users\<nome>`, `/home/<nome>` vira `<user>`.
4. E-mails, IPv4/IPv6/MAC viram `<email>`/`<ip>`/`<mac>`; tokens opacos de 32+ caracteres viram `<token>`.

O relatório inteiro passa pelo redator como último passo. Os testes usam entradas hostis e provam que os segredos não aparecem.
Limite: o redator não conhece segredos que o app não lhe informa (hoje: chaves dos destinos); os demais dependem das regras
genéricas acima. Revise o arquivo antes de publicá-lo.

### Segurança do caminho
O caminho do arquivo vem **somente** da janela nativa; a mensagem `diagexport` da página não carrega caminho algum.

## 2. Checklist antes de entrar ao vivo
Botão **RODAR CHECKLIST** (roda em thread própria; a UI não trava). Cada item mostra ✔ OK, ⚠ atenção, ✖ problema ou
– não verificado, com uma dica de correção de uma linha.

| Item | Regra |
|---|---|
| Disco | espaço livre da pasta de gravação e minutos estimados pelo bitrate atual (vídeo+áudio; é estimativa). ≥120 min ✔, 30–120 ⚠, <30 min ou abaixo do limite mínimo do gravador ✖ |
| Internet | conexão HTTPS ao host de teste de conectividade do Windows (`www.msftconnecttest.com`) |
| Upload | só com teste ligado: ~512 KB (máx. 10 s) para o endereço HTTPS **configurado por você**; comparado à soma de vídeo+áudio kbps dos destinos ligados (≥1,5× ✔, ≥1× ⚠, abaixo ✖). **Desligado por padrão**; sem ele o item fica "não verificado" |
| Destinos | cada destino ligado precisa de servidor e chave (a chave nunca é mostrada) |
| Câmeras | online x atribuídas |
| Áudio | dispositivo presente e sinal acima de -60 dBFS (pico recente) nos canais atribuídos |
| Codificador | H.264 por hardware (✔), só software (⚠), nenhum (✖), enumeração falhou (não verificado) via Media Foundation |
| Relógio | data plausível; diferença para o cabeçalho `Date` do servidor: ≤120 s ✔, ≤600 s ⚠, mais ✖ |

Configuração do teste de upload: `%LOCALAPPDATA%\HBStudio\checklist.json` (apenas ligado/desligado e a URL, sem segredos;
URL só `https://`, sem usuário/senha).

## Não validado
Sondas Windows (WinHTTP, DXGI, Media Foundation, janela Salvar) só foram checadas por sintaxe (mingw) e pela CI;
**não validadas em hardware real**. O teste de upload é uma estimativa curta e varia.
