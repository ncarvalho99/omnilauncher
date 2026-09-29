# SwitchU 2.6.5

Maintenance and stability release resolving power-menu reboot on Atmosphere, improving home grid slot handling, migrating SteamGridDB options to a dedicated SwitchU Menu tab, and preparing the update bridge to OmniLauncher.

## English

### Power and Reboot Fixes
- **Clean Atmosphere Payload Reboot**: Replaced raw `spsmShutdown` power-down calls with `appletStartRebootSequence()`, allowing Atmosphere's `bpc-mitm` service to safely arm `sdmc:/atmosphere/reboot_payload.bin` into IRAM before power cycling. Eliminates black screen drops into RCM during power-menu reboots on Erista consoles with AutoRCM.
- **Power Sequence Safety**: Recovered from failed SD card commits and power requests without freezing the daemon.

### Grid and Interface Fixes
- **Free Slot Movement Across Grid**: Dynamic streamer reallocation allows placing icons in any empty slot on any page without requiring sequential placement.
- **Ghost Entry Cleanup**: Pruned 0-ID entries from folders and fixed root folder continuation cell navigation.
- **Touchscreen Power Button & Sidebar Activation**: Direct touch taps on the sidebar and power icon now immediately open dialogs and applets reliably.

### Customization and SteamGridDB
- **Dedicated SteamGridDB Tab**: Moved all SteamGridDB artwork options (artwork toggle, background artwork opacity slider, API key, and bulk artwork scan) into a dedicated SteamGridDB tab in the SwitchU Menu.
- **SteamGridDB Background Opacity Adjustment**: Allows adjusting game background artwork opacity from 0% to 100% with real-time preview.
- **Cleaned System Settings**: System Settings is now focused strictly on console preferences.

### Future-Ready Updates
- **OmniLauncher Migration Path**: Forward-compatible update engine bridges SwitchU 2.6.5 to OmniLauncher 1.0.0+ when released.

## Portugues

### Correcoes de Energia e Reinicializacao
- **Reinicializacao Limpa de Payload no Atmosphere**: Substituidas chamadas diretas de desligamento `spsmShutdown` por `appletStartRebootSequence()`, permitindo que o servico `bpc-mitm` do Atmosphere carregue com seguranca `sdmc:/atmosphere/reboot_payload.bin` na IRAM antes de reiniciar. Elimina a tela preta com queda no modo RCM ao reiniciar pelo menu de energia em consoles Erista com AutoRCM.
- **Seguranca na Sequencia de Energia**: Tratamento resiliente caso a gravacao no cartao SD ou chamada de energia falhem, evitando travamento do daemon.

### Grade e Interface
- **Movimentacao Livre de Icones na Grade**: Realocacao dinamica do streamer permite mover e posicionar jogos em qualquer espaco vazio, sem exigir posicoes sequenciais.
- **Limpeza de Fantasmas**: Remocao de entradas com ID zero em pastas e correcao da navegacao por celulas de continuacao.
- **Ativacao por Toque na Barra Lateral e Botao de Energia**: Toques diretos na tela na barra lateral e no icone de energia agora abrem imediatamente as opcoes e applets.

### Personalizacao e SteamGridDB
- **Aba Dedicada do SteamGridDB**: Todas as opcoes do SteamGridDB (ativar arte, controle deslizante de opacidade, chave de API e busca de artes ausentes) agora ficam em uma aba dedicada no Menu SwitchU.
- **Ajuste de Opacidade da Arte de Fundo SteamGridDB**: Controle de opacidade da arte de fundo de 0% a 100% com indicador em tempo real.
- **Configuracoes do Sistema Mais Limpas**: Menu de Ajustes agora focado exclusivamente em opcoes do console.

### Atualizacoes Futuras
- **Ponte de Migracao para o OmniLauncher**: Mecanismo de atualizacao preparado para migrar do SwitchU 2.6.5 para o OmniLauncher 1.0.0+ assim que publicado.


# SwitchU 2.6.4

Major feature release introducing **Phase 6.1: YouTube Music Store & In-Console Downloader**, the **Multimedia Center (`Central Multimídia`)** with live audio visualizer and hardware volume synchronization, custom soundtrack BGM management, and virtual keyboard touch isolation.

## English

### 🎵 YouTube Music Store & In-Console Downloader (BGM Shop)
- **New Music Tab in ThemeShop**: Added dedicated "Music (YouTube)" tab alongside Theme tabs.
- **Installed Music Library View**: By default, displays all local custom soundtracks installed on `sdmc:/config/SwitchU/music/`, showing track names, file sizes, and total storage space in the header (e.g. `2 songs - 18.3 MB on the SD card`).
- **Direct YouTube Search**: Press Search (or X) to search YouTube for any video, soundtrack, or artist, rendering authentic 16:9 video thumbnails and duration badges in a clean 2x2 card grid.
- **Pre-Download Size Estimation**: Accurately calculates estimated SD card storage before downloading (e.g., `Duration: 3:21 • Est. Size: ~4.8 MB`) based on duration and 192kbps stereo MP3 encoding.
- **True 0% – 100% Progress Bar**: Displays a real-time progress dialog during audio conversion and transfer with downloaded MB counter, smoothly reaching 100% on completion.
- **Automatic Thumbnail Downloads**: Video thumbnails are automatically saved alongside the audio as `<Title>.jpg`, displaying authentic artwork for your downloaded tracks in the shop and multimedia center.
- **Download Complete Modal Popup**: When a download finishes, an interactive dialog confirms the installation and offers immediate playback with `[ Play Now ]` or `[ OK ]`.
- **In-Shop Track Deletion**: Delete downloaded songs directly from the detail sheet (`[ Delete Track ]`) with immediate SD cleanup and playlist synchronization.
- **Clean ASCII Filename Sanitizer**: Automatically cleans non-ASCII symbols, emojis, and musical note characters (`♫`) to safe FAT32 filenames, preventing filesystem errors on Nintendo Switch.

### 🎛️ Multimedia Center (`Central Multimídia`)
- **HUD Quick Launcher**: Opened via top HUD button (`media_center.png`) with pulsing active playback glow or shoulder shortcuts.
- **Live Visualizer & Vinyl Graphic**: Real-time animated 4-band audio equalizer visualizer bars over a spinning vinyl record graphic.
- **Hardware Master Volume Sync**: Direct integration with Horizon OS `audctl` (`audctlGetActiveOutputTarget` & `audctlGetTargetVolume`), dynamically synchronizing on-screen volume with the console's physical Volume +/- buttons in real time.
- **Playlist Quick Delete**: When browsing the playlist in Row 2, press **`X`** to instantly delete the track from SD card with real-time list updating and contextual legend (`A: Play • X: Delete`).
- **Minus Button Isolation**: Pressing `-` (Minus) inside the Multimedia Center is strictly isolated, preventing background layout toggling.
- **Marquee Text Scrolling**: Automatic ping-pong scrolling for long song titles and audio mode descriptions.
- **Audio Precedence Modes**: Cycle between `Custom SD First`, `Theme Music First`, `Custom SD Only`, and `Theme & Preset Only`.

### 🛡️ Input Focus & Security
- **Touch Event Isolation**: While the on-screen keyboard (`TextEntryScreen`) or modal dialogs are active, touch inputs are consumed exclusively by the active layer, completely preventing touches from leaking through to background tabs and buttons.
- **Backend Architecture & Hardening**: Deployed hardened backend service (`switchu-ytdl.service`) with constant-time client key authentication (`X-SwitchU-Key`), strict regex input validation, SSRF domain whitelisting, concurrency limits, and live MP3 proxy streaming.

## Português (Brasil)

### 🎵 Loja de Músicas & Downloader do YouTube
- **Nova Aba de Músicas na Loja**: Adicionada a aba "Músicas (YouTube)" dentro da ThemeShop.
- **Biblioteca de Músicas Instaladas**: Por padrão, exibe todas as faixas locais salvas em `sdmc:/config/SwitchU/music/`, com tamanho dos arquivos e espaço total ocupado no cabeçalho (ex: `2 músicas - 18,3 MB no cartão SD`).
- **Busca Direta no YouTube**: Pressione Buscar (ou X) para procurar qualquer música, trilha sonora ou artista, exibindo miniaturas 16:9 em alta resolução e duração das faixas.
- **Estimativa de Tamanho Pré-Download**: Calcula com precisão o espaço necessário no cartão SD antes de baixar (ex: `Duração: 3:21 • Tam. Est.: ~4.8 MB`).
- **Barra de Progresso Real (0% – 100%)**: Exibe barra de progresso contínua com contagem de MBs baixados e avanço visual suave até a conclusão.
- **Download Automático de Miniaturas**: Salva a miniatura do vídeo junto com o áudio (`<Nome>.jpg`), exibindo a capa autêntica na biblioteca da loja e na central.
- **Janela de Confirmação Pós-Download**: Ao concluir o download, uma janela modal confirma o sucesso e permite iniciar a reprodução imediatamente com `[ Tocar Agora ]` ou `[ OK ]`.
- **Exclusão de Faixas na Loja**: Exclua músicas baixadas diretamente na janela de detalhes (`[ Excluir Música ]`) com atualização imediata da lista.
- **Sanitização de Nomes FAT32**: Limpa automaticamente caracteres especiais, emojis e notas musicais (`♫`), garantindo compatibilidade total com o sistema de arquivos do Switch.

### 🎛️ Central Multimídia
- **Acesso Rápido no Topo**: Acesse pelo botão no HUD superior (`media_center.png`) com animação pulsante durante a reprodução.
- **Equalizador Visual & Disco de Vinil**: Barras de equalização de áudio em 4 bandas animadas em tempo real sobre gráfico de vinil.
- **Sincronização de Volume de Hardware**: Integração direta com o serviço `audctl` do Horizon OS, acompanhando e ajustando o volume físico do console em tempo real.
- **Atalho de Exclusão Rápida**: Na lista de faixas (Linha 2), pressione **`X`** para excluir a faixa e miniatura do cartão SD instantaneamente com legenda no rodapé (`A: Tocar • X: Excluir`).
- **Isolamento do Botão Menos (-)**: Pressionar `-` dentro da Central Multimídia não altera mais o layout da tela inicial em segundo plano.
- **Texto Deslizante (Marquee)**: Rolagem horizontal suave para títulos longos e descrições de modos de áudio.
- **4 Modos de Precedência de Áudio**: `Músicas do SD Primeiro`, `Músicas do Tema Primeiro`, `Somente Músicas do SD` e `Somente Tema e Padrão`.

### 🛡️ Isolamento de Toque & Segurança
- **Isolamento de Entrada Touch**: Ao digitar no teclado virtual (`TextEntryScreen`), os toques são consumidos exclusivamente pelo teclado, impedindo qualquer clique acidental nas abas ou botões em segundo plano.
- **Serviço de Backend Seguro**: Serviço backend (`switchu-ytdl.service`) com autenticação por chave de cliente em tempo constante (`X-SwitchU-Key`), validação estrita por regex, proteção contra SSRF e streaming direto de MP3.

# SwitchU 2.6.3

Targeted fix release addressing user-reported issues with game update title name resolution (Super Mario Bros. Wonder), WaraWara Plaza Mii avatar rendering and "no name" labels, and initial focus bounds on new game detected modal dialogs.

## English

### Title Name Resolution & Updates
- **Super Mario Bros. Wonder Name Display**: Fixed an issue where Super Mario Bros. Wonder (and games with modern update title blocks) displayed raw numeric Title IDs (`010015100B514000`) instead of the game's actual title name.
- **Enhanced NACP Decompression**: Upgraded `decompressNacpTitles` in the control cache to handle modern Deflate-compressed NACP title formats across variable buffer sizes and multiple windowBits modes (raw Deflate, zlib headers, auto-detect), verifying UTF-8 name validity.
- **Expanded Language Mapping**: Extended system language table to 18 entries, ensuring Brazilian Portuguese (`SetLanguage_PTBR = 17`) and other localized variants correctly resolve preferred language strings.
- **Built-in Title ID Fallback**: Added robust title resolution fallbacks for major first-party and popular Switch titles so that games with empty update NACP strings always display their authentic titles on the grid and in SteamGridDB searches.
- **Cache Logging & Flushing**: Added explicit log flushing in the daemon control-cache worker to ensure all title discovery and metadata caching operations are written to disk.

### WaraWara Plaza & Miis
- **Restored Bundled Guest Avatars**: Fixed asset packaging in the toolchain to ensure all 16 bundled high-resolution guest Mii avatar textures (`guest_01.png` – `guest_16.png`) and `plaza_dialogues.json` are installed into `sdmc:/switch/SwitchU/`.
- **Friendly Mii Names**: System Mii database records with default or empty names (`"no name"`, `"Mii"`, or blank) are now assigned distinct friendly guest names on the Plaza rather than showing `"no name"`.
- **Database Mii Disambiguation**: Resolved duplicate name handling that previously discarded valid database Miis, allowing all available system Miis to be placed across community pedestals with assigned head textures and favorite shirt colors.
- **Plaza Community Dialogues**: Restored authentic community game tips and Miiverse dialogue speech bubbles.

### Modal Dialogs & Focus
- **Button-Scoped Focus Rect**: Fixed `OverlayDialog` focus rect calculation so the focus target accurately matches the active button rather than inheriting the entire 1280x720 screen bounding box.
- **Dialog Pre-Layout**: Added immediate layout calculation on dialog show so button coordinates are positioned prior to cursor animation, ensuring the selection cursor frames the primary action button ("Download" / "Baixar") on the very first frame.
- **Global Cursor Suppression**: Suppressed the main menu global selection ring when modal dialogs, progress dialogs, or user selectors are active, eliminating unwanted screen-wide focus framing.

## Português (Brasil)

### Resolução de Nomes e Atualizações
- **Exibição do Nome de Super Mario Bros. Wonder**: Corrigida a exibição do Title ID numérico (`010015100B514000`) no lugar do nome oficial do jogo ao instalar atualizações.
- **Descompressão Aprimorada de NACP**: Suporte aprimorado no `control_cache` para descompressão de blocos de títulos NACP modernos em formato Deflate em múltiplos modos, validando nomes em UTF-8.
- **Mapeamento Completo de Idiomas**: Tabela de idiomas expandida para 18 entradas, garantindo resolução correta para Português Brasileiro (`SetLanguage_PTBR = 17`) e demais variações regionais.
- **Resolução de Títulos Conhecidos**: Adicionada tabela de mapeamento para grandes títulos da Nintendo, garantindo que atualizações com NACP sem strings de idiomas continuem exibindo seus nomes corretos na grade e no SteamGridDB.
- **Gravação em Disco no Daemon**: Adicionada gravação forçada (`flush`) no worker de cache de controle do daemon para persistência imediata dos registros de metadados no cartão SD.

### WaraWara Plaza e Miis
- **Avatares de Convidados Restaurados**: Corrigido o instalador da toolchain para incluir os 16 avatares de convidados em alta resolução (`guest_01.png` – `guest_16.png`) e `plaza_dialogues.json` na pasta `sdmc:/switch/SwitchU/`.
- **Nomes Amigáveis para Miis**: Miis do banco de dados do sistema que possuíam nomes padrão ou vazios (`"no name"`, `"Mii"`) agora recebem nomes variados e amigáveis na praça.
- **Desambiguação de Miis do Banco**: Corrigido o descarte de Miis com nomes duplicados, permitindo que todos os Miis cadastrados no console apareçam na praça com texturas faciais e camisetas coloridas variadas.
- **Diálogos do Miiverse**: Restaurados os balões de fala com dicas autênticas de jogos e postagens do Miiverse na praça.

### Janelas Modais e Foco
- **Retângulo de Foco nos Botões**: Corrigido o `focusRect` do `OverlayDialog` para corresponder exatamente ao botão selecionado em vez de cobrir a tela inteira de 1280x720.
- **Pré-Layout da Janela Modal**: Adicionado cálculo de layout imediato ao exibir a janela para que as coordenadas dos botões estejam prontas antes de posicionar o cursor, selecionando diretamente o botão principal ("Baixar" / "Download") no primeiro quadro.
- **Ocultação do Cursor Global**: O anel de seleção do menu principal agora é ocultado enquanto caixas de diálogo, telas de progresso ou seletor de usuários estiverem ativos.

# SwitchU 2.6.2

Stability, ergonomics, and security update resolving user-reported issues with folder drag-and-drop icon textures, grid slot reachability, page deletion, single Joy-Con controls, and controller reordering, while introducing official client service authentication, automatic NTP clock sync, dynamic SteamGridDB artwork detection, and authentic Wii U Plaza branding.

## English

