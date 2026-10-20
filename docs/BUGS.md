# Registro de defeitos (estado real)

Severidade: P0 crítico · P1 alto · P2 moderado · P3 baixo. Fonte principal: `docs/REVIEW-1.md` (auditoria independente nº 1).

| ID | Sev. | Defeito | Estado |
|---|---|---|---|
| B-01 | P1 | App não abria em Windows sem `MFCreateVirtualCamera` (import estático) | **Corrigido** (carga dinâmica) — verificado no CI (exit 0) |
| B-02 | P1 | Crash ao iniciar: `CoIncrementMTAUsage(nullptr)` | **Corrigido** — verificado no CI |
| B-03 | P1 | `redactSecrets` quadrático/estouro de pilha e vazamento de formatos (REVIEW A3/A4) | **Corrigido** — reescrito linear; 20 testes de regressão + fuzz ASan/UBSan (200 mil entradas) |
| B-04 | P1 | `removeConsumer` não esperava callbacks em voo — use-after-free (A5) | **Corrigido** — remoção síncrona; testes de regressão; TSan limpo |
| B-05 | P2 | Falha do renderizador deixava consumidores sem quadros (M6) | **Corrigido** — reenvia último quadro / preto |
| B-06 | P1 | Pipe da câmera virtual: ACL ampla, sem FIRST_PIPE_INSTANCE, sem timeout (A1/A2) | **Corrigido no código**; **não validado em Windows 11 real** |
| B-07 | P1 | Entrega NV12 sem validar layout; token perdido em falha (A6/M1) | **Corrigido no código**; não validado em Windows 11 real |
| B-08 | P2 | Corridas no ciclo de vida do hub/stream da câmera virtual (M2/M3) | **Parcialmente corrigido** (hub singleton). Ciclo stream↔fonte e `ShutdownObject` vazio: **ABERTO** |
| B-09 | P2 | Instalador: ferramentas por nome, nome de conta localizado, sem retry (M7) | **Corrigido no código** (caminho absoluto, SID, retry); instalador não executado em PC real |
| B-10 | P2 | Host WebView2 sem trava de navegação / sem try-catch / feeder duplicado | **Corrigido no código** |
| B-11 | P3 | Itens BAIXO da auditoria (mixFrames, downscale 8K, switcher vazio, UTF-8 em nomes…) | **Corrigidos** com testes |
| B-12 | P2 | Teste de áudio com leitura fora dos limites (achado pelo MSVC) | **Corrigido** — suíte passa sob ASan |
| B-13 | P2 | Seção de entradas mostrava 8 fontes sem alerta acima da capacidade | **Corrigido** — aviso de desempenho |
| B-14 | P2 | Fontes de teste lentas (16,7 fps no runner de CI) | **Corrigido** — 30,2 fps no mesmo tipo de runner; 0,84 ms/quadro medido em 1080p |
| B-15 | P2 | ONVIF/RTSP/PTZ/áudio **ainda não ligados ao aplicativo** (módulos prontos e testados isoladamente) | **ABERTO** — próxima prioridade |
| B-16 | P3 | `docs/REVIEW-1.md` M4 (log em pasta compartilhada) e demais MÉDIOS restantes | ABERTO/parcial — ver o relatório |
