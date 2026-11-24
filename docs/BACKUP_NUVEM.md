# Backup automático em nuvem

Copia as gravações para a nuvem **durante** a transmissão, em segundo plano, sem atrapalhar o ao vivo.
Módulo: `src/cloud/` (biblioteca `hb_cloud`), testes em `tests/cloud_*.cpp`, cartão **CONFIG → Backup em nuvem**.

> **Honestidade primeiro.** Tudo foi testado com *transporte falso* (sem rede) e com os vetores oficiais de assinatura da AWS.
> **Nada foi validado contra um serviço real** (AWS, Backblaze, Cloudflare, Wasabi, MinIO, Google) e o código
> Windows (WinHTTP, DPAPI, prioridade de thread, sondagem de arquivo em uso) **nunca foi compilado nem executado** aqui:
> o CI do Windows e um teste manual são a verificação de verdade. Veja a seção [O que NÃO foi validado](#o-que-não-foi-validado).

## O que está implementado

| Item | Estado |
|---|---|
| **S3 compatível** (AWS S3, Backblaze B2, Cloudflare R2, Wasabi, MinIO) via endpoint/região/bucket/prefixo editáveis | Implementado + testado com transporte falso |
| Assinatura **AWS SigV4** | Implementada; verificada contra 6 vetores da suíte oficial de testes SigV4 e 2 exemplos da documentação do S3 |
| Upload **multipart** (partes de 8 MiB por padrão, mínimo 5 MiB, no máx. 10 000 partes, objeto até 5 TiB; a parte cresce sozinha em arquivos enormes) | Implementado |
| Arquivos pequenos (≤ 1 parte): `PutObject` simples | Implementado |
| **Google Drive** (upload resumível, escopo `drive.file`, OAuth2 *installed app* + PKCE + redirecionamento em `127.0.0.1`, seu próprio *Client ID*) | Construtores de requisição, máquina de estados e fluxo OAuth testados com transporte falso |
| **WebDAV (Nextcloud)** | **Não implementado** |
| Fila **persistente** (diário JSON, sobrevive a reinício, retoma do offset que o servidor confirma) | Implementado |
| Novas tentativas com **espera exponencial** (5 s, 10 s, 20 s… teto de 10 min; 8 tentativas) | Implementado |
| Integridade | S3: MD5 de cada parte contra o ETag + SHA-256 de cada parte assinado (`x-amz-content-sha256`, o servidor rejeita corpo corrompido) + conferência do tamanho final (HEAD) + ETag final do multipart (divergência vira *aviso*, pois nem todo provedor calcula igual). Drive: MD5 do arquivo inteiro contra `md5Checksum` |
| **Dedupe** | Mesmo caminho+tamanho+data não entra duas vezes; se o objeto já existe no destino com o mesmo tamanho, não reenvia |
| Limite de banda (balde de fichas) | Implementado, testado com relógio injetado |
| Pausar durante a transmissão, chave geral, cancelar/repetir | Implementado |
| *Apagar local após o envio* | Implementado, **desligado por padrão**; só apaga depois da confirmação e se o arquivo não mudou |

## Como os arquivos chegam à fila

1. **Evento `recording.fileClosed`** `{path, kind: "program"|"iso-video"|"iso-audio", index}` no `EventBus`: o gravador
   publica quando um arquivo (ou segmento rotacionado) é finalizado. É a fonte autoritativa: o arquivo é enviado na hora.
2. **Reserva por pasta**: a cada 5 s o app olha a pasta de gravações. Um arquivo só é enfileirado se **parou de crescer
   há pelo menos N segundos** (padrão 30) e não está aberto por outro programa (Windows). A **primeira varredura nunca
   envia o histórico** (só arquivos novos ou que mudarem depois); gravações antigas não são enviadas.
   O tipo é deduzido pelo nome: contendo "iso" → ISO (`.wav`/`.flac` = áudio, resto = vídeo); senão PROGRAM.
3. Segurança extra no motor: mesmo que um arquivo entre sem o aviso de "fechado", ele só sobe se continuar com tamanho e
   data iguais e fora de uso; se crescer, a tentativa recomeça. Extensões aceitas: `mp4 mkv mov wav flac avi`.

Nome no destino: `AAAA-MM-DD/<program|iso-video|iso-audio>/<arquivo>` dentro do prefixo configurado
(no Drive o caminho vira um nome só: `AAAA-MM-DD_program_arquivo.mp4`, dentro da pasta "HB Studio").

## Nunca atrapalhar a transmissão

- Uma única thread de upload com **prioridade baixa** (no Windows, `THREAD_MODE_BACKGROUND_BEGIN`: CPU e disco).
- **Pausar durante a transmissão** (ligado por padrão): com qualquer destino RTMP/SRT ativo nada sobe; se a transmissão
  começar no meio de uma parte, o envio é interrompido na hora (a parte é reenviada depois, sem contar como falha).
- **Limite de upload em Mbps** (0 = sem limite). O corpo é enviado em fatias de 32 KiB, cada uma liberada pelo limitador.
- **Chave geral** ("Enviar as gravações…" desmarcada): para tudo imediatamente e nada novo entra na fila.

### A realidade da banda de upload

A gravação padrão do app é ~8 Mbps de vídeo + 0,2 Mbps de áudio ≈ **8,2 Mbps ≈ 3,7 GB por hora**.
Subir "ao vivo" uma gravação exige, portanto, **mais de 8,2 Mbps de upload sobrando**, além da própria transmissão
(um RTMP 1080p típico usa 4,5–6 Mbps). Em um link de 20 Mbps de upload, transmitindo a 6 Mbps sobram ~14 Mbps: dá,
com folga pequena. Em links de 5–10 Mbps (comum em ADSL/igrejas) **não dá** para fazer as duas coisas; use o padrão
(pausar durante a transmissão) e deixe o backup terminar depois, ou grave em bitrate menor. Faça a conta: 1 hora de
culto = ~3,7 GB; a 10 Mbps de upload livres levam ~50 min para subir. Trilhas ISO multiplicam o volume.
O limite em Mbps serve para deixar folga ao resto da rede.

## Criar um bucket e chaves

Os preços são **aproximados e mudam**: confirme na página do provedor antes de usar.

### Cloudflare R2 (sem taxa de saída/egress)
1. Painel Cloudflare → **R2** → *Create bucket* (ex.: `gravacoes-igreja`).
2. **R2 → Manage R2 API Tokens → Create API token**, permissão *Object Read & Write*, restrito ao bucket.
3. Anote *Access Key ID*, *Secret Access Key* e o endpoint `https://<ACCOUNT_ID>.r2.cloudflarestorage.com`.
4. No app: Endpoint = esse endereço, **Região = `auto`**, Bucket, Endereçamento = *Path-style*.
5. Custo de referência: ~US$ 0,015/GB-mês, sem cobrança de egress; há cota gratuita mensal.

### Backblaze B2
1. *Buckets → Create a Bucket* (privado). Anote o endpoint S3 mostrado (ex.: `https://s3.us-east-005.backblazeb2.com`)
   e a região (`us-east-005`).
2. *Application Keys → Add a New Application Key*, restrita ao bucket, leitura e escrita.
   Atenção: a chave **só aparece uma vez**; "keyID" = Access Key ID, "applicationKey" = chave secreta.
3. No app: Endpoint, Região (do endpoint), Bucket, Endereçamento = *Path-style*.
4. Custo de referência: ~US$ 6/TB-mês; saída gratuita até ~3× o armazenado por mês.

### Wasabi / MinIO / AWS S3
- **Wasabi**: endpoint regional (`https://s3.<região>.wasabisys.com`); tem **retenção mínima de 90 dias** de cobrança por arquivo apagado.
- **MinIO**: `http://seu-servidor:9000` (cuidado: `http` não é criptografado), Endereçamento = *Path-style*.
- **AWS S3**: use o endpoint **regional** (`https://s3.sa-east-1.amazonaws.com`) e a região correspondente; crie um usuário IAM
  com permissão apenas de `s3:PutObject`, `s3:AbortMultipartUpload`, `s3:ListMultipartUploadParts` (e `s3:GetObject`
  para a conferência de tamanho) nesse bucket. Egress da AWS é cobrado (~US$ 0,09/GB) quando você **baixa**.

### Custos e armadilhas gerais
- Uploads em si costumam ser grátis; paga-se **armazenamento** e, em alguns provedores, **baixar** (egress) e requisições.
- Uploads multipart interrompidos e nunca concluídos **ficam ocupando espaço** em alguns provedores; configure uma regra de ciclo de vida
  para abortar multiparts incompletos após alguns dias. O app tenta abortar o que cancelar.
- **Teste de conexão**: grava `.hbstudio-teste.txt` no prefixo e tenta apagá-lo. Se a chave não puder apagar, o teste
  continua "OK" e avisa para você apagar o arquivo à mão.
- Chaves restritas sem `ListBucket` fazem o dedupe por HEAD não distinguir "não existe" de "sem permissão": o app trata como
  "não existe" e os problemas reais de credencial aparecem no envio.

## Google Drive

1. Em <https://console.cloud.google.com> crie um projeto, ative a **Google Drive API**, configure a tela de consentimento OAuth
   (adicione você como usuário de teste) e crie um **ID do cliente OAuth do tipo "App para computador"**.
2. No app: cole o *Client ID* (e o *Client secret*, que o Google também emite para esse tipo), clique **CONECTAR CONTA GOOGLE**,
   autorize no navegador. O retorno é recebido numa porta local em `127.0.0.1` (nunca exposta à rede) com `state` anti-falsificação e PKCE.
3. Escopo `drive.file`: o app só enxerga o que ele mesmo criou, na pasta "HB Studio". O token de renovação fica protegido (DPAPI).
- Limites do Google: ~750 GB/dia de upload por conta e a cota de armazenamento da conta (15 GB grátis). Quando acabar, o erro aparece claro na fila.
- Em projetos em modo "Testing" o token de renovação **expira em 7 dias**: publique o app (ou reconecte quando o erro aparecer).

## Segredos

- Chaves/segredos/tokens ficam **fora** do arquivo de configurações (`cloud.json`), em `cloud_secrets.json`, cada valor cifrado
  com **DPAPI** (`CryptProtectData`, por usuário do Windows). A interface só recebe uma dica mascarada (`••••ABCD`; da chave
  secreta nem isso) e os campos são só de escrita: em branco = manter a guardada.
- Nunca vão para o registro (log). As mensagens de erro do servidor passam por filtro de caracteres de controle e tamanho.
- O diário da fila (`cloud_journal.json`) guarda caminhos, UploadId do multipart e a URI da sessão resumível do Drive
  (a URI funciona como um "bilhete" de upload): proteja a pasta do usuário como qualquer outro arquivo de configuração.

## Erros que a interface mostra

Sem permissão/chave errada, relógio do Windows desajustado, região/endpoint errados, bucket inexistente, **cota/espaço do provedor
esgotado**, limite diário do Drive, sessão expirada (o arquivo recomeça do zero), arquivo local sumiu, MD5/ETag divergente (a parte é reenviada).
Erros de credencial/cota **não** ficam tentando sem parar: o item vai para *Erro* até você clicar **REPETIR**.
Falhas de rede são tentadas de novo sozinhas, com espera crescente.

## Comandos (UI → app) e estado (app → UI)

`cloudsave|{json}` · `cloudtest` · `cloudgoogle|login` · `cloudclear|s3|gdrive` · `cloudretry|<id>|all` · `cloudcancel|<id>` ·
`cloudclearfinished` · `cloudscan`. O app responde com `cloud_state` (configuração sem segredos, dicas, fila, totais, velocidade).

## O que NÃO foi validado

- **Nenhum serviço real**: nunca houve uma requisição de verdade a AWS, B2, R2, Wasabi, MinIO ou Google. Particularidades de cada
  provedor (cabeçalhos exigidos, ETag de multipart, limites de partes do B2, `Region=auto` do R2, comportamento de `HEAD` sem permissão) estão baseadas na documentação, não em teste.
- **Código só do Windows**, escrito sem compilador Windows: cliente WinHTTP (envio em fatias, política de não seguir redirecionamentos,
  leitura de cabeçalhos), DPAPI, `THREAD_MODE_BACKGROUND_BEGIN`, sondagem de arquivo em uso (`CreateFile` sem compartilhamento), `ShellExecute` do navegador,
  e o ramo Winsock do ouvinte de loopback (o ramo POSIX é exercitado nos testes).
- Fluxo OAuth do Google ponta a ponta com o Google de verdade (tela de consentimento, expiração de 7 dias em modo teste).
- Comportamento sob queda de rede/energia no meio de um upload em arquivos de dezenas de GB.
- A publicação do evento `recording.fileClosed` pelo gravador (agente de gravação multitrilha): até esse trabalho ser integrado, vale a reserva por pasta.
- Não há "enviar gravações antigas" nem WebDAV; o dedupe de S3 compara só tamanho (não conteúdo) com o objeto já existente.