### Controllers & Input
- **Sideways Joy-Con & 8-Player Support**: Configured standard 8-player input across all connected controllers so the Home menu responds to whichever controller is in the player's hands. Lone Joy-Cons are now set to horizontal hold orientation, enabling standard A/B/X/Y confirmation and navigation on a single Joy-Con (including left Joy-Con). Added state logging for connected controllers.
- **Change Grip/Order Reassignment**: When launching the Change Grip/Order applet, current controller connections are no longer locked in (`enable_take_over_connection = 0`), allowing Player 1 to be reassigned to any controller rather than remaining stuck on the opening gamepad.
- **Continuous D-Pad Page Flipping**: Holding D-pad Left or Right continuously now triggers automatic wrap-around page transitions (infinite page turn) while keeping focus on the game grid, while tapping one-by-one continues to navigate into the launcher sidebar menus as expected.
- **Hold-to-Delete Page (ZL)**: Remapped page deletion from `X` to `ZL` with a matching hold animation featuring a minus (`-`) icon and filling circular progress arc, pairing naturally with `ZR` for page creation. When ZL is held on an empty page, the deletion animation runs directly on the current page without jumping back, while a quick tap on ZL continues to navigate to the previous page.
- **Keyboard Erase Remap**: Swapped `B` to backspace/erase and `X` to cancel/close in the on-screen keyboard, matching standard Switch game keyboard conventions, with updated localizations across all 8 languages.

### Folder & Grid Improvements
- **Drag-and-Drop Texture Isolation**: Fixed an issue where dragging an icon into an open folder temporarily replaced the dragged icon's image with the folder's first game icon. Independent textures and custom artwork are now preserved across folder boundaries.
- **Unrestricted Grid Placement**: Fixed empty slot placement constraints when moving folders or icons. Edit-mode D-pad navigation now skips continuation cells, allowing smooth traversal across multi-cell widgets, and grid layout slots dynamically accommodate the full visible grid.
- **Folder Move Persistence**: Fixed internal folder grid moves not persisting changes to disk.

### Title Identification & Network Services
- **Game Update Name Recognition**: Resolved an issue where games with updates installed (such as Super Mario Bros. Wonder) fell back to displaying numeric Title IDs instead of localized names due to compressed NACP language title blocks.
- **SteamGridDB Real Title Detection & Auto-Prompt**: Added an automatic check on boot, restart, or when a homebrew port is marked as a game that prompts users with a centered modal dialog displaying the resolved game name (instead of the title ID hex code) asking if they would like to download covers and artwork from SteamGridDB.
- **Modal Dialog Liquid Glass & Dimming Scrim**: Enhanced `OverlayDialog` with a translucent backdrop dimming scrim and authentic liquid glass frosted blur matching the default SwitchU window style.
- **Official Client Key Gate**: Embedded a compile-time authentication key (`X-SwitchU-Key`) in official SwitchU client builds, protecting project servers (`gallery.nclabs.dev`, `switchu-api.nclabs.dev`, `themes.nclabs.dev`) from unauthorized third-party forks.
- **Automatic Internet Clock Sync**: Integrated background NTP synchronization on startup and network connection so the console time automatically stays accurate without requiring manual configuration.
- **Wii U Plaza Icon**: Replaced the procedural top header icon with the authentic Wii U logo icon (`warawara_u.png`).

## Português

### Controles e Entrada
- **Joy-Con Individual na Horizontal e Suporte a 8 Jogadores**: Configurada a leitura padrão de até 8 jogadores para que o menu inicial responda a qualquer controle que esteja nas mãos do jogador. Joy-Cons avulsos agora são configurados automaticamente na orientação horizontal, habilitando os botões de ação A/B/X/Y mesmo no Joy-Con esquerdo sozinho. Adicionado registro de diagnóstico do estado dos controles.
- **Reatribuição no Mudar a Ordem/Pegada**: Ao abrir o menu de Mudar a Ordem/Pegada dos controles, as conexões atuais não são mais fixadas (`enable_take_over_connection = 0`), permitindo que o Jogador 1 seja reatribuído a qualquer controle em vez de ficar preso no controle que abriu o aplicativo.
- **Mudança Contínua de Páginas com D-Pad**: Segurar o D-Pad para a esquerda ou direita agora troca de página continuamente com giro infinito mantendo o foco na grade, enquanto toques únicos continuam navegando para a barra lateral.
- **Segurar para Excluir Página (ZL)**: Remapeada a exclusão de páginas de `X` para `ZL` com animação de progresso circular e ícone de menos (`-`). Ao segurar ZL numa página vazia, a animação é exibida diretamente na página atual sem voltar antes; toques rápidos no ZL continuam voltando uma página.
- **Remapeamento do Teclado Virtual**: Invertidos os botões `B` para apagar caractere e `X` para cancelar/fechar o teclado virtual, seguindo o padrão oficial do console, com textos localizados em todos os 8 idiomas.

### Pastas e Grade
- **Isolamento de Texturas no Arrastar e Soltar**: Corrigida a substituição temporária do ícone de um jogo arrastado pelo primeiro ícone de uma pasta aberta.
- **Posicionamento sem Bloqueios na Grade**: Corrigidos os limites de movimentação de pastas e ícones sobre espaços vazios adjacentes a widgets de múltiplas células.
- **Persistência de Movimentação em Pastas**: Corrigido o salvamento em disco de reorganizações de ícones dentro de pastas.

### Reconhecimento de Títulos e Serviços de Rede
- **Nomes de Jogos Atualizados**: Corrigida a exibição de códigos numéricos em vez dos nomes localizados em jogos com atualizações instaladas (como Super Mario Bros. Wonder) através da descompressão zlib de blocos NACP.
- **Detecção Real de Jogos e Aviso do SteamGridDB**: Diálogo de download do SteamGridDB agora exibe o nome real do jogo e abre automaticamente ao marcar um port como jogo.
- **Vidro Líquido e Desfoque nos Diálogos**: Janelas modais agora contam com escurecimento translúcido e desfoque idênticos às demais telas do SwitchU.
- **Chave de Autenticação do Cliente Oficial**: Compilação oficial agora injeta chave de acesso (`X-SwitchU-Key`) aos serviços `nclabs.dev`.
- **Sincronização Automática de Relógio via Internet**: Ajuste de hora via NTP em segundo plano na inicialização e ao conectar à internet.
- **Ícone do Wii U na WaraWara Plaza**: Adicionado o logotipo autêntico do Wii U (`warawara_u.png`) no topo da tela.

# SwitchU 2.6.1

Maintenance release bringing dynamic page sizing to the Home menu and folders, seamless icon drag-and-drop into folders, dedicated Plaza icon caching, and critical UI/focus polish.

## English

### Dynamic Pages & Page Management
- **Dynamic Launcher Pages**: Added a toggle in Theme Shop > Options to dynamically size Home launcher pages to fit installed games and folders, rather than keeping 8 fixed pages.
- **Hold-to-Create Animation**: Holding `ZR` (or touch-holding the `+` button) on the last page of the Home menu or open folder smoothly fills a radial progress arc; the new page is created once the animation reaches 100%. Releasing early cleanly cancels without creating a page.
- **Empty Page Deletion**: Pressing `(X) Delete page` on any empty page removes it from the Home grid or folder, accompanied by confirmation audio and screen transition.
- **Folder Page Cleanup**: Added "Delete empty pages" in Folder Options (Management tab) to quickly trim trailing empty pages.

### Folders & Drag-and-Drop Polish
- **Drag & Drop into Folders**: Fixed moving icons with `Y` into and out of folders. The ghost icon texture, scale, and placement anchors are preserved across folder boundaries, fixing the issue where icons collapsed into a dot in the top-left corner.
- **Streamlined Dossier**: Removed the redundant "Add to folder" tab from the Game Details (`+`) screen, keeping folder organization purely drag-and-drop.

### WaraWara Plaza & Performance
- **Dedicated Plaza Icon Cache**: Fixed community pedestal icon flickering and texture eviction thrashing in WaraWara Plaza by decoupling Plaza pedestal textures from the Home grid `IconStreamer`.
- **Large Icon Loading**: Expanded the `control_cache` buffer limit from 256 KB to 1 MB, ensuring large homebrew and port icons load reliably without falling back to letter tiles.
- **Ambient Miis Restricted to Plaza**: Removed wandering Miis from the Home menu background, keeping them exclusively in WaraWara Plaza.

### UI & Shortcut Polish
- **Theme Shop Selection Clarity**: Enhanced card selection borders, glow halos, and action button focus outlines for high visibility across all themes.
- **Consolidated Port Renaming**: Removed duplicate "Rename" button from Game Details when viewing ports, keeping it centralized under "Port options".
- **Shortcut Safety**: Audited `X` button actions across all screens to ensure strict non-overlapping behavior (`(X)` deletes empty pages, closes running games, or removes games from folders when applicable).
- **Localization**: Full translation updates across all 8 supported languages.

## Português (Brasil)

### Páginas Dinâmicas e Gerenciamento
- **Páginas Dinâmicas no Launcher**: Adicionada opção na Loja de Temas > Opções para ajustar a quantidade de páginas dinamicamente de acordo com os jogos e pastas instalados, em vez de manter 8 páginas fixas.
- **Animação Segurar para Criar**: Segurar `ZR` (ou segurar no botão `+` pelo toque) na última página da tela inicial ou de uma pasta preenche suavemente um anel de progresso; a nova página é criada quando a animação atinge 100%. Soltar antes cancela a ação.
- **Exclusão de Página Vazia**: Pressionar `(X) Excluir página` em qualquer página vazia a remove da grade inicial ou pasta com retorno sonoro e transição de tela.
- **Limpeza de Páginas em Pastas**: Adicionada ação "Excluir páginas vazias" nas Opções da Pasta (aba Gerenciamento).

### Pastas e Arrastar e Soltar
- **Arrastar e Soltar em Pastas**: Corrigida movimentação de ícones com `Y` para dentro e fora de pastas. A textura e proporções do ícone são mantidas nas transições, evitando o ponto fixo no canto superior esquerdo.
- **Dossiê Simplificado**: Removida a aba redundante "Adicionar à pasta" do dossiê de detalhes (`+`), mantendo a organização de pastas por arrastar e soltar.

### WaraWara Plaza e Desempenho
- **Cache Dedicado na Plaza**: Corrigido piscar de ícones e descarte de texturas na WaraWara Plaza desacoplando as texturas dos pedestais do streamer da grade inicial.
- **Carregamento de Ícones Grandes**: Aumentado o limite de leitura do cache de controle de 256 KB para 1 MB, permitindo que ícones grandes de homebrews e ports carreguem perfeitamente sem ícones de letras.
- **Miis Exclusivos na Plaza**: Removidos os Miis do fundo da tela inicial, mantendo-os exclusivamente na WaraWara Plaza.

### Interface e Atalhos
- **Clareza na Loja de Temas**: Maior contraste e bordas iluminadas nos cartões de temas e botões de ação para fácil visualização em qualquer tema.
- **Renomear Ports Centralizado**: Removido botão duplicado "Renomear" nos detalhes de ports, mantendo-o dentro de "Opções do port".
- **Auditoria de Atalhos**: Botão `X` revisado para comportamento não sobreposto (exclui páginas vazias, fecha jogos suspensos ou remove de pastas conforme o contexto).
- **Localização**: Traduções completas atualizadas para os 8 idiomas suportados.

# SwitchU 2.6.0

Major release introducing the initial implementation of the authentic Wii U WaraWara Plaza, Most Played sort mode with playtime badges, custom title renaming in the dossier, minimalist dossier layout refinements, and integrated downstream system/storage fixes.

## English

> **⚠️ WaraWara Plaza Notice**: This is an initial implementation of the WaraWara Plaza. It currently runs in offline mode using local community posts and system Mii avatars. Online network integration (Miiverse / Pretendo community services) along with further animations and polish will be introduced in future updates.

### WaraWara Plaza (Phase 5)

- Added the authentic Wii U WaraWara Plaza accessible via the screen swap button in the top bar.
- 3D-perspective radial plaza featuring authentic blue pedestals, carousel rotation, and ambient wandering Miis with dynamic pacing and idle behaviors.
- Speech bubbles displaying community commentary, tips, and thoughts around installed game pedestals.
- Interactive Wii U pointer hand cursor supporting analog stick, touch, and motion navigation.
- Smooth camera zoom controls (`ZL` / `ZR`) and extended panning with the Right Stick.
- Direct game launching and Software Information dossier access directly from community pedestals.
- Seamless, locked 60 FPS transitions between the Home Grid and WaraWara Plaza.

### Most Played Sort Mode & Playtime Badges

- Added "Most played" as sort mode 4 in the `R` shoulder button cycle, sorting games by total play duration.
- Compact playtime badges (e.g. "8 h", "28 min") rendered directly on game icons when in Most played mode.
- Asynchronous playtime queries via `pdm:qry` that cache durations to keep navigation smooth and instantaneous.

### Game Details Dossier Polish & Custom Renaming

- In-dossier game renaming (`Rename`): customize titles for games and homebrew; leaving the name empty restores the original NACP catalogue title.
- Streamlined left rail to 6 clean options, preventing vertical overflow and ensuring Version, Play time, and Mods are always visible.
- Removed "Mark as game port" from official Nintendo Switch titles.
- Nested "Restore default" directly inside the "Active artwork" modal dialog, allowing one-step restoration of custom covers and backgrounds.
- Consolidated game port management ("Edit search title" and "Unmark port") into a single "Port options" dialog.
- Fixed vertical navigation clamp so controller focus reaches all actions smoothly down to Delete software.

### Storage, Grid Synchronization & System Settings

- Fixed software deletion grid sync: automatic icon streamer title remapping in projected sort modes prevents stuck loading spinners and stale selection pills.
- Full SD footprint deletion removing LayeredFS contents and community port folders with real-time progress.
- Settings > System: live memory pool breakdown and active sysmodule enumeration, plus in-menu nickname editing.
- Settings > Display & Internet: live DNS display, USB 3.0 restart notification toast, complete resolution picker, and reliable brightness persistence across reboots.
- Automatic log rotation (`menu-*.log`, `daemon-*.log`) and "Save logs for copying" in System settings for simple troubleshooting.

---

## Português

> **⚠️ Aviso sobre a WaraWara Plaza**: Esta é uma implementação inicial da WaraWara Plaza. Atualmente ela funciona em modo offline utilizando postagens comunitárias locais e avatares Mii do sistema. A integração online (serviços de comunidade Miiverse / Pretendo) e refinamentos visuais adicionais serão introduzidos em atualizações futuras.

### WaraWara Plaza (Fase 5)

- Adicionada a autêntica WaraWara Plaza do Wii U, acessível através do botão de troca de tela na barra superior.
- Praça radial em perspectiva 3D com pedestais azuis autênticos, rotação de carrossel e Miis caminhando com ritmo e comportamentos naturais.
- Balões de fala exibindo comentários da comunidade, dicas e pensamentos ao redor dos pedestais de jogos instalados.
- Cursor de mão apontadora autêntico do Wii U com suporte a analógico, toque e movimento.
- Controles suaves de zoom (`ZL` / `ZR`) e visão panorâmica com o analógico direito.
- Inicialização direta de jogos e acesso à ficha de informações a partir dos pedestais da praça.
- Transições fluidas e estáveis a 60 FPS entre a Grade Principal e a WaraWara Plaza.

### Modo de Ordenação Mais Jogados e Indicadores de Tempo

- Adicionado o modo "Mais jogados" como modo 4 no ciclo do botão `R`, ordenando títulos pelo tempo total de jogo.
- Indicadores compactos de tempo de jogo (ex.: "8 h", "28 min") exibidos nos ícones dos jogos no modo Mais jogados.
- Consultas assíncronas de tempo via `pdm:qry` com cache local para manter a navegação instantânea e sem travamentos.

### Ficha de Detalhes do Jogo e Renomeação Personalizada

- Renomeação de jogos na ficha (`Renomear`): personalize títulos de jogos e homebrews; deixar o campo vazio restaura o nome original do catálogo NACP.
- Menu lateral simplificado com 6 opções limpas, evitando sobreposição vertical e garantindo que Versão, Tempo de jogo e Mods estejam sempre visíveis.
- Removida a opção "Marcar como port de jogo" de títulos oficiais do Nintendo Switch.
- Opção "Restaurar padrão" integrada diretamente dentro da janela de "Arte ativa", permitindo restaurar capa e fundo personalizados em um só lugar.
- Consolidadas as ações de port ("Editar título de busca" e "Desmarcar port") em um único diálogo de "Opções de port".
- Corrigida a trava de navegação vertical para permitir alcançar todas as opções com o controle até "Excluir software".

### Gerenciamento de Armazenamento, Sincronização da Grade e Configurações

- Corrigida a sincronização da grade pós-exclusão: o remapeamento automático do streamer em modos de ordenação projetados evita ícones travados em carregamento e títulos desalinhados.
- Exclusão completa de arquivos no SD, removendo pastas de mods LayeredFS e ports com barra de progresso em tempo real.
- Configurações > Sistema: exibição detalhada dos pools de memória, lista de sysmodules ativos e edição de apelido no menu.
- Configurações > Tela e Internet: exibição de servidores DNS ativos, aviso de reinicialização para USB 3.0, seletor de resolução e persistência de brilho após reinicialização.
- Rotação automática de logs (`menu-*.log`, `daemon-*.log`) e opção "Salvar logs para cópia" em Configurações do Sistema.

