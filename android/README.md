# Port Android ARM64 — cliente 3.3.5a (build 12340)

## O que funciona (v0.83)
- Telas originais de login, reinos e personagens lidas dos MPQs do celular.
- Cenário 3D original atrás das telas (ModelFFX): login (modelo do AccountLogin), seleção
  (cenário da raça do personagem, com o personagem 3D na posição do cenário) e criação
  (cenário da raça/Cavaleiro da Morte). Câmera 0 do modelo, arrastar gira o personagem.
- Criação de personagem completa: raças por facção (Aliança/Horda, bloqueio por expansão),
  classes válidas da raça, sexo, pele, rosto, estilo e cor de cabelo, traço facial (barba,
  brincos, chifres…), aleatório, nome aleatório (gerador original NameGen) e criar.
  Regras portadas do cliente original (CCharacterComponent/ComponentUtils/CCharacterCreation).
- Apagar personagem (com confirmação). A lista é atualizada após criar/apagar.
- Personagem com textura composta como no original (pele + rosto + barba + couro cabeludo +
  roupa íntima) e geosets de cabelo/barba.
- Login SRP no authserver, escolha de reino e personagem (servidor/porta do `realmlist.wtf`).
- Entrada no mundo com o personagem equipado (texturas de armadura por camada, geosets de
  luvas/botas/mangas/robe/capa/cinto, elmo, ombreiras e armas nos pontos de encaixe do M2,
  regras de CCharacterComponent::GeosRenderPrep/UpdateItem). A seleção de personagem também
  mostra o equipamento.
- Mapa em blocos 3x3 ao redor do jogador, carregados em segundo plano e trocados ao andar:
  terreno, WMOs (com o conjunto de doodads de cada WMO) e doodads M2.
- Colisão: chão de WMO (cavernas, prédios), paredes de WMO e de doodads (troncos, pedras),
  degraus, queda e salto (gravidade e velocidade do cliente), câmera que não atravessa paredes.
- Câmera: arrastar gira, pinça aproxima/afasta (1,5 a 50 jardas).
- Animações do personagem: parado, correndo e pulando.
- Outros jogadores, NPCs e objetos (SMSG_UPDATE_OBJECT/COMPRESSED, DESTROY, MONSTER_MOVE,
  movimento dos outros jogadores): modelos de criatura (CreatureDisplayInfo, variações de
  textura, geosets), NPCs humanoides (CreatureDisplayInfoExtra com textura “baked” e armas),
  jogadores com equipamento (consulta de itens), nomes (consultas de nome/criatura).
- Interface original do WoW (beta, Configuração → "Interface original do WoW no mundo"): o
  motor de UI decompilado (CSimpleTop, FrameXML, FrameScript/Lua 5.1, fontes Gxu/FreeType,
  texturas BLP) carrega `Interface\FrameXML\FrameXML.toc` dos MPQs e desenha sobre o mundo.
  Dados do jogo chegam pela camada de jogo em Lua (`Tools/AndroidPort/ui/*.lua`, embutida por
  `tools/gen_ui_prelude.py`); as outras ~2000 funções da API original existem como stubs que
  devolvem o "sem dados" do original (0, texto ou nada; `tools/gen_ui_api.py` lê os retornos das
  funções Script_* decompiladas).
- Relatório de travamento (v0.65): se o app fechar por erro nativo (ou exceção C++), na próxima
  abertura aparece "O jogo fechou na última vez" com motivo, thread, PC/LR, última etapa
  (ex.: o arquivo do FrameXML sendo carregado), últimas mensagens Lua e a pilha — botão Copiar.
  Se a falha foi num arquivo do FrameXML, esse arquivo é pulado na próxima vez (até 12, lista
  zerada a cada versão nova); outras falhas na UI desligam a interface original.
- v0.66: Lua compilado sem FORTIFY do Android. O `strchr` fortificado do bionic recebia tamanho 0
  para strings Lua (texto guardado logo após o cabeçalho TString) e abortava no GC
  (`lgc.c traversetable`) ao iniciar a interface.
