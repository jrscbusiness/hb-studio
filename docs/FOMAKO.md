# FOMAKO NK20A e câmeras PTZ FoMaKo — pesquisa de protocolos

Pesquisa feita em 2026-10-20 (WebSearch/WebFetch). **Nenhuma câmera real foi testada.**
Cada afirmação tem nível de confiança:

- **ALTA** — lido por mim no texto do manual oficial do fabricante (PDF em fomako.net), em mais de um manual.
- **MÉDIA** — vem de uma fonte única, de snippet de busca (página não pôde ser aberta) ou de fórum.
- **SUPOSIÇÃO** — inferência minha; **não** é fato confirmado.

## 1. O modelo "NK20A"

**Não encontrei nenhuma página, manual ou anúncio de uma câmera "FOMAKO NK20A".** A busca por "FOMAKO NK20A"
retorna apenas rolamentos (Koyo NK20/16A) e itens sem relação.

O modelo mais próximo que existe é a **FoMaKo KN20A Pro** (NDI, zoom óptico 20x) — a grafia "NK20A" pode ser
inversão de letras de "KN20A" (**SUPOSIÇÃO**). A família "KN" (KN20A, KN30A Pro…) é a linha NDI da FoMaKo.

| Afirmação | Confiança | Fonte |
|---|---|---|
| Existe "FoMaKo KN20A Pro NDI PTZ Camera, 20x" à venda | MÉDIA (snippet) | [B&H KN20A Pro](https://www.bhphotovideo.com/c/product/803471568-USE/fomako_kn20a_pro_ndi_ptz.html) (a página retornou 403 ao abrir; só vi o resumo da busca) |
| Ficha KN20A Pro: controle Pelco-D / Pelco-P / VISCA; H.264/H.265/MJPEG; PoE | MÉDIA (snippet) | mesma página B&H |
| Listagem "KN20" na Newegg cita ONVIF, RTSP, RTMP/RTMPS, SRT, NDI 6 / HX3 | MÉDIA (snippet, lojista) | [Newegg](https://www.newegg.com/p/3C6-02V2-000A2) |
| O manual da KN30A Pro existe e é "NDI 6 / NDI HX3 certified" | MÉDIA (snippet) | [manuals.plus KN30A](https://manuals.plus/asin/B0B14V24F3) |

**Conclusão:** para a NK20A/KN20A usar os valores da seção 2 (manuais de modelos irmãos) **como ponto de partida a
verificar na câmera real**, nunca como garantido.

## 2. Manuais oficiais FoMaKo lidos (fomako.net)

Baixei e li o texto de dois manuais oficiais (modelos 4K / 12G-SDI, mesma família de firmware/UI):

- [FoMaKo 4K NDI/SDI/HDMI/USB PTZ STREAM CAMERA — User Manual V1.0](https://www.fomako.net/uploads/20230823/e2b9ad5c55b3164c5e06383b05d47dd3.pdf)
- [FoMaKo 12G-SDI HDMI NDI USB 4K PTZ CAMERA — User Manual V1.0](https://www.fomako.net/uploads/20250710/ca7f07cb130e5d205543d889a6716605.pdf)
- [FoMaKo Professional PTZ Controller User Manual V1.3](https://www.fomako.net/uploads/20260122/a4a50ff11cd404e94d9b05e3e68df73b.pdf) (controlador KC-608 etc.)

Outros manuais FoMaKo que aparecem na busca (não lidos): [1080P PTZ IP Stream](https://manuals.plus/fomako/1080p-ptz-ip-stream-camera-manual),
[20X IP Streaming](https://manuals.plus/fomako/20x-ip-streaming-camera-manual) (manuals.plus retornou 403).

### 2.1 Protocolos e portas

| Item | Valor | Confiança | Fonte |
|---|---|---|---|
| Protocolos de rede da câmera | NDI\|HX, TCP/IP, HTTP, RTSP, RTMP/RTMPS, **ONVIF** (tabela de especificações) | ALTA (2 manuais) | manual 4K V1.0 e 12G V1.0 |
| Porta HTTP (web UI) | 80 | ALTA | manual 4K V1.0, tela Network |
| Porta RTSP | 554 | ALTA | idem |
| **IP VISCA TCP** | **5678** | ALTA | idem; "Connect Camera to Controller Keyboard" |
| **IP VISCA UDP** | **1259** | ALTA | idem |
| **Sony VISCA (over IP, UDP)** | **52381** | ALTA | idem |
| **ONVIF** | porta **2000** (citada na lista de portas para teclado/controlador) e opção ON/OFF + "ONVIF Auth" ON/OFF na tela Network | ALTA | idem |
| SRT | porta 4578 (padrão na UI), com criptografia/senha opcionais | ALTA | idem |
| Multicast | padrão 224.1.2.3, porta 6688 | ALTA | idem |
| FoMaKo recomenda "Sony VISCA ou IP VISCA" para controle (mais estáveis); o manual do 12G recomenda "Sony Visca (Visca over IP)" | ALTA | idem |
| O **controlador FoMaKo** recomenda **ONVIF** ("better compatibility") para adicionar câmeras e também suporta VISCA-IP, Pelco-D/P, NDI | ALTA | [Controller Manual V1.3](https://www.fomako.net/uploads/20260122/a4a50ff11cd404e94d9b05e3e68df73b.pdf) |
| Quick-start V3.2 (via B&H) lista os mesmos números: IP VISCA 5678, Sony VISCA 52381, ONVIF 2000 | MÉDIA (snippet) | [B&H lit_files/1357815.pdf](https://www.bhphotovideo.com/lit_files/1357815.pdf) (403 ao abrir) |

Nota: o controlador pede "camera control ID (1–255)" ao adicionar VISCA IP (ALTA, manual 4K V1.0). Como esse número
entra no endereço do pacote VISCA over IP é **desconhecido** — ver docs/PTZ.md.

### 2.2 Serial (RS-232 / RS-485)

| Item | Valor | Confiança |
|---|---|---|
| Portas | 1x RS485 (3 pinos phoenix, até 1200 m), 1x RS232 IN e 1x RS232 OUT (mini-DIN 8 pinos, 30 m); protocolos VISCA / Pelco-D / Pelco-P | ALTA (manual 4K V1.0) |
| Protocolo selecionável no menu OSD | Auto, VISCA, PELCO-D, PELCO-P | ALTA |
| Baud rate | 2400 / 4800 / 9600 / 38400; padrão 9600 | ALTA |
| Endereço VISCA | 1–7 (só efetivo em Auto/VISCA); "V_AddrFix" On/Off | ALTA |
| Endereço Pelco-D | 0–254; Pelco-P 0–31 (só em Auto) | ALTA |
| Presets | 255 posições, precisão 0,1°; velocidade de chamada 1–24 | ALTA (spec do manual 4K) |

### 2.3 Rede, credenciais, RTSP

| Item | Valor | Confiança | Fonte |
|---|---|---|---|
| Usuário/senha padrão da web UI | **admin / admin** | ALTA | manuais 4K e 12G; também no manual do controlador (admin) |
| IP padrão (manual 4K/12G) | **192.168.100.88** (gateway 192.168.100.1) | ALTA | manual 4K V1.0 (tela Network) e 12G ("default IP address: 192.168.100.88") |
| IP padrão de fallback (quick-start V3.2 / fórum) | 192.168.5.163 (LCD) / exemplo 192.168.5.173 | MÉDIA | snippet B&H; fórum OBS |
| Default é DHCP; o manual recomenda trocar para IP fixo | | ALTA | manual 4K V1.0 |
| **RTSP, modelos 4K/12G (manual)** | `rtsp://<ip>:554/1` (chamado "RTSP UDP Stream URL") | ALTA | manual 4K V1.0 e 12G V1.0 |
| **RTSP, modelos 20x IP/FMK12UH (fórum, resposta da própria FoMaKo)** | principal `rtsp://<ip>:554/live/av0`; sub `rtsp://<ip>:554/live/av1` (também `rtsp://<ip>` curto) | MÉDIA (fórum, mas postado pela conta "fomako") | [fórum OBS](https://obsproject.com/forum/threads/connecting-fomako-ptz-cameras-and-controller-to-obs.170069/) |
| O controlador FoMaKo lê sozinho as URLs RTSP HD/SD de câmeras FoMaKo; senão usa "main stream / sub stream" | | ALTA | Controller Manual V1.3 |
| Autenticação RTSP é opcional (ON/OFF) | | ALTA | manual 4K V1.0 |
| Pode haver diferença de caminho por modelo/firmware: `/1` vs `/live/av0`. **Testar ambos na NK20A/KN20A.** | | SUPOSIÇÃO baseada nas duas fontes acima | |

### 2.4 O que NÃO encontrei

- Manual específico da "NK20A" (nem da KN20A Pro) — não achei PDF dela.
- Formato exato do cabeçalho/endereçamento do VISCA over IP FoMaKo (se usa o enquadramento Sony de 8 bytes na 52381, se
  o protocolo "IP VISCA" 1259/5678 é puro VISCA sem cabeçalho, ou outro). **Não documentado nos manuais que li.**
- Caminho ONVIF (`/onvif/device_service`) e perfis (S/T) da FoMaKo: o manual só diz "ONVIF On/Off, porta 2000" —
  detalhes de perfil/serviço PTZ/Imaging **não confirmados**.
- Se a câmera expõe foco/íris por ONVIF Imaging — não confirmado.
- Comandos de foco manual/íris via VISCA nessa linha — não confirmado (a UI web tem Focus In/Out e Zoom).
- API HTTP/CGI proprietária — nenhuma documentação encontrada.

## 3. Fontes

1. FoMaKo 4K User Manual V1.0 — https://www.fomako.net/uploads/20230823/e2b9ad5c55b3164c5e06383b05d47dd3.pdf
2. FoMaKo 12G-SDI User Manual V1.0 — https://www.fomako.net/uploads/20250710/ca7f07cb130e5d205543d889a6716605.pdf
3. FoMaKo PTZ Controller Manual V1.3 — https://www.fomako.net/uploads/20260122/a4a50ff11cd404e94d9b05e3e68df73b.pdf
4. Fórum OBS (resposta da FoMaKo, RTSP av0/av1) — https://obsproject.com/forum/threads/connecting-fomako-ptz-cameras-and-controller-to-obs.170069/
5. B&H KN20A Pro (snippet) — https://www.bhphotovideo.com/c/product/803471568-USE/fomako_kn20a_pro_ndi_ptz.html
6. B&H quick start V3.2 (snippet; 403) — https://www.bhphotovideo.com/lit_files/1357815.pdf
7. Newegg KN20 (snippet) — https://www.newegg.com/p/3C6-02V2-000A2
8. manuals.plus KN30A — https://manuals.plus/asin/B0B14V24F3