---

# SwitchU 2.5.1 (Hotfix)

Hotfix restoring correct Game Details dossier layering when a title is opened from the Daily Activity Log.

## English

### Daily Activity Log

- Fixed the Software Information dossier rendering behind the still-active Activity Log after selecting a game from Daily Log, Monthly Log, or Software Library.
- The dossier is now explicitly raised to the top of the overlay stack whenever it opens, while preserving the existing `B` navigation back to the Activity Log.
- The regression was independent of the Phase 5 Mii avatar milestone: that commit only added avatar-manager initialization and state, while the missing overlay raise already existed in the 2.5.0 activity-to-dossier path.

---

## Português

### Registro de Atividades

- Corrigida a ficha de Informações do Software que era renderizada atrás do Registro de Atividades ainda ativo após selecionar um jogo no Registro Diário, Registro Mensal ou Biblioteca de Software.
- A ficha agora é explicitamente movida para o topo da pilha de overlays sempre que é aberta, preservando a navegação existente com `B` de volta ao Registro de Atividades.
- A regressão era independente do marco de avatares Mii da Fase 5: esse commit apenas adicionou inicialização e estado do gerenciador de avatares, enquanto a ausência da elevação do overlay já existia no caminho do Registro de Atividades para a ficha na versão 2.5.0.

---

# SwitchU 2.5.0

Feature release adding game favorites, a slide-out Quick Settings HUD, a Wii U-style Daily Activity Log, and an in-dossier Atmosphère cheat manager.

## English

Game favorites on the home grid, a Liquid Glass Quick Settings HUD with live hardware readouts, a complete Daily Activity Log built on real console play statistics, and a cheat manager that toggles Atmosphère cheats without an external overlay.

### Game favorites

- Mark any game as a favorite from the home menu with a Right Stick click (`RStick` / R3) on the focused icon, with audio and visual feedback (`Sfx::Activate` / `Sfx::ToggleOff`).
- Favorited games show a golden 5-point star medallion on the home grid icon and beside the title in the Software Information dossier.
- The grid view cycle (`R`) gained a `Favorites` mode that stably projects every favorited game to the front of the grid.
- Favorites persist in `config.json` under `favorites` (`AppConfig::favoriteTitleIds`), and the selected sort mode survives restarts.
- Added favorites strings and hints across all 8 supported languages.

### Quick Settings HUD

- Added a slide-out Quick Settings panel with cubic slide animation, opened with a Left Stick click (`LStick` / L3) or by tapping the status bar, and closed with `B`, `L3`, or a tap outside the panel.
- Live hardware card with battery charge and charging state (`psmGetBatteryChargePercentage` / `psmGetChargerType`) and real-time thermals (`tsGetTemperature`, `tcGetSkinTemperatureMilliC`).
- Sliders for screen brightness (`lblGetCurrentBrightnessSetting` / `lblSetCurrentBrightnessSetting`), BGM volume and sound-effect volume, with touch scrub and stick control.
- Pill switches for Airplane Mode (`nifm:s` system session IPC) and Wi-Fi (`setsysSetWirelessLanEnableFlag`), with a procedural vector Wi-Fi icon.
- Power options for Sleep, Reboot and Power Off as frosted glass buttons with authentic Switch glyphs; their confirmation dialogs now layer above the panel instead of behind it.
- The panel renders with the GPU Liquid Glass pipeline, so the wallpaper and icons genuinely blur and refract beneath it; the global selection cursor hides while the HUD is open and focus returns to the active icon on close.
- Added a Daily Activity Log entry point inside the panel, and localized every Quick Settings string across all 8 supported languages.

### Daily Activity Log

- Added a Wii U-style Activity Log built on the Horizon OS `pdm:qry` service (`pdmqryQueryPlayStatisticsByApplicationId`, `pdmqryGetAvailablePlayEventRange`, `pdmqryQueryAppletEvent`), opened from Quick Settings.
- **Daily Log**: date navigation, total play time, titles played, and proportional per-title bars with Wii U colour coding.
- **Monthly Log**: month navigation, active-day count, the classic daily distribution bar graph with a today indicator, and a scrollable monthly ranking.
- **Software Library**: all-time rankings with gold, silver and bronze medallions, play counts, average session length, and first and last played dates.
- Every ranked row shows the real game or homebrew icon beside a left-aligned ranking number.
- Homebrew ports resolve to real names and artwork through `sdmc:/switch/DBI/dbi.titles`, `control_cache` / `nsGetApplicationControlData`, and `sdmc:/switch/P2PNX/installed-icons/`, with alternate launcher IDs merged into one entry.
- CFW tools, homebrew menus and utility launchers are filtered out of every view so only games and game ports are ranked.
- Playtime accounting closes active sessions when a launcher or `qlaunch` takes focus and reconciles daily totals against lifetime statistics, so sleep and suspension no longer inflate the numbers.
- Statistics prefetch in a background worker and icons decode off the render thread, so the log opens without a stall and navigates at 60 FPS.
- Bidirectional navigation between the tab rail and the content list, `ZL` / `ZR` date stepping clamped to today, `Y` to jump back to today, and `A` to open the dossier with `B` returning to the log.

### Atmosphère cheat manager

- Added an in-dossier Cheats screen that lists and toggles Atmosphère cheats from `sdmc:/atmosphere/contents/<titleId>/cheats/` without launching an external overlay.
- Resolves uppercase and lowercase title directories and the `.switchu-disabled/cheats/` location, and switches between build IDs with `L` / `R` when a title has cheats for several versions.
- Parses standard `[Cheat Name]` and master `{Master Code}` headers, strips UTF-8 BOMs, and skips overlay directives (`--SectionStart:`, `--SectionEnd:`, `--DisableSubmenus`) and separator punctuation common in community cheat packs.
- Understands the EdiZon and Ultrahand `:ENABLED` suffix, stripping it from the displayed name.
- Toggle state is written to `toggles.txt` in Atmosphère's native dmnt format, matched case-insensitively for interoperability with dmnt, EdiZon, EdiZon-SE and Ultrahand-Overlay; the `<buildId>.txt` cheat files are never modified.
- Animated switch pills, master-code indicators, opcode line counts, a live opcode preview and a build summary such as `Build 1/3: 0/15 active`; `A` toggles one cheat, `X` toggles all, `B` returns to the dossier.
- An informative empty state names the exact SD card path when a title has no cheats installed.

<details>
<summary>Preview / Screenshots</summary>

![](./screenshots/35.jpg)
![](./screenshots/36.jpg)
![](./screenshots/37.jpg)
![](./screenshots/38.jpg)
![](./screenshots/39.jpg)
![](./screenshots/40.jpg)

</details>

---

## Português

Favoritos no menu principal, painel de Ajustes Rápidos em Liquid Glass com leitura de hardware em tempo real, um Registro de Atividades no estilo Wii U com estatísticas reais do console e um gerenciador de cheats do Atmosphère dentro da ficha do jogo.

### Jogos favoritos

- Marque qualquer jogo como favorito no menu principal pressionando o Analógico Direito (`RStick` / R3) sobre o ícone em foco, com retorno sonoro e visual (`Sfx::Activate` / `Sfx::ToggleOff`).
- Jogos favoritos exibem um medalhão de estrela dourada de 5 pontas no ícone da grade e ao lado do título na ficha de Informações do Software.
- O ciclo de visualização da grade (`R`) ganhou o modo `Favoritos`, que projeta de forma estável todos os favoritos para o início da grade.
- Os favoritos são salvos em `config.json` na chave `favorites` (`AppConfig::favoriteTitleIds`) e o modo de ordenação escolhido é preservado entre reinicializações.
- Adicionados textos e dicas de favoritos nos 8 idiomas suportados.

### Ajustes Rápidos (HUD)

- Adicionado painel deslizante de Ajustes Rápidos com animação cúbica, aberto com o clique do Analógico Esquerdo (`LStick` / L3) ou tocando na barra de status, e fechado com `B`, `L3` ou toque fora do painel.
- Cartão de hardware em tempo real com carga e estado de carregamento da bateria (`psmGetBatteryChargePercentage` / `psmGetChargerType`) e temperaturas reais (`tsGetTemperature`, `tcGetSkinTemperatureMilliC`).
- Controles deslizantes de brilho da tela (`lblGetCurrentBrightnessSetting` / `lblSetCurrentBrightnessSetting`), volume da música e volume dos efeitos sonoros, com arraste por toque e controle pelo analógico.
- Chaves para Modo Avião (IPC de sessão de sistema `nifm:s`) e Wi-Fi (`setsysSetWirelessLanEnableFlag`), com ícone vetorial de Wi-Fi desenhado proceduralmente.
- Opções de energia de Suspender, Reiniciar e Desligar como botões de vidro fosco com glifos autênticos do Switch; seus diálogos de confirmação agora aparecem acima do painel, e não atrás dele.
- O painel é renderizado pelo pipeline Liquid Glass na GPU, desfocando e refratando o papel de parede e os ícones; o cursor global é ocultado com o painel aberto e o foco retorna ao ícone ativo ao fechar.
- Adicionado o acesso ao Registro de Atividades dentro do painel, com todos os textos dos Ajustes Rápidos localizados nos 8 idiomas suportados.

### Registro de Atividades

- Adicionado o Registro de Atividades no estilo Wii U, construído sobre o serviço `pdm:qry` do Horizon OS (`pdmqryQueryPlayStatisticsByApplicationId`, `pdmqryGetAvailablePlayEventRange`, `pdmqryQueryAppletEvent`), acessível pelos Ajustes Rápidos.
- **Registro Diário**: navegação por data, tempo total de jogo, quantidade de títulos jogados e barras proporcionais por título com as cores do Wii U.
- **Registro Mensal**: navegação por mês, contagem de dias ativos, o clássico gráfico de distribuição diária com indicador do dia atual e ranking mensal com rolagem.
- **Biblioteca de Software**: ranking histórico com medalhões de ouro, prata e bronze, número de partidas, duração média de sessão e datas de primeira e última execução.
- Cada linha do ranking exibe o ícone real do jogo ou do homebrew ao lado do número de classificação alinhado à esquerda.
- Ports de homebrew passam a exibir nome e arte reais por meio de `sdmc:/switch/DBI/dbi.titles`, `control_cache` / `nsGetApplicationControlData` e `sdmc:/switch/P2PNX/installed-icons/`, unificando IDs alternativos de lançadores em uma única entrada.
- Ferramentas de CFW, menus de homebrew e lançadores utilitários são filtrados de todas as visões, de modo que apenas jogos e ports sejam classificados.
- A contabilização de tempo encerra sessões ativas quando um lançador ou o `qlaunch` recebe o foco e reconcilia os totais diários com as estatísticas vitalícias, evitando que suspensão e repouso inflem os números.
- As estatísticas são pré-carregadas em thread de segundo plano e os ícones são decodificados fora da thread de renderização, abrindo o registro sem travamentos e navegando a 60 FPS.
- Navegação bidirecional entre as abas e a lista de conteúdo, avanço de datas com `ZL` / `ZR` limitado ao dia atual, `Y` para voltar a hoje e `A` para abrir a ficha, com `B` retornando ao registro.

### Gerenciador de cheats do Atmosphère

- Adicionada a tela de Cheats dentro da ficha do jogo, que lista e ativa cheats do Atmosphère em `sdmc:/atmosphere/contents/<titleId>/cheats/` sem abrir um overlay externo.
- Resolve diretórios de título em maiúsculas e minúsculas e o local `.switchu-disabled/cheats/`, alternando entre build IDs com `L` / `R` quando o título possui cheats para várias versões.
- Interpreta cabeçalhos padrão `[Nome do Cheat]` e master `{Master Code}`, remove BOM UTF-8 e ignora diretivas de overlay (`--SectionStart:`, `--SectionEnd:`, `--DisableSubmenus`) e separadores comuns em pacotes de cheats da comunidade.
- Reconhece o sufixo `:ENABLED` do EdiZon e do Ultrahand, removendo-o do nome exibido.
- O estado das chaves é gravado em `toggles.txt` no formato nativo dmnt do Atmosphère, comparado sem diferenciar maiúsculas para interoperar com dmnt, EdiZon, EdiZon-SE e Ultrahand-Overlay; os arquivos `<buildId>.txt` nunca são modificados.
- Chaves animadas, indicadores de master code, contagem de linhas de opcode, pré-visualização de opcode e resumo de build como `Build 1/3: 0/15 ativos`; `A` alterna um cheat, `X` alterna todos e `B` volta para a ficha.
- Quando o título não possui cheats instalados, um estado vazio informativo indica o caminho exato no cartão SD.

---

# SwitchU 2.4.3 (Hotfix)

Hotfix release addressing manual date and time modification failures and adding standalone public SNTP network time synchronization.

## English

Fix for manual date and time modification, non-blocking public SNTP pool time synchronization, and multi-thread toast safety.

### Date and time management

- Fixed manual date/time modification in SwitchU daemon: resolved Horizon OS permission denial (`0x274` / `Time::PermissionDenied`) by directly configuring `NetworkSystemClock` (`time:s` cmd 1) and `LocalSystemClock` (`time:a` cmd 4) instead of relying on stock automatic correction toggles.
- Added public SNTP time synchronization client supporting `pool.ntp.org` pools (`0.pool.ntp.org` through `3.pool.ntp.org`) and fallbacks (`time.google.com`, `time.cloudflare.com`). Enables reliable network time synchronization on consoles where stock Nintendo telemetry is blocked by 90DNS or Atmosphère hosts.
- Added a dedicated "Synchronize Clock Now" action button in the System settings tab.
- Toggling "Synchronize Clock via Internet" now triggers immediate background SNTP query with on-screen toast feedback.
- Implemented background worker thread via libnx native Horizon `Thread` API pinned to Core 2, avoiding runtime aborts and preserving 60 FPS UI performance.
- Hardened toast message presentation (`TabbedOverlayScreen`) with thread-safe mutual exclusion for background worker notifications.

---

## Português

Correção na alteração manual de data e hora, sincronização de horário via pools SNTP públicos e segurança de threads para notificações toast.

### Gerenciamento de data e hora

- Corrigida a alteração manual de data e hora no daemon do SwitchU: solucionado o erro de permissão do Horizon OS (`0x274` / `Time::PermissionDenied`) através do acesso direto via IPC ao `NetworkSystemClock` (`time:s` cmd 1) e `LocalSystemClock` (`time:a` cmd 4).
- Adicionado cliente SNTP para sincronização de horário através dos pools públicos do `pool.ntp.org` (`0.pool.ntp.org` a `3.pool.ntp.org`) e servidores de contingência (`time.google.com`, `time.cloudflare.com`). Permite sincronizar a hora pela rede mesmo em consoles com bloqueio de telemetria da Nintendo via 90DNS ou hosts do Atmosphère.
- Adicionado botão de ação "Sincronizar relógio agora" na aba de Sistema das configurações.
- Ativar a opção "Sincronizar relógio pela Internet" agora dispara sincronização imediata em segundo plano com feedback em toast.
- Implementada execução em segundo plano utilizando threads nativas do Horizon OS (`Thread` da libnx) fixadas no Core 2, eliminando falhas de runtime e mantendo a interface fluida a 60 FPS.
- Protegida a exibição de notificações toast (`TabbedOverlayScreen`) com exclusão mútua (`mutex`) para despacho seguro a partir de threads secundárias.

---

# SwitchU 2.4.2

## English

Dedicated self-uninstall tab, full filesystem purge, GPU liquid glass styling for progress dialogs and sliders, docked wake responsiveness fixes, and tutorial localization corrections.

### Self-uninstall and recovery

- Added a dedicated bottom-anchored Uninstall tab to the SwitchU overlay with localized guidance across all 8 supported languages.
- Implemented a complete and clean SD card purge on self-uninstall: removing the sysmodule (`0100000000001000`), executables, downloaded themes, and configurations before rebooting into stock Nintendo qlaunch.
- Updated confirmation and information dialogues in all languages with explicit notices that the console reboots twice to finalize the removal.

### Visual design and materials

- Upgraded the system `ProgressDialog` to use the GPU liquid glass rendering pipeline with real-time backdrop capture, blur, and refraction, matching the dossier interface styling.
- Restyled slider and progress bar tracks across settings and dialogs to have a glassy translucent background with fine outlines so only the active progress percentage displays colored accent.

### Tutorial and localization

- Corrected hardcoded tutorial title strings so the tutorial header properly localizes in all languages (e.g. "Tutorial do SwitchU" in Portuguese) instead of displaying the French fallback.

### Performance and stability

- Removed redundant sleep requests when unlocking the screen, fixing delayed wake responsiveness while docked.
- Ensured proper NS service initialization during home screen software deletion routines.

---

## Português

Aba dedicada de desinstalação, limpeza completa de arquivos, visual de vidro líquido (liquid glass) para caixas de progresso e controles deslizantes, correção na resposta ao despertar no dock e correções de localização do tutorial.

### Desinstalação e recuperação

