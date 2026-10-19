# HB Studio — Prova de conceito da Câmera Virtual (Windows 11)

**Objetivo:** provar que o Program Output do HB Studio aparece como uma webcam no Windows 11 e é
recebido pelo OBS. **Isto é uma prova de conceito**, não o produto final.

## Como funciona
```
hb_vcam_poc.exe (usuário normal)          Serviço "Windows Camera Frame Server"
  ProgramOutput (barras 1080p30)            carrega hb_vcam_source.dll
        │  named pipe  \\.\pipe\HBStudioVirtualCamera   │
        └───────────────────────────────────────────────▶ NV12 ─▶ OBS / Câmera / Chrome
```
- A câmera só é "ligada" (a DLL só é carregada) quando **algum aplicativo abre a câmera**.
- Sem sinal do app, a câmera mostra uma imagem cinza com uma **barra branca em movimento** ("sem sinal").
  Isso já prova que a câmera virtual funciona, mesmo sem o app enviando quadros.
- Com o app enviando, aparecem as **barras de cor coloridas com um marcador branco andando** embaixo.
- O Windows acrescenta "Windows Virtual Camera" ao nome: a câmera aparece como **"HB Studio Windows Virtual Camera"**.

## Requisitos
Windows 11 (build 22000 ou superior), OBS Studio instalado, permissão de câmera para apps de
desktop (Configurações → Privacidade e segurança → Câmera).

## Passo a passo
1. Baixe o artefato **hb-studio-vcam-poc** da última execução verde do CI
   (GitHub → aba *Actions* → última execução → *Artifacts*), extraia o zip em qualquer pasta.
2. **Instalar (uma vez):** botão direito no PowerShell → *Executar como administrador*:
   ```powershell
   Set-ExecutionPolicy -Scope Process Bypass
   cd "C:\caminho\da\pasta\extraida"
   .\install.ps1
   ```
   Deve terminar com "Installed:" e "Registered:". Isso copia a DLL para `C:\Program Files\HB Studio\VirtualCamera`.
3. **Rodar o app de teste** num terminal **normal (sem administrador)**:
   ```
   hb_vcam_poc.exe
   ```
   Deve imprimir "Virtual camera created". Deixe aberto.
4. **Teste A — App Câmera do Windows** (o teste mais simples): abra o app *Câmera*, troque para
   "HB Studio Windows Virtual Camera". Esperado: barras de cor com marcador andando; no terminal aparece
   `[camera client active: sending frames]`.
5. **Teste B — OBS Studio:** *Fontes → + → Dispositivo de Captura de Vídeo* → escolha
   "HB Studio Windows Virtual Camera". Esperado: o mesmo vídeo. Anote a resolução/FPS mostrados nas
   propriedades. (**Este é o teste principal do MVP.**)
6. **Teste C — Navegador:** abra https://webcamtests.com no Chrome/Edge e escolha a câmera.
7. Feche o OBS/app Câmera: o terminal deve voltar a `[camera closed/idle...]`. Abra de novo: deve reconectar sozinho.
8. Encerre o `hb_vcam_poc` com Enter. A câmera some da lista.

## O que me enviar
Para cada teste (A, B, C): **funcionou / não funcionou** e, se não funcionou, o que apareceu
(a câmera não existe na lista? aparece mas fica preta? aparece "sem sinal" cinza?). Envie também o
conteúdo de `C:\ProgramData\HBStudio\vcam.log` e o texto que o `hb_vcam_poc.exe` imprimiu.

Interpretação rápida:
| Sintoma | Provável causa |
|---|---|
| `Start failed 0x80070005` | Privacidade de câmera bloqueada |
| `MFCreateVirtualCamera failed` | Windows < 11 (build 22000) |
| Câmera na lista no app Câmera, mas **não no OBS** | OBS (DirectShow) não enxerga câmeras do Frame Server → ativa o plano B (NDI Output / filtro DirectShow) |
| Câmera aparece, imagem preta, log sem "stream started" | Falha na ativação da DLL (veja o log) |
| Cinza com barra branca (sem sinal) mesmo com o app rodando | Pipe não conectou (log: "producer connected" ausente) |

## Desinstalar
PowerShell como administrador: `.\uninstall.ps1`.