- v0.67: no mundo, a 2ª linha do topo mostra o estado da UI original (desligada, aguardando,
  falhou + motivo, ou arquivos carregados/erros/quadros desenhados). Tocar no texto abre o log
  da UI (erros Lua, mensagens do FrameXML, visibilidade de UIParent/PlayerFrame/MainMenuBar/
  ChatFrame1) com Copiar e Ligar/Desligar UI.
- v0.68: a UI original carregava (146 arquivos, quadros visíveis) mas não aparecia: o mundo
  deixava no dispositivo Gx a matriz World do último modelo desenhado, culling etc., e o
  CSimpleRender só ajusta fog/profundidade/blend. `uiPaint` agora desenha as camadas com
  estado limpo (RsPush + estados de UI + World/Tex0/Tex1 identidade, como CGxDevice::ICursorDraw).
- v0.69 (UI original):
  - Toque: o dedo é o botão esquerdo do mouse (apertar, arrastar, soltar), em pixels da
    superfície (o mundo renderiza a 80%, antes os toques caíam no lugar errado). Dedo sobre
    um quadro da UI não gira a câmera nem seleciona alvo; deslizar na vertical rola (chat, listas).
  - Chat: janelas com cor/transparência originais (GetChatWindowInfo & cia. com os padrões do
    cliente), abas Geral/Registro de combate, cores por tipo (UPDATE_CHAT_COLOR), CVars com os
    valores padrão do cliente (`tools/gen_ui_cvars.py`). Digitar: tocar na caixa do chat abre o
    teclado do Android; Enter envia (SendChatMessage → CMSG_MESSAGECHAT; /s, /y, /w, /p, /g, emotes).
  - Dados: magias conhecidas (livro de magias com abas Geral + especializações da classe, ícones
    SpellIcon, ranks, passivas), barras de ação (SMSG_UPDATE_ACTION_BUTTONS; tocar lança a magia
    com CMSG_CAST_SPELL no alvo; arrastar do livro para a barra com CMSG_SET_ACTION_BUTTON),
    bolsas e equipamento (campos do jogador + objetos de item, consulta de itens, ícones,
    links, mover/equipar/usar itens), dinheiro e XP, ficha do personagem (atributos, armadura,
    dano), missões (registro por zona, objetivos com contagem, rastreador de objetivos,
    abandonar), grupo (lista, convite com janela de aceitar/recusar, líder, sair), tooltips.
  - Addons da Blizzard dos MPQs carregados sob demanda (LoadAddOn: relógio, calendário,
    talentos, conquistas, registro de combate…).
  - Minimapa: blocos `textures\Minimap` (md5translate.trs) em volta do jogador, redondo, com
    seta e zoom.
  - 3D: quadros de modelo (ficha do personagem, provador) desenham o modelo carregado da
    unidade; retratos do jogador/alvo/grupo em 3D (renderizados e recortados em círculo; o
    ícone da classe aparece até o modelo carregar).
  - Testado no PC com o FrameXML 3.3.5 real (sem erros de script) num harness EGL sem tela.
- v0.70: CHARACTER_POINTS_CHANGED com os argumentos do original (o ChatFrame comparava o 2º
  argumento e dava erro); o primeiro valor visto de um campo não dispara evento de mudança.