- Adicionada uma aba dedicada de Desinstalação fixada na base do menu SwitchU, com instruções localizadas em todos os 8 idiomas suportados.
- Implementada a limpeza completa e segura do cartão SD ao desinstalar: removendo a sysmodule (`0100000000001000`), executáveis, temas baixados e configurações antes de reiniciar no qlaunch original da Nintendo.
- Atualizados os diálogos informativos e de confirmação com aviso explícito de que o console reiniciará duas vezes para concluir a remoção.

### Design visual e materiais

- Atualizada a janela de progresso (`ProgressDialog`) para utilizar a pipeline de vidro líquido na GPU com captura de fundo em tempo real, desfoque e refração, alinhando-se ao visual das fichas de detalhes (dossiê).
- Redesenhadas as trilhas de barras de progresso e controles deslizantes para apresentar fundo translúcido e bordas finas de vidro, destacando a cor de destaque apenas na porcentagem preenchida.

### Tutorial e localização

- Corrigido o título da página do tutorial para que seja traduzido corretamente em todos os idiomas (ex: "Tutorial do SwitchU" em português) em vez de exibir o texto em francês.

### Desempenho e estabilidade

- Removida a solicitação redundante de suspensão ao destravar a tela, corrigindo o atraso ao despertar com o console no dock.
- Garantida a inicialização correta do serviço NS durante a rotina de exclusão de softwares na tela inicial.

---

# SwitchU 2.4.1

## English

Custom search titles for game ports, modal focus and navigation fixes, bounded dossier layout, carousel rendering optimizations, and teardown stability.

### Game ports and search titles

- Edit and save custom search titles for game ports directly from the Software Information dossier, allowing accurate IGDB metadata matching for community ports with differing executable names.
- Custom game-port search titles are now saved in user configuration and preserved across restarts.
- Restructured the Software Information left rail so metadata facts dynamically fit within the glass panel when port action buttons are displayed.
- Normalized Android-port packaging suffixes in the metadata proxy before IGDB queries and cache key generation.

### Navigation and focus restoration

- Prioritize the active text-entry keyboard over parent modals, restoring d-pad navigation when entering search titles.
- Closing text entry or canceling the platform picker reliably restores controller focus to the parent dialog or details screen.

### Performance and stability

- Optimized carousel (`DynamicLine`) rendering in `IconGrid` by eliminating redundant linear scans and recycling scratch buffers.
- Hardened NS service teardown during NetConnect library applet handoffs.
- Synchronized platform picker availability tasks with generation tracking to prevent async race conditions.
- Added translations for newly introduced game port actions across all 8 supported languages.

<details>
<summary>Preview / Screenshots</summary>

![](./screenshots/31.jpg)
![](./screenshots/32.jpg)
![](./screenshots/33.jpg)
![](./screenshots/34.jpg)

</details>

## Português

Títulos de busca personalizados para ports de jogos, correções de foco e navegação em modais, layout contido na ficha de detalhes, otimizações no carrossel e maior estabilidade de encerramento.

### Ports de jogos e títulos de busca

- Edite e salve títulos de busca personalizados para ports diretamente da ficha de informações do software, permitindo correspondência precisa de metadados no IGDB para ports com nomes de executáveis diferentes.
- Os títulos de busca personalizados para ports são salvos nas configurações do usuário e mantidos entre reinicializações.
- Reestruturado o painel lateral da ficha de detalhes para que os fatos de metadados caibam dinamicamente dentro do painel de vidro quando os botões de ação de port estiverem visíveis.
- Normalizados os sufixos de ports Android no proxy de metadados antes de consultar o IGDB e salvar no cache.

### Navegação e restauração de foco

- O teclado virtual agora tem prioridade de foco sobre os diálogos pais, restaurando a navegação por direcional ao digitar títulos de busca.
- Fechar o teclado de texto ou cancelar o seletor de plataforma restaura o foco do controle para o diálogo pai ou tela de detalhes.

### Desempenho e estabilidade

- Otimizada a renderização do carrossel (`DynamicLine`) no `IconGrid`, eliminando buscas lineares redundantes e reutilizando buffers de desenho.
- Reforçado o encerramento do serviço NS durante a transição para o applet de conexão de rede (NetConnect).
- Tarefas de disponibilidade no seletor de plataforma sincronizadas com controle de geração para evitar condições de corrida.
- Adicionadas traduções para as novas ações de ports de jogos em todos os 8 idiomas suportados.

# SwitchU 2.4.0

## English

A new platform picker identifies game ports accurately, fetches only the metadata
that matters, and makes the information easier to read in every supported
language.

### Game ports and platform metadata

- Choose the original platform for a game port from a dedicated visual picker.
  SwitchU uses that choice to find the right metadata and cover artwork rather
  than treating every port as a Nintendo Switch release.
- Platform availability is checked with a fast cached request, so reopening the
  picker never lets an older network response replace current results.
- The platform-picker dossier now uses the same liquid-glass blur as the rest of
  the menu, improving contrast for dark platform artwork.
- Added a PC platform icon and corrected matching for platform metadata slugs.

### Language and service reliability

- Translated platform-picker and game-port controls across all bundled locales.
- Completed remaining Portuguese interface translations.
- Limited the metadata proxy's Gemini integration to the four confirmed
  free-tier models.

## Português

Um novo seletor de plataforma identifica corretamente os ports de jogos, busca
apenas os metadados relevantes e deixa as informações mais fáceis de ler em
todos os idiomas suportados.

### Ports de jogos e metadados de plataforma

- Escolha a plataforma original de um port de jogo em um seletor visual próprio.
  O SwitchU usa essa escolha para encontrar os metadados e a capa corretos, sem
  tratar todos os ports como lançamentos de Nintendo Switch.
- A disponibilidade da plataforma é verificada com uma consulta rápida em cache,
  então reabrir o seletor nunca deixa uma resposta de rede antiga substituir os
  resultados atuais.
- A ficha do seletor de plataforma agora usa o mesmo desfoque de vidro líquido
  do restante do menu, melhorando o contraste de artes de plataformas escuras.
- Adicionado um ícone de plataforma PC e corrigida a correspondência dos slugs
  de metadados de plataforma.

### Idioma e confiabilidade do serviço

- Traduzidos os controles do seletor de plataforma e dos ports de jogos em todos
  os idiomas incluídos.
- Concluídas as traduções restantes da interface em português.
- A integração Gemini do proxy de metadados foi limitada aos quatro modelos
  confirmados do plano gratuito.

# SwitchU 2.3.2

## English

Icons in the single-row view fill in on their own, and folders are one button in
both directions.

### The single-row view

- Icons no longer sit on their loading spinner until the selection reaches them.
  Switching into the view rebuilds the row while the artwork already in memory
  stays valid, so nothing needed fetching and nothing was fetched — but only the
  focused tile was reconnected to the picture it already had. Every tile is
  reconnected now.

### Folders

- **X** is the folder button both ways: on the home screen it files the focused
  title into a folder, and inside an open folder it takes the focused title out.
  Removing used to be **R**, which meant two buttons for one idea.
- The **Add to folder** row is gone from the options behind **+** and from the
  game dossier. Both halves of the job are the same press now, so the menu had
  nothing left to offer.
- **X** still closes a suspended title when one is selected, and the hint bar
  says which of the two it is.

## Português

Os ícones da visualização de linha única aparecem sozinhos, e pastas viraram um
botão só nos dois sentidos.

### Visualização de linha única

- Os ícones não ficam mais no símbolo de carregamento até a seleção chegar neles.
  Entrar na visualização reconstrói a linha enquanto as artes já em memória
  continuam válidas, então não havia nada a buscar e nada foi buscado — mas só o
  ícone em foco era reconectado à imagem que já tinha. Agora todos são.

### Pastas

- **X** é o botão de pasta nos dois sentidos: na tela inicial guarda o título em
  foco em uma pasta, e dentro de uma pasta aberta tira o título em foco dela.
  Remover era **R**, o que dava dois botões para a mesma ideia.
- A linha **Adicionar à pasta** saiu das opções do **+** e da ficha do jogo. As
  duas metades do trabalho são o mesmo toque agora, então o menu não tinha mais
  o que oferecer.
- **X** continua fechando um título suspenso quando há um selecionado, e a barra
  de dicas diz qual das duas coisas ele faz.

# SwitchU 2.3.1

## English

A hotfix over 2.3.0. Taking a game out of a folder is one button now, changing
SwitchU's language really does apply without a restart, and the README describes
what the launcher has become.

### Folders

- **R takes the focused title out of the open folder**, with no menu and no
  confirmation. It used to mean opening the options with **+**, choosing a row
  and confirming, after which the options stayed on screen over software that
  was no longer there.
- The row that did it is gone from the options and from the game dossier. Both
  now only offer **Add to folder**, which is the half that still needs a menu.
- R was free on that screen: sorting is not available inside a folder, so the
  button was advertised and did nothing. Its hint now says what it does.

### Language

- Changing **System > SwitchU Language** now updates the widget tiles and the
  SwitchU screen immediately. 2.3.0 claimed this and did not do it: the fix went
  into a copy of the handler that is compiled out, so the labels kept the old
  language until something else happened to rebuild the home screen.

### Repository

- The README lists everything the launcher actually has, including folders,
  widgets, the single-row view, the on-screen keyboard, artwork without an API
  key and the delete that also removes what a title left on the SD card. It had
  not been updated for several releases.
- A MAC address was visible in one of the 2.3.0 screenshots. It has been removed
  from the image and from this repository's history.

## Português

Uma correção sobre a 2.3.0. Tirar um jogo de uma pasta agora é um botão só,
trocar o idioma do SwitchU realmente vale sem reiniciar, e o README descreve o
que o launcher se tornou.

### Pastas

- **R tira o título em foco da pasta aberta**, sem menu e sem confirmação. Antes
  era abrir as opções no **+**, escolher uma linha e confirmar, e as opções
  continuavam na tela sobre um software que não estava mais ali.
- A linha que fazia isso saiu das opções e da ficha do jogo. As duas agora só
  oferecem **Adicionar à pasta**, que é a metade que ainda precisa de um menu.
- O R estava livre naquela tela: ordenar não funciona dentro de uma pasta, então
  o botão era anunciado e não fazia nada. A dica agora diz o que ele faz.

### Idioma

- Trocar **Sistema > Idioma do SwitchU** agora atualiza os widgets e a tela
  SwitchU na hora. A 2.3.0 prometia isso e não fazia: a correção foi parar numa
  cópia do handler que não é compilada, então os rótulos ficavam no idioma
  antigo até algo reconstruir a tela inicial por outro motivo.

### Repositório

- O README lista tudo o que o launcher tem de fato, incluindo pastas, widgets, a
  visualização de linha única, o teclado na tela, artes sem chave de API e a
  exclusão que também remove o que um título deixou no cartão SD. Ele estava
  desatualizado havia várias versões.
- Um endereço MAC aparecia em uma das capturas da 2.3.0. Ele foi removido da
  imagem e do histórico deste repositório.

# SwitchU 2.3.0

## English

