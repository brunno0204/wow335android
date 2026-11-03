# Servidor local para testar o port (AzerothCore 3.3.5a, sem Warden)

Requisitos no PC: Git e Docker Desktop (com compose). Celular e PC na mesma rede Wi-Fi.

1. Baixe o núcleo:
   `git clone https://github.com/azerothcore/azerothcore-wotlk.git --depth 1`
   `cd azerothcore-wotlk`
2. Copie `docker-compose.override.yml` desta pasta para dentro de `azerothcore-wotlk`.
   Ele desliga o Warden (`AC_WARDEN_ENABLED=0`). Sem isso o app para com
   "O servidor exige Warden".
3. Suba tudo (a primeira vez leva 10–15 min, baixa os dados do servidor):
   `docker compose up -d --build`
4. Crie a conta (no console do worldserver):
   `docker attach ac-worldserver`
   `account create teste teste123`
   `account set gmlevel teste 3 -1`   (GM, para usar .tele e .go)
   Saia com Ctrl+P e depois Ctrl+Q (não use Ctrl+C).
5. Descubra o IP do PC (`ipconfig`, "Endereço IPv4"), edite `realmlist-lan.sql`
   com ele e rode no MySQL do container (usuário root, senha `password`):
   `docker exec -i ac-database mysql -uroot -ppassword < realmlist-lan.sql`
   No Windows/PowerShell: `Get-Content realmlist-lan.sql | docker exec -i ac-database mysql -uroot -ppassword`
6. No celular, troque o `realmlist.wtf` da pasta do cliente pelo desta pasta
   (com o mesmo IP). Libere no firewall do Windows as portas TCP 3724 e 8085.
7. No app: login `teste` / `teste123`, escolha o reino AzerothCore e o personagem.
   Crie o personagem no PC (cliente 3.3.5a) — a criação ainda não existe no app.

Dicas de teste com conta GM (digite no chat do PC ou no console do worldserver):
`.tele goldshire`, `.tele orgrimmar`, `.go xyz -8949.95 -132.49 83.53 0` — o app
recebe o teleporte, confirma ao servidor e recarrega o mapa.

O servidor usa build 12340 (padrão do AzerothCore), igual ao app.