- v0.71 (UI original + jogabilidade):
  - Pacotes do servidor para a UI (`ServerEvents.cpp`): auras (buffs/debuffs com tempo e
    acúmulos, cancelar buff), barra de lançamento e canalização (sua e do alvo), recargas de
    magias e itens + recarga global, entrar/sair de combate, registro de combate
    (COMBAT_LOG_EVENT_UNFILTERED: golpes, magias, curas, periódicos, erros, XP), números de dano
    nos retratos (UNIT_COMBAT), subir de nível, mensagens de saque/dinheiro/XP no chat.
  - Interação: tocar de novo no alvo já selecionado fala com o NPC (gossip, missões,
    vendedor, treinador, banco, curandeiro espiritual), saqueia o corpo ou ataca; tocar num
    objeto (baú, erva, objeto de missão) usa o objeto. `InteractUnit` na API.
  - Janelas: saque (dinheiro e itens), conversa (opções e missões), missões do NPC
    (detalhes, aceitar, progresso, recompensas e escolha), vendedor (comprar; tocar num item da
    bolsa com o vendedor aberto vende; consertar tudo), treinador (Blizzard_TrainerUI).
    Textos com $N/$R/$C/$G substituídos como no cliente.
  - Morte: soltar espírito, fantasma, recuperar corpo quando perto, ressurreição por outro
    jogador e curandeiro espiritual.
  - Social: amigos e ignorados (adicionar/remover, nível/classe/zona), /who, guilda (lista,
    MOTD, ranques).
  - Zona e subzona reais (AreaTable da área do terreno sob o jogador): texto do minimapa,
    aviso ao entrar em zona, território (contestado/aliado/hostil/santuário).
  - Toque longo (½ s parado) = botão direito do mouse; arrastar só começa depois de mover.
  - Escala da interface: automática maior em telas de celular; `/escala 0.8–1.6`.
  - Macros (criar/editar/apagar na janela de macros, na barra de ações, com condicionais
    [target=], [@], combat, harm, help…), variáveis salvas dos addons e CVars alteradas
    guardadas em `files/WTF/` (conta e personagem), gravadas a cada 30 s.
- v0.72 (mundo, movimento, áudio):
  - Movimento: empurrar pouco o joystick anda devagar (modo andar no servidor); puxar para trás
    anda de costas olhando para frente (MSG_MOVE_START_BACKWARD), como no original. Botão
    "CÂMERA LIVRE/SEGUE": com ele ligado, arrastar gira só a câmera enquanto o personagem segue o
    rumo; ao soltar, a câmera volta para trás dele.
  - Nadar: água do mapa (MH2O dos ADTs) desenhada (rio/mar com a cor da luz do dia, lava, lodo) e
    natação ao entrar em água funda (flutua na superfície, MSG_MOVE_START_SWIM/STOP_SWIM, animações).
  - Céu, neblina e dia/noite: Light.dbc/LightParams/LightIntBand/LightFloatBand como o original
    (Lights.cpp/DayNight decompilados): domo de céu com as faixas de cor, neblina com a distância
    da zona (esconde a borda do mapa), escurecimento à noite; hora do jogo do servidor
    (SMSG_LOGIN_SETTIMESPEED; GetGameTime na UI).
  - Montarias: jogador e outros montados aparecem na montaria (modelo de CreatureDisplayInfo,
    cavaleiro no anexo 0 com a animação de montaria).
  - Animações: emotes do servidor (dançar, sentar, acenar…), golpes corpo a corpo, ferimento,
    esquiva/aparo, preparar/lançar/canalizar magia, nadar, morto — para você e para os outros.
  - Grama e plantas do chão (GroundEffectTexture/GroundEffectDoodad, espalhadas como o
    CMapChunk::CreateDetailDoodads), perto do jogador.
  - Masmorras/raides só de WMO (WDT com WMO global) carregam e permitem andar.
  - Áudio: música da zona (ZoneMusic, dia/noite, com o intervalo de silêncio do original),
    som ambiente (SoundAmbience), sons da interface (PlaySound) e das magias (SpellVisualKit),
    volumes das opções de som do original. Arquivos extraídos dos MPQs para o cache.
  - Login: modelos com mais de 256 ossos (o cenário de Nortúndria) agora carregam — antes
    falhavam e o app caía no cenário antigo do Portal Negro.