This release rebases the fork onto [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.2.0. Folders, home widgets, the single-row view, the game dossier and the
SteamGridDB artwork scan all arrive from upstream, and the About screen now reads
**Based on SwitchU 1.2.0**. Most of the work here went into making those features
hold up on hardware, and a good part of it is repair rather than addition.

### From PoloNX 1.2.0

- **Folders** on the home screen, with their own page, name and colour.
- **Home widgets**: clock, battery, recently played and playtime, image pins and
  random screenshots, in 1x1 and 2x1 sizes.
- **A single-row view**, reached with Minus, showing one large icon with its
  neighbours either side.
- **The game dossier** on Plus, with artwork, mods, playtime and store details.
- **SteamGridDB artwork**, scanned for the whole library or chosen per title.
- **A controller test screen**, and the SwitchU Manager updater.

### What this fork does differently

- **The on-screen keyboard is our own.** The system keyboard cannot be called
  from a library applet: `swkbdShow()` never returns, and the console froze on
  the first rename. SwitchU draws its own keyboard instead, with accented
  characters, a symbols page and touch.
- **SteamGridDB works without your own API key.** Searches, heroes and grids go
  through the fork's own service. A personal key is still accepted and unlocks
  logos, which the service has no endpoint for.
- **The single-row view is a real carousel.** It wraps in both directions, skips
  empty slots, repeats while ZL, ZR or the d-pad is held, and shows the game's
  logo above the row. Sorting with R is switched off there: order in that view is
  the arrangement you built.
- **Folders hold homebrew as well as games**, can be renamed from their own
  header, and their tile draws up to nine member icons on clear glass, in a grid
  that follows how many are inside.
- **Deleting software removes it.** The upstream delete only asks the system to
  drop what it installed, which leaves a port copied onto the card untouched: one
  such title reported "deleted" and left 49 GB behind. SwitchU now also sweeps
  `atmosphere/contents`, the older `atmosphere/titles`, the SX OS layout and its
  own artwork caches, names the folders before you confirm, and shows a progress
  bar while it works.
- **Changing the language no longer needs a restart.** Widget labels and the
  update screen follow the setting immediately.

### Fixed since 1.2.0

- The home arrangement stopped rewriting itself. Placing a new entry looked for
  the first empty cell, which is the second half of a 2x1 tile, and the tile was
  then moved elsewhere and saved: creating one folder was enough to scatter every
  widget across the pages.
- Pressing R inside an open folder rebuilt the saved layout out of that folder's
  contents, clearing every home entry that was not in it. R is now ignored there.
- Restarting into the single-row view, or deleting a game while in it, left one
  icon alone with nothing either side and no way to move.
- The artwork backdrop was never constructed, so the hero, its gradient and the
  logo above the row had never once been drawn.
- The artwork prefetch queued eight neighbours against a two-entry cache and
  re-read them off the card indefinitely, starving the icons of both worker
  threads.
- The selection ring froze in place whenever an overlay left the menu's route
  behind, most visibly after opening the SwitchU screen.
- The battery, playtime and recently-played widgets showed nothing at all.
- The keyboard read as clear glass over the settings overlay, and the SteamGridDB
  key was masked while typing although it is stored in plain text.
- A folder kept titles that had been deleted, drawing a blank coloured square in
  their place.
- Long game names ran into the button hints along the bottom; the hints now wrap
  onto a second row instead.

### Carried over from before the rebase

- Everything in 2.2.0 is still here: the separate SwitchU language, Bluetooth
  pairing from the launcher, the recovery paths for launching and returning, and
  the Theme Shop that opens without a pause.

### Credit

PoloNX remains credited in the About screen. This fork keeps its own version
number, and the upstream release it descends from is shown beside it.

## Português

Esta versão rebaseia o fork sobre o [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.2.0. Pastas, widgets na tela inicial, a visualização de linha única, a ficha do
jogo e a busca de artes no SteamGridDB vêm todos do upstream, e a tela Sobre agora
mostra **Baseado no SwitchU 1.2.0**. A maior parte do trabalho aqui foi fazer
esses recursos se sustentarem no console, e boa parte dele é conserto, não adição.

### Vindo do PoloNX 1.2.0

- **Pastas** na tela inicial, com página, nome e cor próprios.
- **Widgets**: relógio, bateria, jogados recentemente e tempo de jogo, imagens
  fixadas e captura aleatória, nos tamanhos 1x1 e 2x1.
- **Uma visualização de linha única**, acessada com Menos, que mostra um ícone
  grande com os vizinhos dos dois lados.
- **A ficha do jogo** no Mais, com artes, mods, tempo de jogo e dados da loja.
- **Artes do SteamGridDB**, buscadas para a biblioteca inteira ou escolhidas
  título a título.
- **Uma tela de teste de controle** e o atualizador SwitchU Manager.

### O que este fork faz de diferente

- **O teclado na tela é nosso.** O teclado do sistema não pode ser chamado de um
  library applet: `swkbdShow()` nunca retorna, e o console travava ao renomear
  pela primeira vez. O SwitchU desenha o próprio teclado, com acentos, página de
  símbolos e toque.
- **O SteamGridDB funciona sem a sua própria chave de API.** Buscas, heroes e
  grids passam pelo serviço do próprio fork. Uma chave pessoal continua aceita e
  libera os logos, para os quais o serviço não tem endpoint.
- **A visualização de linha única é um carrossel de verdade.** Dá a volta nos dois
  sentidos, pula espaços vazios, repete enquanto ZL, ZR ou o direcional ficam
  pressionados, e mostra o logo do jogo acima da linha. Ordenar com R fica
  desligado ali: a ordem naquela visualização é o arranjo que você montou.
- **Pastas guardam homebrews além de jogos**, podem ser renomeadas pelo próprio
  cabeçalho, e o ladrilho desenha até nove ícones dos membros sobre vidro limpo,
  numa grade que acompanha quantos estão dentro.
- **Apagar um software apaga de verdade.** A exclusão do upstream só pede ao
  sistema que descarte o que ele instalou, o que deixa intacto um port copiado
  para o cartão: um título assim informou "apagado" e deixou 49 GB para trás. O
  SwitchU agora também varre `atmosphere/contents`, o antigo `atmosphere/titles`,
  o layout do SX OS e os próprios caches de arte, mostra as pastas antes de você
  confirmar e exibe uma barra de progresso enquanto trabalha.
- **Trocar o idioma não exige mais reiniciar.** Os rótulos dos widgets e a tela de
  atualização acompanham a mudança na hora.

### Corrigido desde a 1.2.0

- O arranjo da tela inicial parou de se reescrever sozinho. Colocar uma entrada
  nova procurava a primeira célula vazia, que é a segunda metade de um ladrilho
  2x1, e o ladrilho era então movido para outro lugar e salvo: criar uma pasta já
  bastava para espalhar todos os widgets pelas páginas.
- Pressionar R dentro de uma pasta aberta reconstruía o layout salvo a partir do
  conteúdo daquela pasta, apagando todas as entradas da tela inicial que não
  estavam nela. R agora é ignorado ali.
- Reiniciar na visualização de linha única, ou apagar um jogo estando nela,
  deixava um ícone sozinho, sem nada dos lados e sem como se mover.
- O fundo de arte nunca era construído, então o hero, seu degradê e o logo acima
  da linha nunca tinham sido desenhados uma única vez.
- A pré-carga de artes enfileirava oito vizinhos contra um cache de duas entradas
  e os relia do cartão indefinidamente, deixando os ícones sem as duas threads de
  trabalho.
- O anel de seleção congelava no lugar sempre que uma sobreposição deixava a rota
  do menu para trás, o que aparecia principalmente depois de abrir a tela SwitchU.
- Os widgets de bateria, tempo de jogo e jogados recentemente não mostravam nada.
- O teclado parecia vidro limpo sobre a tela de configurações, e a chave do
  SteamGridDB era ocultada durante a digitação embora fique salva em texto puro.
- Uma pasta mantinha títulos que já haviam sido apagados, desenhando um quadrado
  colorido em branco no lugar deles.
- Nomes de jogos longos invadiam as dicas de botões na parte de baixo; as dicas
  agora quebram para uma segunda linha.

### Mantido de antes do rebase

- Tudo da 2.2.0 continua aqui: o idioma próprio do SwitchU, o pareamento Bluetooth
  pelo launcher, os caminhos de recuperação ao iniciar e voltar de um jogo, e a
  Loja de Temas que abre sem pausa.

### Créditos

O PoloNX segue creditado na tela Sobre. Este fork mantém o próprio número de
versão, e a versão upstream da qual descende é mostrada ao lado dele.

# SwitchU 2.2.0

## English

SwitchU can now be set to its own language, separately from the console. Bluetooth
headphones pair and connect from the launcher for real, launching and returning are
steadier when something goes wrong, and the Theme Shop opens without the pause it
used to have.

### SwitchU Language

- **System > SwitchU Language** is now the first row of the System settings, and
  changing it switches the launcher's language while you watch. Press A to pick
  one; the choice is remembered. All eight languages the launcher ships with are
  in the list.
- **Console Language**, below it, now says it is read-only, which is what it always
  was. It reports what the Nintendo Switch itself is set to and is changed in the
  console's own System Settings, not here.

### Bluetooth audio

- Bluetooth headphones now pair and connect from the launcher. The Bluetooth screen
  used to change only the saved setting, so it could show Bluetooth as on while the
  console's radio was off and a scan would find nothing. It now turns the real radio
  on, keeps checking for devices while the scan runs, and leaves the results on
  screen.
- **Airplane Mode** now switches the Bluetooth radio off as well, instead of only
  recording that it should be off.

### Launching and returning

- A launch that fails now brings you back to the launcher instead of leaving a blank
  screen. Resuming a game you left suspended recovers the same way.
- Starting a game for the first time on a newly created account now creates that
  account's save data for it, instead of failing on an account that has never played
  the game.
- Closing a game from the launcher no longer holds up everything else while it waits.
  The console asks the game to exit, keeps running, and only forces it after the full
  grace period has passed.
- Pressing HOME while a game had its on-screen keyboard open no longer crashes the
  launcher on the way back.
- The launch animation now runs for 340 milliseconds instead of 1.45 seconds, and
  the launcher does its save-data checks and bookkeeping while it plays rather than
  after. How long the console itself then takes to bring a game up is unchanged.

### Themes

- Installing a still-image theme over an animated one now actually replaces the
  wallpaper. The new picture was being loaded without clearing the animation already
  in memory, so the old frames kept playing over it.

### Theme Shop

- Opening the Theme Shop again no longer pauses. It used to rebuild the whole screen
  every time, re-reading and decoding every installed theme's screenshot in the frame
  the shop appeared.
- Installed screenshots now load in the background. A card shows "Loading
  screenshot..." for a moment instead of the whole shop waiting on the picture.
- Browsing and downloading themes no longer risks the shop stalling while artwork
  reaches the graphics card. Uploads are batched and the queue is emptied before
  anything waits on it.
- Some of the opening is still the graphics work behind the frosted-glass panel, and
  that is not addressed here.

### Other changes

- Settings and the Theme Shop are built the first time you open them instead of at
  boot, so a session that never opens them never pays for them.
- Leaving the launcher for a game no longer waits on background work still
  finishing, which could catch the Gallery or the Theme Shop mid-task.

---

## Português

O SwitchU agora pode ficar em um idioma próprio, separado do console. Fones Bluetooth
pareiam e conectam de verdade pelo launcher, iniciar e voltar de um jogo se recuperam
melhor quando algo falha, e a Loja de Temas abre sem a pausa que tinha antes.

### Idioma do SwitchU

- **Sistema > Idioma do SwitchU** agora é a primeira linha das configurações de
  Sistema, e trocá-lo muda o idioma do launcher na hora. Pressione A para escolher;
  a escolha fica salva. Os oito idiomas que o launcher traz estão na lista.
- **Idioma do Console**, logo abaixo, agora diz que é somente leitura, que é o que
  sempre foi. Ele mostra como o Nintendo Switch está configurado e é alterado nas
  configurações do próprio console, não aqui.

### Áudio Bluetooth

- Fones Bluetooth agora pareiam e conectam pelo launcher. A tela de Bluetooth
  alterava apenas a configuração salva, então podia mostrar o Bluetooth ligado
  enquanto o rádio do console estava desligado e a busca não encontrava nada. Agora
  ela liga o rádio de verdade, consulta os dispositivos enquanto a busca acontece e
  mantém os resultados na tela.
- O **Modo Avião** agora desliga também o rádio Bluetooth, em vez de apenas registrar
  que ele deveria estar desligado.

### Iniciar e voltar

- Um jogo que falha ao iniciar agora devolve você ao launcher, em vez de deixar a
  tela em branco. Retomar um jogo deixado suspenso se recupera do mesmo jeito.
- Iniciar um jogo pela primeira vez em uma conta recém-criada agora cria o save
  daquela conta, em vez de falhar em uma conta que nunca jogou aquele título.
- Fechar um jogo pelo launcher não trava mais o resto enquanto espera. O console pede
  para o jogo sair, continua funcionando e só força a saída depois que todo o prazo
  de tolerância passa.
- Pressionar HOME com o teclado na tela de um jogo aberto não derruba mais o launcher
  na volta.
- A animação de abertura agora dura 340 milissegundos em vez de 1,45 segundo, e o
  launcher faz as verificações de save e o registro interno enquanto ela roda, em vez
  de depois. O tempo que o console leva para colocar o jogo no ar não muda.

### Temas

- Instalar um tema de imagem parada sobre um animado agora troca o papel de parede de
  verdade. A imagem nova era carregada sem limpar a animação que já estava na
  memória, então os quadros antigos continuavam rodando por cima.

### Loja de Temas

- Abrir a Loja de Temas de novo não trava mais. Ela reconstruía a tela inteira a cada
  abertura, relendo e decodificando a captura de cada tema instalado no quadro em que
  a loja aparecia.
- As capturas dos temas instalados agora carregam em segundo plano. O cartão mostra
  "Carregando captura..." por um instante, em vez de a loja inteira esperar pela
  imagem.
- Navegar e baixar temas não corre mais o risco de travar a loja enquanto as imagens
  chegam à placa de vídeo. Os envios são agrupados e a fila é esvaziada antes de
  qualquer espera.
- Parte da abertura ainda é o trabalho gráfico por trás do painel de vidro fosco, e
  isso não está resolvido aqui.

### Outras mudanças

- Configurações e Loja de Temas passam a ser montadas na primeira vez que você as
  abre, em vez de na inicialização, então uma sessão que nunca as abre não paga por
  elas.
- Sair do launcher para um jogo não espera mais por trabalho em segundo plano ainda
  em andamento, que podia pegar a Galeria ou a Loja de Temas no meio de uma tarefa.

# SwitchU 2.1.0

## English

The console now goes to sleep on its own again, and the lock screen from the
stock home menu is back with it. A daemon fault that left every power option
dead after a sleep was found and fixed along the way.

### Automatic sleep

- SwitchU now runs the idle countdown behind **Rest Mode > Automatic Sleep**.
  The setting was always there and always saved, but nothing acted on it, so a
  console left alone in the launcher simply never slept. It now follows the
  handheld or docked delay you chose and genuinely suspends: the screen goes
  off and the console draws almost nothing, exactly as it does from the stock
  home menu.
- Sleeping from **Rest Mode > Sleep** no longer disables the background
  service. Every power option, Sleep, Restart and Shutdown, kept working only
  until the first sleep of a session and then silently stopped responding.
  Waking the console also failed to reach the launcher for the same reason.
  Both are fixed.

### Lock screen

- Waking the console now shows a lock screen with the clock, in the shape the
  stock home menu uses. Press the same button three times to get back in, any
  face, shoulder, d-pad or stick button, or tap the screen three times when no
  controller is attached. A half-finished sequence forgets a press after a
  couple of seconds, so a console loose in a bag cannot let itself in.
- HOME unlocks immediately, since the console's own system layer has already
  answered for that press.
- There is nothing to configure. The lock screen is what greets you after the
  console sleeps, and it is not shown while you are using the launcher, where it
  would only keep the screen lit in front of a sleep that was already coming.

### Other changes

- The **UI Wireframe** developer toggle no longer appears in Settings. It drew
  debug outlines around every element and was never meant to be reachable.
- Game descriptions recover from a translation service outage instead of
  falling back to English on the first failure.

---

## Português

O console volta a entrar em descanso sozinho, e a tela de bloqueio do menu
original volta junto. No caminho, foi encontrada e corrigida uma falha do
serviço que deixava todas as opções de energia sem resposta depois de dormir.

### Descanso automático

- O SwitchU agora executa a contagem de inatividade de **Modo de Descanso >
  Suspensão automática**. A opção sempre existiu e sempre foi salva, mas nada
  agia sobre ela, então um console deixado parado no launcher simplesmente
  nunca dormia. Agora ele respeita o tempo escolhido para portátil ou base e
  suspende de verdade: a tela apaga e o console passa a consumir quase nada,
  exatamente como pelo menu original.
- Suspender por **Modo de Descanso > Suspender** não desliga mais o serviço em
  segundo plano. Todas as opções de energia, Suspender, Reiniciar e Desligar,
  funcionavam apenas até o primeiro descanso da sessão e depois paravam de
  responder em silêncio. Acordar o console também não chegava ao launcher pelo
  mesmo motivo. Os dois casos estão corrigidos.

### Tela de bloqueio

- Ao acordar, o console mostra uma tela de bloqueio com o relógio, no formato
  do menu original. Pressione o mesmo botão três vezes para voltar, qualquer
  botão frontal, gatilho, direcional ou clique de analógico, ou toque a tela
  três vezes quando não houver controle conectado. Uma sequência pela metade
  esquece um toque depois de alguns segundos, então um console solto na mochila
  não consegue se desbloquear sozinho.
- HOME desbloqueia na hora, porque o próprio sistema do console já respondeu
  por esse toque.
- Não há nada para configurar. A tela de bloqueio é o que recebe você depois
  que o console dorme, e não aparece enquanto você está usando o launcher, onde
  só manteria a tela acesa na frente de um descanso que já estava chegando.

### Outras mudanças

- O ajuste de desenvolvedor **UI Wireframe** não aparece mais nas Configurações.
  Ele desenhava contornos de depuração em volta de cada elemento e nunca deveria
  estar acessível.
- As descrições dos jogos se recuperam de uma indisponibilidade do serviço de
  tradução em vez de cair para o inglês na primeira falha.

# SwitchU 2.0.1

## English

Thirteenth release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

It asks the console for more memory to allow larger animated themes to fit, and
shrinks all themes in the catalogue to half their memory footprint.

### Memory allocation and performance

- The launcher now requests up to 416 MB of application heap before safely
  falling back. On tested consoles this raised the image budget to 296 MB,
  giving larger animated themes substantially more room when memory is
  available.
- Migrated the entire theme catalogue from the BC7 to the BC1 (DXT1) format.
  This cuts the GPU memory footprint by 50% (from 456 KB to 232 KB per frame)
  and significantly reduces package size. User testing found no visible quality
  loss in the tested themes. Playback can still be sampled for sequences above
  the 320-frame safety cap, or reduced gracefully when an applet has less free
  memory after returning from suspended software.
- Fixed a deko3d safety failure: if a GPU image-memory allocation is refused,
  SwitchU now leaves that image unloaded instead of using an invalid memory
  block and triggering a fatal `svcBreak` when returning from an applet.
- **Note**: For the memory and storage savings to take effect, animated themes
  already installed on your console must be deleted and downloaded again.

### Server deployment

- Replaced the deployment scripts to safely publish themes to standard Ubuntu
  hosts, preserving the immutable hash-based hardlink structure needed by the
  updater.
- Hardened catalogue deployment with the same lock used by ingestion, unique
  same-filesystem staging, archive/package validation, atomic alias replacement,
  and rollback if reindexing fails.

---

## Português

Décima terceira versão da linha [ncarvalho99](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

O launcher passa a pedir mais memória ao console para dar espaço a temas
animados maiores, e todos os temas do catálogo encolheram pela metade.

### Alocação de memória e performance

- O launcher agora pede até 416 MB de *heap* da aplicação antes de usar um
  recuo seguro. Nos consoles testados, isso elevou o orçamento para imagens a
  296 MB, dando bem mais espaço a temas animados maiores quando há memória.
- Todo o catálogo de temas foi migrado do formato BC7 para o BC1 (DXT1). Essa
  mudança corta o consumo de memória da GPU pela metade (de 456 KB para 232 KB
  por quadro) e reduz significativamente o tamanho dos pacotes. Nos temas
  testados, não houve perda visual perceptível. Sequências acima do limite de
  segurança de 320 quadros ainda podem ser amostradas, e pouca memória livre ao
  voltar de um software suspenso pode reduzir a reprodução de forma segura.
- Corrigida uma falha de segurança do deko3d: se uma alocação de memória de
  imagem da GPU for recusada, o SwitchU deixa essa imagem descarregada em vez
  de usar um bloco inválido e provocar `svcBreak` fatal ao voltar de um applet.
- **Nota**: Para que a economia de memória e espaço surta efeito, temas animados
  já instalados no seu console precisam ser apagados e baixados novamente.

### Deploy de servidor

- O script de deploy do servidor foi reescrito para publicar os temas com
  segurança em instâncias Ubuntu normais, respeitando a estrutura de *hardlinks*
  imutáveis exigida pelo atualizador no console.
- O deploy do catálogo foi reforçado com o mesmo bloqueio da ingestão, área
  temporária única no mesmo sistema de arquivos, validação do lote/pacotes,
  troca atômica dos aliases e restauração caso a reindexação falhe.

---

# SwitchU 2.0.0

## English

First release numbered by this fork, based on
[PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0. It follows
1.1.0+fork.11 and is the twelfth release of the
[ncarvalho99](https://github.com/ncarvalho99/SwitchU) line.

### The version number changed, and why

- The previous eleven releases were numbered `1.1.0+fork.N`. That kept the
  upstream version visible, but it put the number that actually changes into
  semver's build metadata -- the one field the specification says to **ignore**
  when comparing versions. Formally, `1.1.0+fork.11` and `1.1.0+fork.2` were the
  same version, and the console only told them apart because the updater's
  comparator was written to read that field. A version has to be unambiguous in
  exactly one place above all others: the code deciding whether to install an
  update.
- Releases are numbered `2.0.0` onwards, as plain semver. Nothing about the
  install changes, and a console on `1.1.0+fork.11` sees this as newer and
  updates normally.
- What this is built from did not go anywhere: Settings, About still shows
  **Based on: SwitchU 1.1.0**, on its own line, which is where it belongs. The
  fork adds to PoloNX's work rather than replacing it.
- The eleven old tags stay. Deleting them would erase the history that got here.

### Themes you already have

- The Animated Themes tab marks the themes already on the console with an
  **Installed** chip, so it takes no opening to tell.
- Opening one offers **Apply** and **Remove** instead of offering to download it
  again -- the same pair the Installed tab shows, acting on the theme the
  download became. Asked for by a player who had no way to remove a theme
  without walking back to the other tab to find it.

### Documentation

- The README describes the launcher as it is. Its TODO list held three items
  that had all shipped -- the panel on **+**, the animated background and the
  SteamGridDB integration -- and its table of contents linked to a Features
  section that did not exist.

---

## Português

Primeira versão numerada por esta fork, baseada no
[PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0. Sucede a
1.1.0+fork.11 e é a décima segunda versão da linha
[ncarvalho99](https://github.com/ncarvalho99/SwitchU).

### O número de versão mudou, e por quê

- As onze versões anteriores eram numeradas `1.1.0+fork.N`. Isso mantinha a
  versão de origem visível, mas colocava o número que de fato muda dentro do
  *build metadata* do semver — justamente o campo que a especificação manda
  **ignorar** ao comparar versões. Formalmente, `1.1.0+fork.11` e
  `1.1.0+fork.2` eram a mesma versão, e o console só as distinguia porque o
  comparador do atualizador foi escrito para ler esse campo. Uma versão precisa
  ser inequívoca num lugar acima de todos: o código que decide se instala uma
  atualização.
- As versões passam a ser numeradas de `2.0.0` em diante, em semver simples.
  Nada muda na instalação, e um console na `1.1.0+fork.11` enxerga esta como
  mais nova e atualiza normalmente.
- De onde isto foi feito não sumiu: Configurações, Sobre continua mostrando
  **Baseado em: SwitchU 1.1.0**, em linha própria, que é onde isso pertence. A
  fork soma ao trabalho do PoloNX em vez de substituí-lo.
- As onze tags antigas ficam. Apagá-las apagaria a história que chegou até aqui.

### Temas que você já tem

- A aba de Temas Animados marca com **Instalado** os temas que já estão no
  console, para não ser preciso abrir cada um só para descobrir.
- Abrir um deles oferece **Aplicar** e **Remover** em vez de oferecer baixar de
  novo — o mesmo par da aba de instalados, agindo sobre o tema em que o download
  se transformou. Pedido por um usuário que não tinha como remover um tema sem
  voltar até a outra aba para procurá-lo.

### Documentação

- O README passou a descrever o launcher como ele é. A lista de TODO tinha três
  itens todos já entregues — o painel no **+**, o fundo animado e a integração
  com o SteamGridDB — e o índice apontava para uma seção de recursos que não
  existia.

---

# SwitchU 1.1.0+fork.11

## English

Eleventh release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Deleting a theme now frees the space, a shortcut stops wearing the icon of the
app it replaced, and nothing writes to the card behind a running game.

### Deleting a theme actually deletes it

- Removing a live wallpaper took it out of the Installed list and left every
  file on the card. The space never came back, and themes accumulated silently.
- The delete used std::filesystem::remove_all and threw the error away.
  remove_all does not work against the console's device paths, so the call
  failed and the theme left the list anyway -- from the outside a failure and a
  success looked the same.
- The result is checked now, a delete that fails says which path stopped it, and
  one that works commits the card so the space is really gone.
- Removing a mod used the same call and had the same fault waiting. It is fixed
  with it.
- Themes already left behind stay on the card. They have to be deleted once by
  hand; the fix applies from here on.

### Shortcuts

- A shortcut deleted and created again for another app kept the old name and
  icon. Names and icons are cached per title, and a forwarder shortcut reuses
  its title when it is recreated, so the cache looked current when it was not.
- Titles that appear or disappear now lose what was cached for them, so a
  shortcut made while the console is on shows its own icon without a reboot.
- Nothing automatic can notice a title reused while the console was off, so
  Options gains **Reload games and shortcuts**: it clears the cache and reads
  the installed titles again. The player is the one who can see the wrong icon.

### Nothing writes to the card behind a game

- The background worker that caches names and icons kept running while a game
  was in the foreground: an IPC call per title and two files written to the card,
  underneath a game whose own content the card is also serving. The catalogue
  rebuild already stood aside for this and the worker did not.
- It waits now, and resumes when the menu is back. Whether this was behind the
  crashes reported while playing is not established -- it is worth not doing
  either way.

### Release notes on the console

- The Update tab showed each note cut off mid-sentence. A changelog bullet wraps
  over several lines and only the first begins with the dash; everything after
  the opening clause was dropped. The whole item is kept now.

---

## Português

Décima primeira versão da fork [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Apagar um tema passa a liberar o espaço, um atalho deixa de vestir o ícone do
aplicativo que substituiu, e nada mais grava no cartão por trás de um jogo.

### Apagar um tema apaga de verdade

- Remover um live wallpaper tirava da lista de instalados e deixava todos os
  arquivos no cartão. O espaço nunca voltava, e os temas se acumulavam em
  silêncio.
- A exclusão usava `std::filesystem::remove_all` e jogava o erro fora. O
  `remove_all` não funciona contra os caminhos de dispositivo do console, então
  a chamada falhava e o tema saía da lista do mesmo jeito — de fora, falha e
  sucesso pareciam iguais.
- O resultado passou a ser conferido, uma exclusão que falha diz em que caminho
  parou, e uma que dá certo confirma no cartão, para o espaço ir embora mesmo.
- Remover um mod usava a mesma chamada e tinha o mesmo defeito esperando. Foi
  corrigido junto.
- Os temas que já ficaram para trás continuam no cartão. Precisam ser apagados
  à mão uma vez; a correção vale daqui em diante.

### Atalhos

- Um atalho apagado e criado de novo para outro aplicativo mantinha o nome e o
  ícone antigos. Nomes e ícones ficam em cache por título, e um atalho forwarder
  reaproveita o seu título ao ser recriado, então o cache parecia atual sem
  estar.
- Títulos que aparecem ou somem passam a perder o que estava em cache, então um
  atalho criado com o console ligado mostra o próprio ícone sem reiniciar.
- Nada automático consegue perceber um título reaproveitado enquanto o console
  estava desligado, então Opções ganhou **Recarregar jogos e atalhos**: limpa o
  cache e relê os títulos instalados. Quem enxerga o ícone errado é a pessoa.

### Nada grava no cartão por trás de um jogo

- O trabalhador em segundo plano que guarda nomes e ícones continuava rodando
  com um jogo em primeiro plano: uma chamada de IPC por título e dois arquivos
  gravados no cartão, por baixo de um jogo cujo próprio conteúdo o cartão também
  serve. A reconstrução do catálogo já se abstinha disso e ele não.
- Agora ele espera, e volta quando o menu está de volta. Se isso estava por trás
  das quedas relatadas durante o jogo não está estabelecido — não fazer isso
  vale de qualquer forma.

### Notas de versão no console

- A aba Atualização mostrava cada nota cortada no meio da frase. Um item do
  changelog quebra em várias linhas e só a primeira começa com o traço; tudo
  depois da cláusula inicial era descartado. Agora o item inteiro é mantido.

---

# SwitchU 1.1.0+fork.10

## English

Tenth release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Moving across the grid no longer stalls, the theme shop says what everything
costs, and an update that has been downloaded stops asking to be downloaded.

### Moving the cursor no longer stalls

- Landing on a game with custom artwork froze the menu for about half a second,
  every time. The cover was read off the card, decoded and uploaded inside the
  frame being drawn -- a megabyte or more of card, and tens of milliseconds of
  JPEG, with the launcher waiting for all of it.
- Reading and decoding now happen on a worker thread and only the upload is left
  in the frame, which is microseconds. The image arrives a few frames later
  rather than instantly, and the grid stays at full speed while it does.
- Moving quickly across the grid abandons requests instead of queueing them:
  only the cover you stop on is ever shown.

### What the themes cost

- The Installed tab counts what the themes on the console occupy, both as a
  total at the top and on each theme's own card. Built-in themes are not
  counted -- they ship inside the launcher and take nothing from the card.
- Those measurements are taken on a worker as well. A theme is hundreds of
  frames on the card, and adding that up during a frame is the same stall the
  artwork had.
- On a theme's detail screen the installed size gets its own line. Sharing one
  line with the download size ran past the column and the ellipsis ate the half
  that matters when deciding whether a theme fits.

### Updates

- An update that has been downloaded no longer offers to download itself again.
  Once the package is staged, checking, the automatic offer and the install row
  are all held back until the restart that applies it. The pending state is read
  from the card rather than remembered, because the menu restarts every time a
  game closes and a forgotten flag was how the loop started.
- The Update tab offers **Restart now** while that restart is pending, so
  nobody has to leave the screen that asked for it.

### Smaller things

- Pressing HOME with the cursor on a sidebar button left it there, while the
  name at the bottom of the screen showed the game -- two things pointing at
  each other's answer. The selector now returns to the grid.
- L and R turn the page in all three theme tabs. Reaching the previous and next
  buttons meant walking down the whole grid, and in a catalogue of dozens of
  themes turning the page is the most repeated thing on that screen.

### Notes

- An earlier package of this release was withdrawn. Applying an update writes
  527 files and about 43 MB, and none of it was committed to the card -- so a
  console carried that much outstanding metadata through the session that
  followed, and the next reboot could bring hekate up unable to find nyx. The
  reboot itself was never at fault; the write before it was. Both the extraction
  and the removal of the archive now commit.
- Because the update is applied by the version already installed, that fix takes
  effect from the update **after** this one. Installing this package from a PC
  avoids the risk entirely.

---

## Português

Décima versão da fork [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Andar pela grade não trava mais, a loja de temas diz quanto tudo custa, e uma
atualização já baixada para de pedir para ser baixada.

### Passar o cursor não trava mais

- Parar num jogo com capa personalizada congelava o menu por cerca de meio
  segundo, toda vez. A capa era lida do cartão, decodificada e enviada à GPU
  dentro do quadro que estava sendo desenhado — um megabyte ou mais de cartão e
  dezenas de milissegundos de JPEG, com o launcher esperando por tudo isso.
- Ler e decodificar passaram para uma thread de trabalho e no quadro sobrou só a
  subida para a GPU, que é questão de microssegundos. A imagem aparece alguns
  quadros depois em vez de na hora, e a grade continua fluida enquanto isso.
- Andar rápido pela grade abandona os pedidos em vez de enfileirá-los: só a capa
  onde você para chega a ser mostrada.

### Quanto os temas custam

- A aba Instalados conta o que os temas ocupam no console, tanto no total, no
  topo, quanto no cartão de cada tema. Os embutidos não entram: vêm dentro do
  launcher e não tomam nada do cartão.
- Essas medições também acontecem numa thread de trabalho. Um tema é centenas de
  quadros no cartão, e somar isso durante o quadro é a mesma travada da capa.
- Na tela de detalhe do tema, o tamanho instalado ganhou linha própria. Dividir
  uma linha com o tamanho do download passava da coluna, e as reticências comiam
  justamente a metade que importa para decidir se o tema cabe.

### Atualizações

- Uma atualização já baixada não se oferece mais para ser baixada de novo. Com o
  pacote pronto no cartão, a verificação, o oferecimento automático e a linha de
  instalar ficam retidos até o reinício que a aplica. Esse estado é lido do
  cartão, e não guardado em memória, porque o menu reinicia toda vez que um jogo
  é fechado — e uma marca esquecida foi como o laço começou.
- A aba Atualização oferece **Reiniciar agora** enquanto esse reinício está
  pendente, para ninguém precisar sair da tela que o pediu.

### Coisas menores

- Apertar HOME com o cursor num botão da barra lateral o deixava lá, enquanto o
  nome no rodapé da tela mostrava o jogo — dois lugares apontando para a
  resposta um do outro. O seletor volta para a grade.
- L e R viram a página nas três abas de temas. Chegar aos botões de anterior e
  próxima significava descer a grade inteira, e num catálogo de dezenas de temas
  virar página é o gesto mais repetido daquela tela.

### Notas

- Um pacote anterior desta versão foi retirado. Aplicar uma atualização grava
  527 arquivos e cerca de 43 MB, e nada disso era confirmado no cartão — então o
  console carregava toda essa metadata pendente pela sessão seguinte, e o
  próximo reinício podia trazer o hekate sem encontrar o nyx. O reinício nunca
  foi o culpado; a gravação anterior a ele é que era. A extração e a remoção do
  pacote passaram a confirmar.
- Como a atualização é aplicada pela versão já instalada, essa correção só passa
  a valer da atualização **seguinte** a esta. Instalar este pacote pelo PC evita
  o risco por completo.

---

# SwitchU 1.1.0+fork.9

## English

Ninth release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Changing theme could end the launcher. It no longer can, and the theme shop now
says what a theme costs before you take it.

### Changing theme could kill the launcher

- Switching between the default dark and light themes could fill the screen with
  colour noise and drop the console back to a relaunch. Reported by a player who
  hit it every time.
- A texture handed its descriptor slot and its memory back the instant it was
  replaced, without waiting for the GPU. The slot returned to the free list, the
  next texture claimed it, and the frame already in flight sampled an image whose
  memory was gone. Changing theme re-primes every installed preview while the
  frame is being drawn, which is why that screen was the one that died.
- Both figures were checked against the reporter's card first: his theme files
  were intact and the right size. Nothing about the crash was local to him except
  the timing.
- Image dimensions are now also refused before they reach the graphics driver,
  which answers a layout it cannot compute by killing the process rather than
  returning an error.

### What a theme costs

- Each animated theme shows its download size at the end of the author's line.
- The top of the catalogue tab summarises the whole repository: how many themes
  it holds, how much they are to download, and how much they occupy once
  unpacked. The two are far apart -- the frames compress hard -- and the number
  that matters when a card is filling up is the second one.
- Both figures also appear on a theme's detail screen, where the decision to
  install is actually made.
- The totals are counted from the catalogue as it is read, so a theme published
  later is included without anything being edited by hand.

### Smaller things

- The tutorial's background gets the same gentle blur the menu uses by default.
  It was the one screen in the launcher rendering its background at full
  sharpness behind the text panel, and it is the first screen anyone sees.
  Suggested by a player who noticed exactly that.
- Settings are written beside the configuration file and swapped in, rather than
  over it. A launcher that dies mid-write used to leave a truncated file that
  failed to parse at the next boot, resetting every preference to its default;
  the previous copy is now kept and read if the current one cannot be.

---

## Português

Nona versão da fork [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Trocar de tema podia encerrar o launcher. Não pode mais, e a loja de temas passa
a dizer quanto um tema custa antes de você levá-lo.

### Trocar de tema podia matar o launcher

- Alternar entre os temas padrão escuro e claro podia encher a tela de ruído
  colorido e devolver o console a um relançamento. Relatado por um usuário que
  reproduzia sempre.
- Uma textura devolvia o slot de descritor e a memória no instante em que era
  substituída, sem esperar a GPU. O slot voltava para a lista livre, a textura
  seguinte o tomava, e o quadro já entregue à GPU lia uma imagem cuja memória
  havia sumido. Trocar de tema reprepara todas as prévias instaladas durante o
  desenho do quadro, e é por isso que era justamente aquela tela que morria.
- Os dois números foram conferidos antes no cartão de quem relatou: os arquivos
  de tema dele estavam íntegros e no tamanho certo. Nada no crash era particular
  a ele além do tempo.
- As dimensões de imagem passam também a ser recusadas antes de chegar ao driver
  gráfico, que responde a um layout impossível encerrando o processo em vez de
  devolver erro.

### Quanto custa um tema

- Cada tema animado mostra o tamanho do download no fim da linha do autor.
- O topo da aba do catálogo resume o repositório inteiro: quantos temas tem,
  quanto é para baixar e quanto ocupa depois de descompactado. Os dois números
  ficam longe um do outro — os quadros comprimem muito — e o que importa quando
  o cartão está enchendo é o segundo.
- Os dois aparecem também na tela de detalhe do tema, que é onde a decisão de
  instalar é de fato tomada.
- Os totais são somados do catálogo conforme ele é lido, então um tema publicado
  depois entra sozinho, sem nada para editar à mão.

### Coisas menores

- O fundo do tutorial recebe o mesmo desfoque suave que o menu usa por padrão.
  Era a única tela do launcher desenhando o fundo em nitidez total atrás do
  painel de texto, e é a primeira tela que qualquer pessoa vê. Sugerido por um
  usuário que notou exatamente isso.
- As configurações são gravadas ao lado do arquivo e trocadas no final, em vez de
  por cima dele. Um launcher que morresse no meio da gravação deixava um arquivo
  truncado que falhava na leitura do boot seguinte, zerando todas as preferências;
  agora a cópia anterior é guardada e lida quando a atual não puder ser.

---

# SwitchU 1.1.0+fork.8

## English

Eighth release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

The launcher can now update itself, and two ways of losing the menu are gone.

### Updating from the launcher

- SwitchU has an **Update** tab, below Options. It shows the installed version,
  what the last check found, and what changed -- the notes for the installed
  build ship with it, so the tab answers before it has spoken to anyone.
- The launcher checks GitHub for a newer release once a day, and there is a
  button to check immediately, both in the Update tab and under Settings, About.
  A check you asked for always answers, including when nothing changed; the
  daily one stays quiet unless there is news.
- Accepting an update downloads it, checks it against the size the release
  published, inspects every path it carries and asks you to restart. The files
  are put in place by the daemon at the next boot, before the menu exists,
  because a running menu cannot replace the font and binaries it is holding
  open. **That first boot takes around half a minute longer than usual** while
  466 files are unpacked; it is a one-time cost per update.
- Nothing is overwritten in place. Each file is written beside its destination
  and swapped in at the end, so a file that cannot be replaced is left exactly
  as it was rather than destroyed. An update that fails is retried at the next
  two boots and then abandoned, so it can never keep the console from starting.
- Release notes scroll with up and down, so long changelogs are read rather than
  truncated.

### Two ways the menu could be lost

- Moving the cursor across a game with custom artwork could end the menu and
  send you back to a relaunch. Its background texture was freed while the frame
  still being drawn was reading it, and the focus path does that on every step
  across the grid.
- Applying a background left the launcher stuck on the game icon, escapable only
  with HOME. The user picker had taken the buttons while being drawn behind the
  dossier, so the grid sat under a menu that was never visible.

### Notes

- An earlier build of this release was withdrawn: its installer reused the theme
  package extractor, which accepts media and text and would have refused 392 of
  the 466 files a launcher build contains, starting with the menu binary itself.
  The extractor now takes an explicit policy, and an update must additionally
  prove that every path it carries stays inside `atmosphere/` and `switch/` --
  it is unpacked at the root of a card that also holds the bootloader.

---

## Português

Oitava versão da fork [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

O launcher passa a se atualizar sozinho, e dois jeitos de perder o menu deixaram
de existir.

### Atualizar pelo launcher

- O SwitchU ganhou a aba **Atualização**, abaixo de Opções. Ela mostra a versão
  instalada, o que a última verificação encontrou e o que mudou — as notas da
  versão instalada vêm junto com o build, então a aba responde antes mesmo de
  falar com alguém.
- O launcher consulta o GitHub uma vez por dia, e há um botão para verificar na
  hora, tanto na aba quanto em Configurações, Sobre. Uma verificação que você
  pede sempre responde, inclusive quando nada mudou; a diária fica calada a menos
  que haja novidade.
- Aceitar a atualização baixa o pacote, confere com o tamanho que a release
  publicou, inspeciona todos os caminhos que ele carrega e pede o reinício. Os
  arquivos são postos no lugar pelo daemon no boot seguinte, antes de o menu
  existir, porque um menu em execução não consegue substituir a fonte e os
  binários que mantém abertos. **Esse primeiro boot demora cerca de meio minuto
  a mais** enquanto 466 arquivos são descompactados; é um custo único por
  atualização.
- Nada é sobrescrito no lugar. Cada arquivo é gravado ao lado do destino e
  trocado no final, então um arquivo que não puder ser substituído fica
  exatamente como estava, em vez de ser destruído. Uma atualização que falhe é
  tentada nos dois boots seguintes e então abandonada, de modo que nunca pode
  impedir o console de iniciar.
- As notas de versão rolam com cima e baixo, então um changelog longo é lido, e
  não cortado.

### Dois jeitos de perder o menu

- Passar o cursor sobre um jogo com arte personalizada podia encerrar o menu e
  devolver você a um relançamento. A textura do fundo era liberada enquanto o
  quadro ainda em desenho a estava lendo, e o caminho do foco faz isso a cada
  passo pela grade.
- Aplicar um fundo deixava o launcher preso no ícone do jogo, com saída apenas
  pelo botão HOME. O seletor de usuário havia tomado os botões enquanto era
  desenhado atrás do dossiê, então a grade ficava sob um menu que nunca aparecia.

### Notas

- Uma versão anterior desta release foi retirada: o instalador reaproveitava o
  extrator de pacotes de tema, que aceita mídia e texto e teria recusado 392 dos
  466 arquivos de um build do launcher, a começar pelo binário do próprio menu.
  O extrator passou a receber uma política explícita, e uma atualização precisa
  ainda provar que todo caminho que carrega fica dentro de `atmosphere/` e
  `switch/` — ela é aberta na raiz de um cartão que também guarda o bootloader.

---

# SwitchU 1.1.0+fork.7

## English

Seventh release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

A maintenance release. Everything here came from playing fork.6 on real hardware
and fixing what got in the way.

### Game dossier

- Games whose entry has no story text no longer fail to open. The dossier read a
  null field as if it were text, which threw and blanked the whole screen, so a
  title without a recorded story showed only *Online details are unavailable*.
  Diablo III: Eternal Collection and Mario Kart 8 Deluxe were the reported cases.
- Deleting software asks for confirmation again. The dialog was being created
  behind the dossier: it took the buttons, so the on-screen hints changed, but no
  card was ever drawn and the delete looked like it was waiting on nothing.
- When online details really are unavailable, pressing **A** retries instead of
  leaving the screen stuck until it is closed and reopened.
- Ports and homebrew no longer load for ever. A search that finished without
  finding the title fell through to the loading message, so a port sat on
  *Loading game details* although the service had already answered.
- **+** on homebrew, forwarders and emulator ports now offers only the action
  that applies to them, removal, instead of a dossier of empty fields. They have
  no catalogue entry, no store version, no mods and no cover art, so nothing is
  requested from the service for them either.
- Synopses stay in the console language. A translation that failed because the
  daily quota was spent used to be stored for 30 days, so the game stayed in
  English long after the quota recovered; it is now retried within the hour.
  Regional variants such as es-MX and pt-PT resolve to their base language
  instead of being treated as having no translator at all.

### Grid and controls

- **R** now advances the sort order when it is released, and only on a short tap.
  Holding **R** over a game is how the homebrew override reaches Sphaira, and the
  grid used to reorder itself under the player's hand during that hold.
- New installations start with menu music at 15%, sound effects at 25% and a
  light 5% background blur, the levels people settled on after using the menu on
  hardware. Grid columns and rows are unchanged.

### Theme catalogue

- The five wallpapers added in fork.6 animate on the site again. Their preview
  choice lived in a file the catalogue generator rewrites, so it was silently
  reverted; the generator itself now prefers a browser-playable clip.
- The catalogue site was rebuilt: larger artwork, search by name or author,
  filters for animated, soundtrack and unlicensed themes, and a loop meter that
  tracks each preview's real position. Turning previews off no longer blanks the
  page, which a missing variable had been doing since the control was added.

### Infrastructure

- The metadata, gallery and catalogue services moved to a single cloud host and
  no longer depend on a machine at home staying powered.
- Translation spreads across several models, so one exhausted daily quota no
  longer means English text for the rest of the day.
- A rate-limit refusal returns a clean 429. It was crashing into a 500 instead,
  and a burst of requests could take every game's details down at once.

### Validation

- Tested on console: dossier for the reported titles, delete confirmation, sort
  on release, holding **R** through to Sphaira, and the new audio defaults.

---

## Português

Sétima versão da fork [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

Versão de manutenção. Tudo aqui saiu de usar a fork.6 no console e corrigir o
que atrapalhava.

### Dossiê do jogo

- Jogos sem texto de história voltam a abrir. O dossiê lia um campo nulo como se
  fosse texto, o que lançava erro e apagava a tela inteira: um título sem enredo
  cadastrado mostrava apenas *Os detalhes online não estão disponíveis*. Diablo
  III: Eternal Collection e Mario Kart 8 Deluxe foram os casos relatados.
- Apagar software pede confirmação de novo. O diálogo era criado atrás do
  dossiê: ele recebia os botões, por isso as dicas na tela mudavam, mas nenhum
  cartão era desenhado e a exclusão parecia travada esperando nada.
- Quando os detalhes online realmente não estão disponíveis, **A** tenta de
  novo, em vez de deixar a tela presa até ser fechada e reaberta.
- Ports e homebrews não ficam mais carregando para sempre. Uma busca que
  terminava sem encontrar o título caía na mesma mensagem de carregamento, então
  um port ficava em *Carregando detalhes do jogo* embora o serviço já tivesse
  respondido.
- O **+** sobre homebrews, atalhos e ports de emulador passa a oferecer apenas a
  ação que faz sentido para eles, a remoção, em vez de um dossiê de campos
  vazios. Não têm ficha no catálogo, versão de loja, mods nem capa, então também
  nada é pedido ao serviço por causa deles.
- As sinopses ficam no idioma do console. Uma tradução que falhava por a cota
  diária ter acabado era guardada por 30 dias, então o jogo continuava em inglês
  muito depois de a cota se recuperar; agora é refeita dentro de uma hora.
  Variantes regionais como es-MX e pt-PT passam a usar o tradutor do idioma
  base, em vez de serem tratadas como sem tradutor algum.

### Grade e controles

- O **R** passa a avançar a ordenação na soltura, e somente em toque curto.
  Segurar **R** sobre um jogo é como o atalho de homebrew alcança o Sphaira, e a
  grade se reordenava sob a mão do jogador durante esse tempo.
- Instalações novas começam com música do menu em 15%, efeitos em 25% e um
  desfoque leve de 5% no fundo, os níveis a que as pessoas chegaram usando o
  menu no console. Colunas e linhas da grade não mudaram.

### Catálogo de temas

- Os cinco papéis de parede acrescentados na fork.6 voltam a animar no site. A
  escolha da prévia deles vivia num arquivo que o gerador do catálogo reescreve,
  então era desfeita em silêncio; agora o próprio gerador prefere um vídeo que o
  navegador consegue reproduzir.
- O site do catálogo foi refeito: arte maior, busca por nome ou autor, filtros
  por animado, com trilha e sem licença, e um medidor que acompanha a posição
  real de cada prévia. Desligar as prévias não apaga mais a página, coisa que
  uma variável inexistente vinha causando desde que o controle foi criado.

### Infraestrutura

- Os serviços de metadados, galeria e catálogo passaram para um único servidor
  em nuvem e não dependem mais de uma máquina em casa continuar ligada.
- A tradução se distribui entre vários modelos, então uma cota diária esgotada
  deixa de significar texto em inglês pelo resto do dia.
- Uma recusa por limite de requisições devolve 429 corretamente. Antes ela
  quebrava em 500, e uma rajada podia derrubar os detalhes de todos os jogos.

### Validação

- Testado no console: dossiê dos títulos relatados, confirmação de exclusão,
  ordenação na soltura, segurar **R** até o Sphaira e os novos padrões de áudio.

---

# SwitchU 1.1.0+fork.6

## English

Sixth release of the [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU)
fork, based on [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

### Game options redesigned as a dossier

- Pressing **+** on a game or homebrew now opens its software dossier directly.
  It combines the former options with game-specific information, artwork and mod
  management in one controller-friendly screen.
- The dossier shows the active icon, version, installed-mod count, local playtime,
  publisher, release date, genres, themes, game modes and Time to Beat estimates
  for quick, main-story and completionist playthroughs.
- IGDB metadata adds translated synopsis and story, publishers, gameplay captures,
  cover art and duration estimates. Metascore and user score are served through
  the SwitchU metadata service; catalogue responses are cached and never include
  console profile data.
- SteamGridDB gallery integration lets players browse covers and backgrounds,
  filter artwork by dimensions, expand previews, apply artwork, inspect the active
  artwork and restore the default icon. Non-matching cover ratios use a blurred
  supporting fill rather than stretching or cropping the game art.
- Full-screen gallery, artwork, restore and mods views now return with **B** to
  the dossier instead of closing to the home screen. Screenshot navigation and
  selection bounds were also corrected.

### Connectivity, themes and polish

- Added a real **Airplane Mode** toggle under Internet. It synchronizes Wi-Fi and
  other wireless state, correctly refreshes after returning to the tab, and is
  translated in every bundled language.
- Opening the system network applet no longer waits for an in-flight catalogue or
  gallery HTTP request to time out; pending small requests are cancelled safely
  during the handoff.
- Default Dark and Default Light return to the author's lightweight default
  themes, with accurate preview thumbnails. Theme browsing remains available from
  the catalogue, and the background-blur control now responds from its first step.
- The mod manager has a clean, independent card layout and system-style toggles
  in place of Enabled/Disabled chips. It keeps direct enable, disable and remove
  actions with restart guidance.
- The entire new interface, labels and notifications are localized for pt-BR,
  en-US, es-ES, fr-FR, de-DE, it-IT, nl-NL and ru-RU.

### Validation

- The sysmodule release payload was tested successfully on console, including the
  redesigned game options and artwork flow.

---

## Português

Sexta versão da fork [ncarvalho99/SwitchU](https://github.com/ncarvalho99/SwitchU),
baseada no [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU) 1.1.0.

### Opções do jogo redesenhadas como dossiê

- Pressionar **+** sobre um jogo ou homebrew agora abre diretamente o dossiê do
  software. Ele reúne as antigas opções, informações específicas, artes e
  gerenciamento de mods em uma única tela navegável pelo controle.
- O dossiê mostra ícone ativo, versão, quantidade de mods, tempo local jogado,
  publicadoras, lançamento, gêneros, temas, modos de jogo e estimativas do
  Time to Beat para jogo rápido, história principal e completo.
- Os metadados do IGDB acrescentam sinopse e história traduzidas, publicadoras,
  capturas de gameplay, capa e durações. Metascore e nota de usuários são
  fornecidos pelo serviço de metadados do SwitchU; as respostas ficam em cache e
  não incluem dados de perfil do console.
- A galeria SteamGridDB permite navegar por capas e fundos, filtrar as artes por
  dimensões, ampliar a prévia, aplicar a arte, consultar a arte ativa e restaurar
  o ícone padrão. Capas em proporção diferente recebem preenchimento desfocado,
  sem esticar ou recortar a arte do jogo.
- As telas em tela cheia de galeria, arte ativa, restauração e mods agora voltam
  com **B** para o dossiê, em vez de fechar para a tela inicial. A navegação de
  capturas e os limites da seleção também foram corrigidos.

### Conexão, temas e acabamento

- Adicionado um toggle real de **Modo avião** em Internet. Ele sincroniza o estado
  do Wi-Fi e das demais conexões sem fio, atualiza corretamente ao retornar à aba
  e foi traduzido para todos os idiomas incluídos.
- Abrir o applet de rede do sistema não espera mais uma consulta de catálogo ou
  galeria atingir timeout: as requisições pequenas pendentes são canceladas com
  segurança durante a transição.
- Default Dark e Default Light voltam a ser os temas padrão leves do autor, com
  miniaturas fiéis. A navegação de temas continua disponível no catálogo, e o
  controle de desfoque do fundo passou a responder desde o primeiro nível.
- O gerenciador de mods recebeu cards independentes sem linhas sobrepostas e
  toggles no estilo do sistema no lugar de botões Habilitado/Desabilitado. As
  ações de ativar, desativar e remover continuam diretas, com aviso de reinício.
- Toda a interface, rótulos e notificações novos foi localizada para pt-BR,
  en-US, es-ES, fr-FR, de-DE, it-IT, nl-NL e ru-RU.

### Validação

- O payload sysmodule de release foi testado com sucesso no console, incluindo
  as opções redesenhadas de jogo e o fluxo de artes.

<details>
  <summary><b>Capturas da fork.6 / Fork.6 screenshots</b></summary>

![](./screenshots/2.jpg)
![](./screenshots/3.jpg)
![](./screenshots/4.jpg)
![](./screenshots/5.jpg)
![](./screenshots/6.jpg)
![](./screenshots/7.jpg)
![](./screenshots/8.jpg)
![](./screenshots/9.jpg)
![](./screenshots/10.jpg)
![](./screenshots/11.jpg)

</details>

---

# SwitchU 1.1.0+fork.5

## English

Fifth release of this fork of [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. It builds on fork.4 with animated backgrounds, a package-based theme
catalogue, persistent game sorting, and fixes validated on a real console.

**Animated themes**

- Theme Shop adds an **Animated Themes** tab. The catalogue and packages are
  downloaded directly from [themes.nclabs.dev](https://themes.nclabs.dev), the
  default HTTPS catalogue configured in SwitchU.
- Animated wallpapers are pre-encoded as DDS **BC7** frames at 912×512 and streamed
  to the GPU; no video is decoded while the menu is rendering. This is the single,
  definitive package quality, selected to stay inside the Switch applet memory budget.
- The menu requests a larger applet heap when available, derives a safe image budget
  from it, and uploads background frames incrementally. Returning from a game no
  longer waits for every animation frame to upload at once.
- Theme packages are checked for safe catalogue paths, archive limits and free SD
  space. Installation is transactional and reliably replaces an existing package:
  a failed install preserves the working theme.
- Download and extraction progress now remain monotonic, and the new Theme Shop
  messages are translated in all bundled languages.

**Home screen and stability**

- Press **R** to cycle between **My order**, **A–Z**, and **Recent**. Manual icon
  moves are saved as My order; Recent uses actual launches and uses the personal
  order only to break ties.
- Fixed the sort view so icon artwork moves with its title. Repeated sorting no
  longer exposes freed GPU textures or stale focus pointers, which caused artifacts
  and menu crashes during console testing.
- Community previews are pruned continuously and only visible candidates upload to
  the GPU. The catalogue also avoids unnecessary manifest requests when optional
  screenshots are absent.
- The daemon throttles application-view refreshes after title events. The former
  200 ms scan loop could cause severe launcher lag after a game exit; Zelda: Tears
  of the Kingdom was launched and returned to the menu successfully after this fix.
- The theme website was split into maintainable HTML, CSS and JavaScript assets and
  hardened with restrictive server security headers and safer client-side rendering.

**Validation**

- The definitive 912×512 BC7 animated-background format was validated on both the
  handheld display and TV.

---

## Português

Quinta versão desta fork de [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. Ela evolui a fork.4 com fundos animados, catálogo de temas por pacote,
ordenação persistente dos jogos e correções validadas em um console real.

**Temas animados**

- A Loja de Temas traz a aba **Temas Animados**. O catálogo e os pacotes são
  baixados diretamente de [themes.nclabs.dev](https://themes.nclabs.dev), o
  catálogo HTTPS padrão configurado no SwitchU.
- Os fundos animados usam quadros DDS em **BC7** a 912×512 enviados em fluxo à GPU;
  não há decodificação de vídeo durante a renderização do menu. Esta é a qualidade
  única e definitiva dos pacotes, escolhida para respeitar a memória de applet.
- O menu pede um heap de applet maior quando o sistema o concede, calcula dele um
  orçamento seguro para imagens e envia os quadros de forma incremental. Voltar de
  um jogo não espera mais o upload de todos os quadros de uma vez.
- Os pacotes verificam caminhos seguros no catálogo, limites do arquivo e espaço
  livre no cartão. A instalação é transacional e substitui corretamente um tema já
  instalado: se falhar, o tema que funcionava é preservado.
- O progresso de download e extração agora é contínuo, e as novas mensagens da Loja
  de Temas foram traduzidas para todos os idiomas incluídos.

**Tela inicial e estabilidade**

- Pressione **R** para alternar entre **Minha ordem**, **A–Z** e **Recentes**.
  Movimentos manuais dos ícones são salvos em Minha ordem; Recentes usa aberturas
  reais dos jogos e usa a ordem pessoal somente para desempates.
- Corrigida a ordenação para que a arte do ícone acompanhe seu jogo. Alternar a
  ordem repetidamente não deixa mais texturas de GPU ou ponteiros de foco obsoletos,
  que causavam artefatos e crashes nos testes no console.
- As texturas de prévia da comunidade são podadas continuamente e somente candidatas
  visíveis chegam à GPU. O catálogo também evita buscar manifestos sem necessidade
  quando capturas opcionais estão ausentes.
- O daemon agora limita a atualização das views de aplicativos após eventos de
  títulos. A varredura anterior a cada 200 ms podia causar lag extremo no launcher
  após sair de um jogo; Zelda: Tears of the Kingdom abriu e retornou ao menu
  normalmente após esta correção.
- O site de temas foi separado em HTML, CSS e JavaScript mais fáceis de manter e
  reforçado com cabeçalhos de segurança restritivos e renderização mais segura no
  cliente.

**Validação**

- O formato definitivo de fundo animado em BC7 a 912×512 foi validado tanto na
  tela portátil quanto na TV.

---

# SwitchU 1.1.0+fork.4

## English

Fourth release of this fork of [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. All the original work is his, and it stays credited in the About page.

A short one: two things that were wrong on screen, and one more attempt at the
crash people keep reporting.

**The reported crash — still open**

Someone with a 1TB card described it more precisely: it happens with many games
installed, a clean card is fine, it goes wrong while the shortcuts are being
built, and some shortcuts came out blank.

That last detail matters, because nothing found so far explains a blank icon.
Following it led to a real defect: when uploading an icon's texture failed, the
failure was ignored entirely. The icon stayed blank, and the texture slot it had
taken was never given back — so every failure made the next one likelier, which
is exactly the shape of "the more games, the worse it gets". The slot is
returned now, and the failure is written to the log with the pool's state.

**This is not a fix for the crash.** It is a real bug that produces one of the
reported symptoms, and it turns the next report into evidence instead of another
guess.

**Fixed**

- The theme screen ran at 30fps. It covers the home scene completely, and the
  optimisation that stops drawing an occluded scene had only ever been offered
  to the settings overlay — about 16ms a frame spent on pixels nothing could see
- Long setting descriptions ran over the control beside them instead of wrapping

---

## Português

Quarta versão deste fork do [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. Todo o trabalho original é dele, e o crédito continua na página Sobre.

Uma versão curta: duas coisas que estavam erradas na tela, e mais uma tentativa
no crash que continua sendo relatado.

**O crash relatado — ainda em aberto**

Alguém com um cartão de 1TB descreveu melhor: acontece com muitos jogos
instalados, cartão limpo funciona, dá errado enquanto os atalhos estão sendo
criados, e alguns atalhos saíam em branco.

Esse último detalhe importa, porque nada do que foi encontrado até agora explica
um ícone em branco. Seguir por ele levou a um defeito real: quando o envio da
textura de um ícone falhava, a falha era simplesmente ignorada. O ícone ficava
em branco, e o espaço de textura que ele tinha ocupado nunca era devolvido — de
modo que cada falha tornava a seguinte mais provável, que é exatamente o formato
de "quanto mais jogos, pior fica". O espaço agora é devolvido, e a falha é
registrada no log junto com o estado do conjunto.

**Isto não é a correção do crash.** É um defeito real que produz um dos sintomas
relatados, e transforma o próximo relato em evidência em vez de mais um
chute.

**Corrigido**

- A tela de temas rodava a 30fps. Ela cobre a cena da home por completo, e a
  otimização que para de desenhar uma cena ocluída só havia sido oferecida ao
  painel de configurações — cerca de 16ms por frame gastos em pixels que
  ninguém podia ver
- Descrições longas de ajustes passavam por cima do controle ao lado em vez de
  quebrar linha

---

# SwitchU 1.1.0+fork.3

## English

Third release of this fork of [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. All the original work is his, and it stays credited in the About page.

This one is about the crash people have been reporting, and about putting the
settings where they are actually looked for.

**The reported crash — attempt one, not confirmed**

A crash report finally arrived with symbols that match the build it came from.
It resolves to `WiiUMenuApp::onUpdate` branching to an address in no module at
all: a call through a pointer to an object that had already been destroyed.

Rebuilding the grid frees every icon, and two places kept using them afterwards.
The focus managers hold raw pointers and call `onFocusLost()` on whatever they
believe is focused; edit mode holds two more and dereferences both without
checking. Both are told now.

That explains the shape of the reports: a title installed while the menu is open
makes the grid genuinely different, so it is rebuilt — and the crash lands during
the rebuild, which is why the new shortcut is missing and only appears after a
restart. On a fresh install the same thing happens repeatedly while the icon
cache is still cold.

**It is not confirmed fixed.** It has never reproduced here, and both changes
come from reading code against crash reports rather than from watching it stop.

### If it still crashes

Two things, and neither needs any technical knowledge:

1. Turn the console off. Put the microSD card in a computer, open the folder
   `atmosphere/crash_reports`, and send whatever is inside — the `.log` files
   there are plain text and contain no personal information.
2. Say which version you were running. It is on the About page in Settings,
   and it should read **1.1.0+fork.3**.

That is everything. The other file needed to read those reports is
`SwitchU-1.1.0-fork.3-symbols.zip`, attached to this release — you do not need
to download it, and it is here rather than left in the build system because
build artifacts are deleted after 90 days and a report that arrives later than
that cannot be read without it.

Without the crash reports there is nothing to go on: the console shows an
error code that says a crash happened and nothing about where.

**Glass**

- Lowering glass sharpness turned the panels into visible squares. The blur
  takes nine samples spaced by its radius, so at the low end they landed 9 to
  36 texels apart with nothing read between them — sampling a grid rather than
  blurring it. Width now comes from repeating the pass instead of spreading it
- The account and power dialogs read sharper than the settings screen with the
  same setting. Refraction was displacing in panel-relative units, so it bent
  proportionally more behind a large panel than a small one. It is measured in
  pixels now, and a small window looks like a large one

**Appearance settings**

- New defaults, chosen after looking at them on a console rather than here:
  glass sharpness 40%, background animation speed 35%, background blur 5%.
  Defaults only apply to a fresh install — an existing one keeps what it has
- Glass sharpness, background animation speed and background blur moved out of
  Settings, Display and into the theme screen, next to the rest of what changes
  how the menu looks. They were in two places at once; now they are in one
- "Theme Shop" is now just **Themes**, in all eight languages

**Accounts**

- The top avatar opens the account list, with a tile for creating a new user
- The accounts dialog can be reached with the controller, not only by touch

**Tutorial**

- Skipping was one line in a corner panel at half size, indistinguishable from
  "skip step". It has its own centred prompt now, at nearly twice the scale,
  translated everywhere

**Under the hood**

- Cached names are terminated on read as well as on write, so a truncated cache
  file cannot walk off the end of one
- The devkitA64 toolchain exposes portlibs, which is what a local build needs to
  find the libraries it links against. No effect on the console

---

## Português

Terceira versão deste fork do [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. Todo o trabalho original é dele, e o crédito continua na página Sobre.

Esta é sobre o crash que vem sendo relatado, e sobre colocar as configurações
onde as pessoas realmente procuram por elas.

**O crash relatado — tentativa um, não confirmada**

Chegou finalmente um crash report com símbolos que batem com a build que o
gerou. Ele resolve para `WiiUMenuApp::onUpdate` saltando para um endereço que
não existe em módulo nenhum: chamada através de um ponteiro para um objeto já
destruído.

Reconstruir a grade libera todos os ícones, e dois lugares continuavam usando-os
depois disso. Os gerenciadores de foco guardam ponteiros crus e chamam
`onFocusLost()` no que julgam estar focado; o modo de edição guarda mais dois e
desreferencia ambos sem verificar. Os dois passam a ser avisados.

Isso explica o formato dos relatos: um título instalado com o menu aberto torna
a grade de fato diferente, então ela é reconstruída — e o crash acontece durante
a reconstrução, que é por isso que o atalho novo não aparece e só surge depois de
reiniciar. Em instalação limpa a mesma coisa se repete enquanto o cache de
ícones ainda está frio.

**Não está confirmado como corrigido.** Nunca reproduziu aqui, e as duas
mudanças vêm de ler código contra crash reports, não de ver o problema parar.

### Se continuar crashando

Duas coisas, e nenhuma delas exige conhecimento técnico:

1. Desligue o console. Coloque o cartão microSD num computador, abra a pasta
   `atmosphere/crash_reports` e mande o que estiver lá dentro — os arquivos
   `.log` são texto puro e não contêm nenhuma informação pessoal.
2. Diga qual versão você estava usando. Ela aparece na página Sobre, dentro de
   Configurações, e deve estar como **1.1.0+fork.3**.

É só isso. O outro arquivo necessário para ler esses relatórios é o
`SwitchU-1.1.0-fork.3-symbols.zip`, anexado a esta release — você não precisa
baixá-lo, e ele está aqui em vez de ficar no sistema de build porque artefatos
de build são apagados depois de 90 dias, e um relato que chegue depois disso
não teria como ser lido.

Sem os crash reports não há por onde começar: o console mostra um código de
erro que diz que houve um crash e nada sobre onde.

**Vidro**

- Baixar a nitidez do vidro deixava os painéis quadriculados. O desfoque tira
  nove amostras espaçadas pelo seu raio, então no mínimo elas caíam a 9 e 36
  texels de distância sem ler nada entre elas — amostrando uma grade em vez de
  borrá-la. A largura agora vem de repetir o passe, não de espalhá-lo
- As janelas de contas e de energia apareciam mais nítidas que a de
  configurações com o mesmo ajuste. A refração deslocava em unidades relativas
  ao painel, e por isso desviava proporcionalmente mais atrás de um painel
  grande. Agora é medida em pixels, e uma janela pequena fica igual a uma grande

**Configurações de aparência**

- Novos padrões, escolhidos olhando no console e não aqui: nitidez do vidro 40%,
  velocidade da animação de fundo 35%, desfoque de fundo 5%. Padrões só valem
  para instalação nova — quem já tem configuração salva mantém a dele
- Nitidez do vidro, velocidade da animação de fundo e desfoque de fundo saíram de
  Configurações, Tela e foram para a tela de temas, junto do resto do que muda a
  aparência do menu. Estavam em dois lugares ao mesmo tempo; agora estão em um
- "Loja de temas" agora é só **Temas**, nos oito idiomas

**Contas**

- O avatar do topo abre a lista de contas, com um bloco para criar um usuário novo
- O diálogo de contas passa a ser alcançável pelo controle, não só por toque

**Tutorial**

- Pular era uma linha num painel de canto, em metade do tamanho, idêntica a
  "pular passo". Agora tem aviso próprio e centralizado, quase o dobro da escala,
  traduzido em todos os idiomas

**Por baixo**

- Nomes em cache são terminados na leitura além da escrita, para que um arquivo
  de cache truncado não possa passar do fim de um deles
- O toolchain devkitA64 expõe portlibs, que é o que um build local precisa para
  achar as bibliotecas que linka. Sem efeito no console

---

# SwitchU 1.1.0+fork.2

## English

Second release of this fork of [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. All the original work is his, and it stays credited in the About page.

This one is mostly about themes, and about giving back control over how the
interface looks: a theme catalogue of our own alongside his, and sliders for the
things people said were too sharp, too busy or too fast. Here is the changelog:

**Themes**

- New theme shop catalogue with eight backgrounds, downloaded on demand — none of it is in the package
- PoloNX's catalogue is read alongside ours rather than replaced, so both sets appear in one list
- A catalogue that cannot be reached no longer empties the shop; whatever the other returns is still listed
- The dark theme is now the default on a fresh install, and the first-run tutorial follows it

**New settings**

- **Background blur** — softens the wallpaper and the shapes drifting over it, together
- **Glass sharpness** — how clearly the screen behind menus shows through them. The default matches how it looked before
- **Background animation speed** — from stopped to twice the theme's own pace

**Bug fixes**

- Fixed the settings overlay frosting the entire screen until the first button press
- Fixed the wallpaper and icons disappearing while the glass sharpness slider was moved
- Fixed the button hints in the corner appearing in English in every language — 15 of the 17 had never been translated
- Restored the more detailed sidebar icons

**Known issue**

A crash has been reported when installing a new game or homebrew. It does not
reproduce here and no crash report has reached us yet, so it is not fixed in this
release. If it happens to you, `sdmc:/atmosphere/crash_reports/` and
`sdmc:/config/SwitchU/` are what make it fixable.

---

## Português

Segunda versão deste fork do [PoloNX/SwitchU](https://github.com/PoloNX/SwitchU)
1.1.0. Todo o trabalho original é dele, e o crédito continua na página Sobre.

Esta é sobre temas e sobre devolver o controle da aparência: um catálogo de temas
nosso ao lado do dele, e sliders para o que as pessoas acharam nítido demais,
carregado demais ou rápido demais. Segue o changelog:

**Temas**

- Novo catálogo da loja de temas com oito fundos, baixados sob demanda — nada disso vai no pacote
- O catálogo do PoloNX é lido junto com o nosso, não no lugar dele, então os dois conjuntos aparecem numa lista só
- Um catálogo fora do ar não esvazia mais a loja; o que o outro devolver continua listado
- O tema escuro passa a ser o padrão em instalação nova, e o tutorial inicial acompanha

**Novas configurações**

- **Desfoque do fundo** — suaviza o papel de parede e as formas que flutuam sobre ele, juntos
- **Nitidez do vidro** — o quanto a tela atrás dos menus aparece através deles. O padrão reproduz como era antes
- **Velocidade da animação de fundo** — de parado até o dobro do ritmo do próprio tema

**Correções de bugs**

- Corrigido o painel de configurações deixando a tela inteira embaçada até o primeiro toque de botão
- Corrigidos o papel de parede e os ícones sumindo enquanto o slider de nitidez era arrastado
- Corrigidas as dicas de botão do canto aparecendo em inglês em todos os idiomas — 15 das 17 nunca haviam sido traduzidas
- Restaurados os ícones mais detalhados da barra lateral

**Problema conhecido**

Foi relatado um crash ao instalar um jogo ou homebrew novo. Não reproduz aqui e
nenhum relatório de crash chegou até agora, então não está corrigido nesta
versão. Se acontecer contigo, `sdmc:/atmosphere/crash_reports/` e
`sdmc:/config/SwitchU/` são o que torna isso corrigível.

---

## Installation / Instalação

Extract to the root of the microSD card, replacing the existing files. Requires Atmosphère.

Extraia na raiz do cartão microSD, substituindo os arquivos existentes. Requer Atmosphère.

---

## Previous releases / Versões anteriores

**1.1.0+fork.1** — microSD corruption from the power menu, return-to-menu stutter
from 1–2s to ~400ms, Settings from 30 to 60 fps, antialiased corners throughout,
sharper glass, and the About page identifying the fork.
