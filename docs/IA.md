# Inteligência Artificial (provedores e chaves de API)

O HB Studio pode usar a IA da sua escolha. Você cola **a sua própria chave** em
**CONFIG > Inteligência Artificial**; todas as ferramentas de IA do app usam o provedor *ativo*.
É totalmente opcional: sem provedor configurado, nada é enviado a lugar nenhum.

Código: `src/ai/` (biblioteca `hb_ai`), testes `tests/ai_*.cpp`.

## Provedores suportados

Todos falam o protocolo "OpenAI-compatível" (`POST {base}/chat/completions`, `GET {base}/models`).

| Tipo | Endereço base (editável) | Chave |
|---|---|---|
| OpenRouter | `https://openrouter.ai/api/v1` | obrigatória |
| 9router (local) | `http://localhost:20128/v1` | normalmente não exigida (qualquer texto serve) |
| Personalizado | você digita (OpenAI, Groq, Together, Ollama `http://localhost:11434/v1`, LM Studio `http://localhost:1234/v1`...) | depende do serviço |

Você pode salvar até 16 provedores, escolher um como **ativo** e definir um modelo padrão para cada.

## Como obter uma chave GRÁTIS no OpenRouter

1. Crie uma conta em <https://openrouter.ai> e abra *Keys* (<https://openrouter.ai/keys>).
2. Crie uma chave (começa com `sk-or-`) e copie.
3. No HB Studio: CONFIG > Inteligência Artificial, tipo **OpenRouter**, cole a chave, clique **BUSCAR MODELOS**,
   marque "Mostrar só modelos grátis" e escolha um modelo com ID terminado em `:free`. **SALVAR**, depois **TESTAR CONEXÃO**.

Modelos grátis são identificados por ID terminado em `:free` ou preço 0 de entrada e saída na lista do OpenRouter
(o selo "grátis" no app segue essa regra). Limites reais (definidos pelo OpenRouter, podem mudar; confira a
documentação deles): modelos `:free` têm limite de requisições por minuto e por dia, e o limite diário
costuma ser bem maior depois de comprar uma pequena quantia de créditos. O app mostra "Limite de requisições
atingido" quando o provedor responde 429 e tenta de novo automaticamente (até 2 vezes, com espera crescente).
Modelos grátis podem sumir ou ficar instáveis a qualquer momento.

## Como rodar o 9router

O 9router é um roteador/proxy local de código aberto (<https://github.com/decolua/9router>) que expõe várias
IAs (inclusive camadas gratuitas de alguns serviços) em um único endereço OpenAI-compatível.

1. Instale o Node.js e rode `npm install -g 9router`, depois `9router`.
2. O painel abre em `http://localhost:20128`; lá você conecta os provedores que quiser.
3. No HB Studio: tipo **9router**, endereço `http://localhost:20128/v1` (padrão), **BUSCAR MODELOS** lista os
   modelos e "combos" configurados no painel. Cole a chave do painel se você ativou `REQUIRE_API_KEY`; senão deixe em branco.

Conforme a documentação pública do projeto (consultada ao escrever isto, **não testamos o 9router aqui**):
porta padrão 20128, chave só exigida se `REQUIRE_API_KEY` estiver ligado, e quais camadas grátis existem muda
com o tempo. Os termos de uso de cada serviço por trás do 9router são responsabilidade sua.

## Segurança e privacidade

- **Chave criptografada**: salva em `%LOCALAPPDATA%\HBStudio\ai.json` protegida com DPAPI (`CryptProtectData`,
  vinculada ao seu usuário do Windows, com entropia própria do app). Outro usuário do Windows (ou outro PC) não
  consegue ler; o app avisa e pede a chave de novo. Não protege contra malware rodando como você.
  Em sistemas sem DPAPI (builds de desenvolvimento) a chave fica só na memória e não é gravada.
- **A UI nunca recebe a chave de volta**: apenas "chave salva" e os 4 últimos caracteres (`••••7f2a`; chaves curtas
  não revelam nada). Deixar o campo em branco ao editar mantém a chave salva.
- A chave não vai para logs (mensagens de erro passam por `redactSecrets`).
- **HTTPS obrigatório** para servidores remotos, com verificação de certificado (padrão do WinHTTP, nunca desligada).
  `http://` só é aceito para `localhost`/`127.x`/`::1` (9router, Ollama, LM Studio). Endereços com usuário/senha são rejeitados.
- **O que sai do computador**: somente quando você usa uma ferramenta de IA (ou clica TESTAR CONEXÃO / BUSCAR MODELOS):
  o texto do pedido (ex.: o assunto digitado no Assistente) e a chave, para o endereço configurado. Nada de vídeo,
  áudio, lista de câmeras ou outros dados do app. O teste com modelo definido envia a palavra "ping" (1 token, pode
  consumir créditos mínimos em modelos pagos). O provedor que você escolheu (e, no 9router, os serviços por trás
  dele) recebe esse texto sob as políticas de privacidade dele; modelos grátis muitas vezes podem usar os dados
  para treino. Não digite informações sensíveis.

## Assistente (uso real da chave)

No mesmo card: escreva o assunto da live e clique **SUGERIR** para receber título e descrição. Você pode copiar
os dois; nada é publicado automaticamente. A IA pode errar: revise antes de usar.

## Para desenvolvedores: contrato `hb::ai::AiService`

```cpp
#include "ai/ai_service.hpp"
if (ai.configured()) {
    auto r = ai.complete("system prompt", "user prompt");   // bloqueante; opts: temperatura, maxTokens, timeout, onDelta (streaming SSE)
    if (r.ok) use(r.text); else toast(r.error);             // r.error já está em português
}
ai.completeAsync(sys, user, opts, [](const hb::ai::ChatResult& r) { /* thread de trabalho */ });
```

- Erros classificados (`ChatResult::kind`): chave inválida (401/403), sem crédito (402), modelo indisponível
  (404/400 com "model"), limite (429), rede, timeout, servidor (5xx), resposta inesperada.
- Retry/backoff em 429/408/5xx/rede: 500 ms, 1 s, 2 s... teto 8 s, respeita `Retry-After` (até 8 s). Streaming não
  é repetido depois de entregar o primeiro trecho. Relógio e `sleep` são injetáveis nos testes.
- Comandos UI->host: `aiget`, `aisave|json`, `aiactivate|id`, `airemove|id`, `aitest|json`, `aimodels|json`,
  `aisuggest|json`. Host->UI: `ai_state`, `ai_models`, `ai_test`, `ai_suggest`.
- O destrutor do `AiService` aguarda requisições em andamento (no máximo o timeout, 30 s por tentativa); fechar o app
  no meio de uma chamada pode demorar até isso.

## O que NÃO foi validado

- O transporte WinHTTP (`http_win.cpp`) e o DPAPI (`secret_store_win.cpp`) foram escritos sem compilador Windows
  nem execução real: dependem do CI/teste no Windows 11.
- Nenhuma chamada real a OpenRouter, 9router, OpenAI, Ollama etc. foi feita; o cliente foi testado com respostas
  simuladas baseadas na documentação pública. Formatos de erro e de `/models` de cada serviço podem diferir.
- O teste de UI (Playwright) valida a tela com mensagens simuladas, não o fluxo real com o host.
- Não há cancelamento de requisição em andamento e nem proxy com autenticação própria (usa o proxy do sistema).
