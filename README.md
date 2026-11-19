# SpaghettiKart — Android

Android version of [SpaghettiKart](https://github.com/HarbourMasters/SpaghettiKart) (the Mario Kart 64 PC port),
kept as a direct fork of it so upstream changes merge straight in. It started from
[izzy2lost/SpaghettiKart](https://github.com/izzy2lost/SpaghettiKart), the original Android port. Made for Android
handhelds with a built-in controller and phones alike. Tested on the **AYN Odin 2 Portal** (1920×1080, Android 13).

What this version adds (details in [CHANGELOG.md](CHANGELOG.md)):

- Upstream SpaghettiKart — branch `main` follows upstream, `android` is this port
- App ID `io.github.almostdrownedrat.spaghettikart` ("SpaghettiKart") — installs next to other SpaghettiKart apps
- **Touch buttons** (N64 layout: stick, A, B, Z, L, R, START, C buttons; D-pad hidden by default), sent to the game
  as a controller, so they can be remapped in the game's controller settings like any pad. Their settings are under
  **Touch controls** in the menu (Back, or the **controller icon** at the top centre): **Touch buttons Automatic /
  Always / Off** (Automatic hides them while a
  controller is used and a touch brings them back — on a handheld you normally never see them), move and resize,
  button size, show / hide single buttons, opacity, stick sensitivity, reset. Touches off the buttons still work
  as a mouse in the menus. The **Mods** button (to the mods and saves managers) is shown while the menu is open
- **Back** (swipe from the edge to show the system bar and tap back, or the back gesture) opens
  the menu in game — the same in all of this fork's Android ports and fully usable with the controller (D-pad /
  stick, **A** choose, **B** back): **Back to the game**, **Game settings and controller** (the game's own menu,
  where the controller is set up) and **Touch controls**. With the game's menu open, Back closes it; **B** closes it too. The game stands
  still while the game's menu is open
- RetroArch shaders (librashader, Vulkan; 64-bit Android 10+): none are shipped. In the menu Back opens, Shader >
  Import shader pack (.zip) takes a slang shader pack such as libretro/slang-shaders' "Download ZIP"; it is unpacked
  into the app's folder (Android/data/<app>/files/shaders) and its presets (.slangp) can then be picked by folder.
  While a shader is in use the game draws into a buffer that is drawn through the shader onto the screen
  (src/port/android/ShaderHost.cpp); switching between shaders applies at once, from Off to a shader at the next
  start. librashader (MIT / MPL-2.0 / GPL-3.0) is loaded at run time; its C header is src/port/android/librashader.h.
- Menu fully usable with the controller: D-pad / stick move, **A** select, **B** back / close, **L / R** section,
  **ZL / ZR** tab, **Left / Right** change a slider by one step, **Y** reset the selected setting to its default.
  Every setting has a reset button (orange when changed), and **Reset page** resets a whole page
- Real full screen, no black strip from the navigation bar; the game renders at the full 1920×1080
- Fixed: the APK was missing the files the launcher needs to turn the ROM into game data

The race renders in 16:9 by default (wider view, not stretched); title and menu screens keep their 4:3 borders.
For 4:3 in the race too, set *Settings → Graphics → Force aspect ratio* to *Original (4:3)*.

### AI assistance

The changes in this Android version were made with AI assistance ([Claude Code](https://claude.com/claude-code)). Planning,
testing on the device and the decision what gets released are by the repository owner. Tested on the
**AYN Odin 2 Portal** — use at your own risk. This applies only to this version's own changes, not to the code of the
original projects.