- v0.73: joystick menor, no meio do lado esquerdo (o canto de baixo fica livre para a UI) e
  dica de gestos escondida com a UI original; NPCs e você tocam a animação de morte uma vez e
  ficam caídos (corpos já mortos aparecem caídos); o golpe não é mais cortado pela animação de
  ferimento; postura de combate entre golpes; texto de território da zona ("Território da
  Horda") sem o erro do ZoneText.lua; linha de estado mostra quantas animações suas chegaram.
- v0.74: posturas e formas (barra de posturas do guerreiro, formas do druida, furtividade…)
  pelos feitiços com SPELL_AURA_MOD_SHAPESHIFT; a barra de ações troca para a barra da postura
  (SpellShapeshiftForm.bonusActionBar), onde o servidor guarda as magias do guerreiro; nomes e
  ícones de itens do saque/vendedor/missão aparecem quando a informação do item chega.
- v0.75: joystick pequeno na área livre do mundo (à direita do chat, acima da barra de ações);
  o retrato 3D do seu personagem continua tentando até o modelo carregar (antes desistia em 30 s
  e ficava o ícone da classe); mapa-múndi: arte da zona (WorldMapArea + todas as camadas
  WorldMapOverlay), continentes e zonas, sua posição no mapa — e sem o erro do
  WorldMapFrame.lua:864 (posições de campo de batalha).
- v0.76: com a UI original, o botão "Sair do mundo" some (ficava em cima do X do mapa e de
  outros botões do canto) e a linha de estado vira uma linha curta ("FPS · UI ok ⓘ"); tocar nela
  abre o log com o estado completo e o botão Sair do mundo. Botão Voltar do Android = Esc:
  fecha o mapa e as janelas, abre o menu do jogo; Logout/Sair do jogo do menu saem do mundo.
- v0.77: espadada de verdade — ao atacar a arma sai da bainha para a mão (pontos de anexo da
  mão) e armas de duas mãos usam Attack2H/Ready2H. Telas de login e seleção de personagem com
  efeitos do M2 de cenário: partículas (emissores plano/esfera, gravidade, arrasto, células de
  textura) e materiais animados (UV rolando, cor/alpha e pesos de textura animados). Tooltips:
  loot, item de missão, vendedor (preço de venda), treinador, forma, auras/buffs (descrição e
  tempo restante), unidades (cor de reação, raça/classe, tipo) e texto de magias com $s/$d/$o
  etc. substituídos, custo/alcance/conjuração/recarga; ponteiro sai da tela 4 s após o toque
  para o tooltip sumir.
- v0.78: retrato 3D do jogador no PlayerFrame — o pedido do retrato do "player" fica preso ao
  modelo do herói mesmo quando a UI chama SetPortraitTexture antes do GUID chegar (antes ficava
  para sempre o ícone da classe, o "X" de espadas cruzadas do guerreiro); leitura de pixels que
  falha tenta de novo em vez de desistir.
- v0.79: arma na mão em combate e até 6 s depois do último golpe (o servidor só muda a bainha
  com CMSG_SETSHEATHED, que o cliente original manda sozinho). Retrato 3D: imagem vazia (preta)
  não substitui mais o ícone e é refeita; o log mostra o resultado de cada tentativa e o erro do
  desenho. Livro de magias sem proficiências de arma/armadura e idiomas na aba Geral (nem magias
  ocultas). Diagnóstico das barras de vida/poder no log 20 s depois de entrar no mundo.
- v0.80: marcas de missão sobre NPCs com os modelos originais (TalkToMe "!" / "?" amarelos,
  cinzas, azuis), no ponto de anexo 18 e na escala do PlayerNameComputeScale; status vindo de
  CMSG/SMSG_QUESTGIVER_STATUS_MULTIPLE. Nomes sobre as unidades com a fonte original
  (FRIZQT__.TTF do jogo), contorno e as cores de reação do cliente (FactionTemplateReaction:
  vermelho hostil, laranja, amarelo neutro, verde amigável, azul jogadores sem PvP, cinza
  mortos). UnitReaction/UnitCanAttack/UnitSelectionColor da UI usam a mesma reação. Joystick
  flutuante: invisível até arrastar o dedo na metade esquerda livre da tela (fora da UI), e
  ancorado onde o dedo encostou; toque rápido continua selecionando.
- v0.81: efeitos dos modelos no mundo — texturas animadas (água, cachoeiras, runas, cor/alpha
  animados) em todos os doodads e partículas (fogo de tochas e fogueiras, fumaça, brilhos) nos
  doodads até 70 jardas, texturas de partícula compartilhadas. Bainha sincronizada com o
  servidor (CMSG_SETSHEATHED): arma na mão em combate, guardada alguns segundos depois.
- v0.82: botão "Exportar arquivos de teste" no log (toque no "UI ok ⓘ"): grava em zips de
  ~25 MB (pasta teste-port ao lado da Data) os arquivos do jogo que esta sessão abriu (modelos,
  texturas, DBCs, UI, mapas), sem sons e músicas, para reproduzir o que o celular mostra.
- v0.83 (testada com os arquivos do celular num simulador de desktop): retrato 3D do jogador e
  do alvo com a câmera de retrato do próprio modelo (M2Camera tipo 0), sem recorte de
  scissor da UI. O "X vermelho" no retrato era o ícone de "não pronto" do ready check: a função
  GetReadyCheckStatus falsa respondia "notready". Desempenho: corte por frustum de blocos de
  terreno, doodads, WMOs e grupos de WMO, água em buffer estático (CPU do mundo ~50 → ~25 ms
  no simulador). Exportação inclui DBCs grandes (Spell.dbc).
- Interface simples (quando a original está desligada): quadro do jogador (vida/mana/fúria/energia), toque em um NPC/jogador seleciona o
  alvo (quadro de alvo + CMSG_SET_SELECTION) e nomes sobre as unidades próximas.
- Movimento enviado ao servidor (start/stop/jump/land/facing/heartbeat), time sync, ping,
  teleporte (`.tele`, `.go`) e troca de mapa.

## Ainda não existe
Na interface original: recarga (cooldown) e barra de lançamento, auras/buffs, macros,
vendedores/treinadores/correio/leilão, talentos com dados, pontos no minimapa, provador
trocando itens, sons da UI. Também: animações de combate e efeitos de magia, água, céu/névoa/iluminação originais, partículas, sons, vídeos e personalização paga.
Warden não é executado: use um servidor sem Warden (`local-server/`).

## Desempenho
Terreno, WMOs e doodads ficam em buffers fixos na GPU; doodads estáticos são esfolados uma
vez; camadas de textura do chão só perto do jogador; o mundo renderiza a 80% da resolução.

## Compilar o APK
- Android Studio ou linha de comando com JDK 17, SDK 35, NDK 27.2.12479018 e CMake 3.22.1:
  `cd android && ./gradlew assembleDebug` → `app/build/outputs/apk/debug/app-debug.apk`.
- Ou envie o repositório ao GitHub: o workflow `.github/workflows/android-apk.yml`
  gera o APK e deixa para download em Actions → artefato `wow-android-arm64-apk`.

## Usar no celular
1. Copie a pasta do cliente 3.3.5a (com `Data/` e os MPQs) para o celular,
   por exemplo `Download/WoW/Data`.
2. Abra o app → Configuração → informe a pasta Data → Permitir arquivos.
3. Ajuste o `realmlist.wtf` para o IP do seu servidor e faça login.

## Código
- `app/` — Activity, telas Glue (Java) e controles de toque.
- `../Tools/AndroidPort/` — biblioteca nativa `libwow_android.so`: MPQ/SFile,
  rede (Grunt/SRP, mundo, presença no mundo, criar/apagar personagem), DBCs, regras de
  personalização, cenas Glue 3D, terreno, modelos M2, GLES, eventos.
- `local-server/` — servidor AzerothCore local sem Warden para testar.
