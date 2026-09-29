# SwitchU 2.6.5

Maintenance and stability release resolving power-menu reboot on Atmosphere, improving home grid slot handling, migrating SteamGridDB options to a dedicated SwitchU Menu tab, and preparing the forward-compatible update bridge to OmniLauncher.

---

## English

### ⚡ Power & Reboot Fixes
- **Clean Atmosphere Payload Reboot**: Replaced raw `spsmShutdown` power-down calls with `appletStartRebootSequence()`, allowing Atmosphere's `bpc-mitm` service to safely arm `sdmc:/atmosphere/reboot_payload.bin` into IRAM before power cycling. Eliminates black screen drops into RCM during power-menu reboots on Erista consoles with AutoRCM.
- **Power Sequence Safety**: Recovered from failed SD card commits and power requests without freezing the daemon.

### 🎮 Grid & Interface Fixes
- **Free Slot Movement Across Grid**: Dynamic streamer reallocation allows placing icons in any empty slot on any page without requiring sequential placement.
- **Ghost Entry Cleanup**: Pruned 0-ID entries from folders and fixed root folder continuation cell navigation.
- **Touchscreen Power Button & Sidebar Activation**: Direct touch taps on the sidebar and power icon now immediately open dialogs and applets reliably.

### 🎨 Customization & SteamGridDB
- **Dedicated SteamGridDB Tab**: Moved all SteamGridDB artwork options (artwork toggle, background artwork opacity slider, API key, and bulk artwork scan) into a dedicated SteamGridDB tab in the SwitchU Menu.
- **SteamGridDB Background Opacity Adjustment**: Allows adjusting game background artwork opacity from 0% to 100% with real-time preview.
- **Cleaned System Settings**: System Settings is now focused strictly on console preferences.

### 🚀 Future-Ready Updates
- **OmniLauncher Migration Path**: Forward-compatible update engine bridges SwitchU 2.6.5 to OmniLauncher 1.0.0+ when released.

---

## Português

### ⚡ Correções de Energia e Reinicialização
- **Reinicialização Limpa de Payload no Atmosphere**: Substituídas chamadas diretas de desligamento `spsmShutdown` por `appletStartRebootSequence()`, permitindo que o serviço `bpc-mitm` do Atmosphere carregue com segurança `sdmc:/atmosphere/reboot_payload.bin` na IRAM antes de reiniciar. Elimina a tela preta com queda no modo RCM ao reiniciar pelo menu de energia em consoles Erista com AutoRCM.
- **Segurança na Sequência de Energia**: Tratamento resiliente caso a gravação no cartão SD ou chamada de energia falhem, evitando travamento do daemon.

### 🎮 Grade e Interface
- **Movimentação Livre de Ícones na Grade**: Realocação dinâmica do streamer permite mover e posicionar jogos em qualquer espaço vazio, sem exigir posições sequenciais.
- **Limpeza de Fantasmas**: Remoção de entradas com ID zero em pastas e correção da navegação por células de continuação.
- **Ativação por Toque na Barra Lateral e Botão de Energia**: Toques diretos na tela na barra lateral e no ícone de energia agora abrem imediatamente as opções e applets.

### 🎨 Personalização e SteamGridDB
- **Aba Dedicada do SteamGridDB**: Todas as opções do SteamGridDB (ativar arte, controle deslizante de opacidade, chave de API e busca de artes ausentes) agora ficam em uma aba dedicada no Menu SwitchU.
- **Ajuste de Opacidade da Arte de Fundo SteamGridDB**: Controle de opacidade da arte de fundo de 0% a 100% com indicador em tempo real.
- **Configurações do Sistema Mais Limpas**: Menu de Ajustes agora focado exclusivamente em opções do console.

### 🚀 Atualizações Futuras
- **Ponte de Migração para o OmniLauncher**: Mecanismo de atualização preparado para migrar do SwitchU 2.6.5 para o OmniLauncher 1.0.0+ assim que publicado.
