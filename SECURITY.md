# Política de segurança

## Versões suportadas

O HB Studio é pré-1.0 e publica uma única versão contínua: a última build da branch `main`
(instalador em [Releases → latest](https://github.com/jrscbusiness/hb-studio/releases/tag/latest)).
Correções de segurança entram nela; não há versões antigas com suporte.

| Versão | Suportada |
|---|---|
| Última build de `main` (`latest`) | Sim |
| Builds anteriores | Não — atualize |

## Como relatar uma vulnerabilidade (em privado)

**Não abra uma issue pública** para vulnerabilidades.

1. Use o relato privado do GitHub: aba **Security → Report a vulnerability**
   (<https://github.com/jrscbusiness/hb-studio/security/advisories/new>).
2. Inclua: versão (rodapé do app), versão do Windows, passos para reproduzir, impacto esperado e, se
   possível, uma prova de conceito. **Não inclua** chaves ou tokens reais.

Este é um projeto mantido por voluntários: esperamos responder em alguns dias e corrigir conforme a
gravidade. Pedimos divulgação coordenada: dê-nos tempo razoável para corrigir antes de publicar
detalhes. Com sua permissão, damos crédito no aviso.

## Escopo

Dentro do escopo (exemplos):

- Execução de código ou escrita fora das pastas esperadas via pacote `.hbplugin`, catálogo do
  Marketplace, projeto/template importado ou arquivo de configuração malformado.
- Falhas no servidor de controle remoto: contornar o pareamento por PIN, comandos fora da lista
  permitida, acesso de fora da rede local.
- Vazamento de segredos (chaves de transmissão, tokens OAuth, chaves de API, chave S3) em logs,
  arquivos, rede ou interface.
- Travamento remoto causado por entrada de rede (RTMP, RTSP, ONVIF, NDI, chat) sem a validação
  exigida em [docs/CONTRIBUTING.md](docs/CONTRIBUTING.md).
- Problemas no instalador / câmera virtual (ex.: permissões amplas demais no pipe, elevação de
  privilégio).

## Fora do escopo / decisões conhecidas

- **O controle remoto pela rede local usa HTTP/WebSocket sem TLS, por projeto**: não existe
  certificado que um celular aceite na LAN sem aviso. Quem está na mesma rede e consegue capturar
  pacotes pode ver o PIN e a chave do aparelho. Mitigações (somente IPs privados, PIN de uso único com
  anti-força-bruta, chaves por aparelho, lista fechada de comandos, validação de `Origin`/`Host`) estão
  descritas em [docs/CONTROLE_REMOTO.md](docs/CONTROLE_REMOTO.md). Use apenas em rede de confiança, nunca
  em Wi-Fi público. Relatar "não usa HTTPS" não é uma vulnerabilidade; contornar as mitigações é.
- O instalador **ainda não é assinado** (Authenticode); o aviso do SmartScreen é esperado (veja
  [BUILD.md](BUILD.md)).
- Plugins nativos (`.dll`) de terceiros executam código com os privilégios do usuário — o app avisa e
  pede confirmação (veja [docs/PLUGINS.md](docs/PLUGINS.md) e [docs/MARKETPLACE.md](docs/MARKETPLACE.md));
  um plugin malicioso instalado de propósito não é uma falha do HB Studio, mas falhas que o escondam ou
  contornem o aviso são.
- Chaves de provedores (IA, nuvem) ficam no computador do usuário; quem já tem acesso à conta do
  Windows do usuário está fora do modelo de ameaça.
- Vulnerabilidades em dependências de terceiros: relate-as ao projeto original; avise-nos se o HB Studio
  estiver afetado.
