# Relatório de testes (ciclo atual)

Legenda: **[SIM]** simulado/fakes · **[REAL-CI]** executado de verdade no runner Windows do GitHub (Windows Server 2022, NÃO é Windows 11) · **[NÃO EXEC.]**.

| Verificação | Resultado | Tipo |
|---|---|---|
| Build GCC 13 (`-Wall -Wextra -Wpedantic`) | 0 avisos | real (Linux) |
| `ctest` Linux (núcleo + ONVIF/Discovery + PTZ + RTSP + áudio) | 395/395 passam | [SIM] fakes + loopback TCP/UDP real |
| `ctest` Windows (MSVC 2022) | 395/395 passam | [REAL-CI] |
| ASan+UBSan, suíte completa | 395/395 limpos | real (Linux) |
| TSan, núcleo (80 testes) | sem avisos | real (Linux) |
| Fuzz do `redactSecrets` (200 mil entradas, ASan/UBSan) | sem falhas | real (Linux) |
| `hb_vcam_selftest`: DLL da câmera virtual ativada como o Frame Server faz, quadros do Program Output via pipe, pixels NV12 conferidos | 22/22 | [REAL-CI] (in-process; não é o Frame Server real) |
| `HBStudio.exe --screenshot`: app real inicia, motor roda, UI renderiza, fecha com exit 0 | OK; 30,2 fps; captura inspecionada | [REAL-CI] |
| `HBStudio-Setup.exe --screenshot`: instalador renderiza | OK; captura inspecionada | [REAL-CI] |
| Instalação real (copiar, registrar câmera virtual, atalhos, desinstalar) | **[NÃO EXEC.]** | requer Windows 11 + administrador |
| Câmera virtual enxergada pelo App Câmera / OBS / navegador | **[NÃO EXEC.]** | requer Windows 11 real |
| Qualquer câmera real (FOMAKO NK20A, ONVIF, RTSP, PTZ, NDI) | **[NÃO EXEC.]** | sem hardware |
| Múltiplos monitores, gravação, streaming, NDI | **[NÃO IMPLEMENTADO]** | roadmap |

Medições: render de 1080p de uma fonte de teste = 0,84 ms/quadro (Linux, -O2). Frame rate do app no runner de CI (2 vCPU): 16,7 fps antes da otimização → 30,2 fps depois (mesmo tipo de runner, execuções diferentes).
