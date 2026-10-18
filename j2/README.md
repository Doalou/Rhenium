# Jalon J2 : moteur (plan)

Objectif du cahier des charges : `content_shell` en un seul processus sur le Lumia, rendu Skia logiciel, réseau et TLS. Critère de sortie : **Wikipédia s'affiche dans content_shell**. Base : Chromium **116.0.5845.228**, celle de J1 (V8 11.6.189.22 et //base y sont validés sur le téléphone).

Statut au 2026-10-16 : **étape 1 faite** (sources, `gn gen` et `ninja -n` passent : 16 954 cibles, 67 145 étapes pour `rhenium_content`), **compilation en masse en cours**.

## Utilisation

```powershell
./fetch.ps1                    # sources (archives gitiles), dépôts externes du DEPS évalué par deps.py
./fetch.ps1 -Missing <chemin>  # fichier réclamé : un .gn/.gni seul, sinon son dépôt ou son dossier
./gen-loop.ps1                 # gn gen + fetch -Missing jusqu'à ce que gn gen passe
./ninja-loop.ps1               # ninja -n + fetch -Missing jusqu'à ce que toutes les sources soient là
./build-content.ps1 -GenOnly   # correctifs J1 + J2, racine GN, args.gn, gn gen (out\content-arm)
./build-content.ps1 -KeepGoing # + ninja -k 0 rhenium_content
./gn.ps1 path //:rhenium_content //third_party/tflite:tflite   # interroger le graphe
```

## Étape 1 : ce qui a été fait

- **Sources** : ~300 dossiers de l'arbre et ~50 dépôts externes, puis ce que `gn gen` et `ninja -n` ont réclamé. `content_shell` est un outil de test de Chromium : sa bibliothèque dépend de `//content/test:test_support`, dont le `BUILD.gn` déclare toutes les suites de tests de content. GN lit toutes les cibles d'un fichier chargé, d'où des centaines de fichiers de build récupérés un par un (`chrome/`, `extensions/`…, fichiers de build seulement).
- **Racine GN** : le `.gn` de Chromium tel quel (valeurs par défaut comme `crashpad_dependencies`, liste blanche d'`exec_script`), partagé avec J1.
- **Arguments** en plus de la liste ci-dessus : `enable_extensions=false`, `enable_ipc_fuzzer=false` (ces deux-là tiraient `chrome/` dans le graphe), `build_with_tflite_lib=false`, `enable_libaom=false`, `rtc_include_dav1d_in_internal_decoder_factory=false`, `rtc_use_h264=false`.
- **Correctif `patches\0003-content-shell-winuwp.patch`** :
  - `content_shell` : tests web, `about:tracing` et devtools hors du pak sous UWP ;
  - Aura sans Views : l'assertion de `tools/grit/grit_args.gni` exempte les builds `winuwp` ; tests Aura qui supposent Views coupés ;
  - devtools : table de ressources vide (`devtools_resources_uwp.cc`) à la place du front-end (node, TypeScript, esbuild) ; overlay d'inspection de Blink minimal (`rhenium_inspector_overlay`) ;
  - telemetry coupé de `content/test` ; `media/cdm:cdm_paths` défini aussi sous Windows ; ANGLE ne copie plus `d3dcompiler_47.dll` en ARM32 (absent du SDK, fourni par W10M).
- **Dans le correctif 0001 de J1** : MIDL en ARM32 (sortie de `midl.exe` prise comme référence, préprocesseur clang-cl de la toolchain).

## Étape 2 : compilation en masse

**Atteinte le 2026-10-17** : `rhenium_content` (bibliothèques de `content_shell`, `content_shell.pak`) se construit pour `winuwp` ARM32. 27 333 objets, 1 246 bibliothèques.

| Passage | Fait | Échecs |
| --- | --- | --- |
| 1 | 26 337 étapes sur 67 145 | 1 447 |
| 2 | 21 106 sur les 39 815 restantes | 340 |
| 3 | arrêté par gn gen (sortie en double de l'overlay devtools) | |
| 4 à 9 | du reste, par petits lots | 54, 8, 16, 6, 3, 1 |
| 10 | 14 456 sur 15 036 | 53 |
| 11 | 18 230 sur 18 727 | 25 (ATL absent pour 18) |
| 12 | 31 166 sur 31 203 (après installation d'ATL) | 1 |
| 13 | 39 sur 39 : `rhenium_content` construit | 0 |

Correctifs, par cause (dans `0003` sauf mention) :

| Cause | Correctif |
| --- | --- |
| V8 de l'arbre sans les correctifs de J1 (677 échecs) | `..\j1\patches\0002` appliqué à `src/v8` (même commit) ; options V8 de J1 (`v8_enable_system_instrumentation`, `v8_enable_etw_stack_walking` à faux) |
| `content` suppose les traces Perfetto | `enable_base_tracing=true` pour J2 |
| Blink compilé une seconde fois pour la chaîne hôte du snapshot V8 | `use_v8_context_snapshot=false` |
| Python 3.14 : `imp`, `distutils`, `cgi` retirés ; `six` 1.15 sans `find_spec` | Imports remplacés (`importlib`, `shutil`, `html`) ; `find_spec` de `six` 1.16 reporté dans les deux copies de `six` |
| `flat_tree` et `fixed_flat_map` sur `std::array` (STL MSVC) | Dans `0002` : `base::ranges::begin/end` ne renvoient plus de pointeurs pour `std::array` avec la STL MSVC ; trois appelants corrigés |
| `rc.exe` de Chromium absent (hook gclient) | Dans `0001` : `rc.py` utilise le `rc.exe` du SDK et notre clang-cl comme préprocesseur |
| node (WebUI) | node 16.13.0 de nodejs.org et `node_modules` de Chromium, empreintes vérifiées ; installés à la main, exclus du dépôt de référence |
| libvpx : pas de configuration `win/arm`, assembleur NEON passé par Perl | Configuration C pure `source/config/winuwp/generic` (sans pthreads) |
| libaom, SwiftShader, dav1d, tflite, H.264 encore tirés | Arguments (`enable_libaom`, `rtc_include_dav1d_in_internal_decoder_factory`, `rtc_use_h264`, `build_with_tflite_lib`) et dépendances conditionnées dans `media/gpu` et `ui/gl` |
| WebRTC : assembleur ARMv7 GNU | Versions C sous Windows ARM32 |
| `sandbox/win` : SDK 19041 et clang 23 | Constantes NT non `constexpr`, `COMPONENT_FILTER` défini, cas 32 bits ouvert à ARM32 |
| Crashpad sans ARM32 Windows | Client porté (registres, capture de contexte par `RtlCaptureContext`) ; `components/crash` sans gestionnaire ni minidumps sous UWP |
| clang 23 plus strict que celui de M116 | Dans `0001` : `-Wno-c++11-narrowing-const-reference`, `-Wno-missing-template-arg-list-after-template-kw` |
| Générateur de stratégies sans plateforme `winuwp` : liste vide, `policy::key` absents (une centaine d'erreurs) | `--target-platform` passé en `win` dans `generate_policy_source.gni` |
| Itérateurs `std::array` de la STL MSVC (classes, pas pointeurs) | `auto*` remplacés par `auto` (une trentaine) ; dans `0002`, `IsContiguousIterator` reconnaît les itérateurs contigus de la STL MSVC (`base::span`) |
| Interfaces caméra et effets audio postérieurs au SDK 19041 | `mf_camera_control_compat_win.h` (déclarations, `MFCreateCameraControlMonitor` déjà chargée à l'exécution) ; cas audio gardés par la version de contrat |
| DXVA AV1 absent du SDK 19041 | Accélérateur AV1 retiré sous `winuwp` (pas de décodage AV1 matériel sur l'Adreno 430) |
| ATL absent (accessibilité Windows, synthèse vocale, module ATL de `content_main`) | Composant « ATL ARM » de MSVC (installé avec le toolset 14.30, lié avec le 14.44) ; `build-base.ps1` le cherche dans tous les toolsets (`RHENIUM_ATL_DIR`), `setup_toolchain.py` l'ajoute à l'environnement ARM32 ; dans `0002`, `win/atl_throw.cc` recompilé pour la cible |
| Tests web dans `shell_main_delegate` | Code des tests web retiré sous `WINUWP` |
| Sorties des hooks `lastchange` (`gpu_lists_version.h`, `skia_commit_hash.h`) | Écrites par `build-content.ps1` (révision de la balise, révision de Skia du `DEPS`), exclues du dépôt de référence |
| Divers | `LICENSE` et `DAWN_VERSION` (hooks), plateforme `winuwp` pour les field trials, contrôle du SDK retiré de `windows_version.cc`, overlay devtools inscrit dans `resource_ids.spec`, barrière de compilation ARM32 pour abseil, chemins d'URL dans le générateur Mojo TypeScript, `#include <vector>` manquant |

## Étape 3 : démarrage

App UWP `Rhenium.Shell` : `rhenium_shell.exe` (cible GN `rhenium_shell`, 122 Mo), empaquetée par `package-shell.ps1` (69 Mo) avec `content_shell.pak`, ICU et l'instantané V8, installée par `..\sonde\deploy.ps1 -Appx ..\j2\out\RheniumShell_arm.appx`.

- **Point d'entrée** (`app\shell_uwp_main.cc`) : vue `CoreApplication` en WRL ; `Run()` active la fenêtre puis lance `ContentMain`, dont la boucle du thread UI tourne sur le `CoreDispatcher` (pompe de J1). Options : `--single-process --headless --disable-gpu --disable-software-rasterizer --disable-gpu-compositing --disable-legacy-window --no-sandbox`, `--data-path` et `--log-file` dans LocalState, complétées par `LocalState\rhenium-args.txt`. Page par défaut : un `data:` qui écrit dans la console, recopiée dans le journal.
- **Journal** (`app\log_export_uwp.cc`) : `content_shell.log` recopié chaque seconde dans `Phone\Downloads\Rhenium.Shell…\rhenium-*.log` ; en cas d'exception non gérée, code et adresses PC/LR relatives à l'exe.
- **Édition de liens** : `dinput8.lib` retiré sous `winuwp` ; résolveurs du sandbox inertes pour ARM32 (`resolver_arm.cc`) ; `MessageWindow` UWP qui échoue proprement ; `_AddressOfReturnAddress` évité dans Blink ; `MoveFileW`, `GetSystemPowerStatus`, `RegisterWaitForSingleObject` et `UnregisterWaitEx` remplacés (`..\j1\app\w10m_imports.cc`), car `kernel32-legacy` manque sur W10M.
- **Vérification des imports** (sonde `-ImportCheck`) : 862 imports, aucun manquant bloquant ; 187 absents en chargement différé (user32, winmm, urlmon, dwmapi, imm32…), sans effet tant qu'ils ne sont pas appelés.
- **Premier démarrage** : le moteur s'initialise, puis trois violations d'accès déterministes sur trois threads (performance_manager, quota, service workers), toutes dans des appels par pointeur de membre virtuel. Cause : avec `/guard:cf`, clang ARM32 place l'appel à `__guard_check_icall` dans les relais (vcall thunks) après le chargement des arguments, qu'il écrase (r1 reçoit la cible). Correctif dans `0001` : `win_enable_cfg_guards` faux sur ARM32 (`/guard:cf,nochecks`, tables CFG gardées).- **Relais vcall** : couper les vérifications CFG ne suffisait pas. Clang ARM32 émet ces relais comme des fonctions variadiques à appel terminal forcé et y prend r1 comme registre de travail (`ldr r1, [r0]` ; `ldr r1, [r1, #n]` ; `bx r1`), alors que r1-r3 portent encore les arguments. `fix_vcall_thunks.py` réécrit chaque relais avec r12, à taille égale, dans la copie de l'exe que `package-shell.ps1` empaquette (218 relais).
- **V8** : gin fournissait l'allocateur de pages de PartitionAlloc, qui demande des pages exécutables à `VirtualAlloc` (refusé aux apps). Sous `WINUWP`, `V8Platform::GetPageAllocator()` rend `nullptr` : V8 et cppgc gardent l'allocateur de J1 (W^X, attente des pages de code) ; options `--no-write-code-using-rwx --write-protect-code-memory`.
- **Fonctions absentes qui renvoient 0** : l'appel différé d'une fonction user32 absente renvoie 0 sans erreur. `FilePath::CompareIgnoreCase` passait par `CharUpperW` : tous les noms se valaient (`.tmp` égal à `.exe`). Remplacé sous `WINUWP` (ASCII direct, sinon `LCMapStringEx`). Audit : aucune autre fonction absente à effet logique au démarrage (`GetSystemMetrics` et `SystemParametersInfo` existent sur le téléphone).
- **ARM32 oublié** : Blink `GetStackStart()` sans branche ARM32 (valeur de retour indéfinie) ; libyuv lisait `/proc/cpuinfo` dès que `__arm__` est défini (mode `"re"` refusé par le CRT). Recherche des avertissements `-Wreturn-type` et des `fopen` en mode glibc sur tout le build : rien d'autre.
- **Fichier d'échange du rendu** : `PreventExecuteMapping` rend vrai sous `WINUWP` (un seul processus ; `GetLongPathName` ne lit pas les dossiers parents d'une app).
- **Résultat (2026-10-18)** : `INFO:CONSOLE(1)] "Rhenium: JS 42"` dans le journal : la page se charge et son JavaScript s'exécute sur le Lumia.
- **Lecture du journal** : chaque export crée un nouveau fichier `rhenium-<lancement>-<n>.log` et supprime le précédent, car MTP tronque la copie d'un fichier qui a grossi depuis qu'il l'a vu. Exceptions notées dès leur levée (thread, pile avec noms de DLL), messages `OutputDebugString` et `abort()` captés.- **Démarrage sans user32** (inventaire des chemins Windows de `content_shell`) : écran `aura::TestScreen`, fenêtre `ui::StubWindow`, `SingletonHwnd` jamais créé, ni `InitCommonControlsEx`, ni DPI de bureau, ni OLE, ni module ATL, ni thème sombre, ni fenêtre de notification des périphériques ; préférences de polices et de barres de défilement fixes ; paramètres de lissage des polices fixes.
## Étape 4 : réseau et TLS

**Atteinte le 2026-10-18**, sans correctif : la page de test (`app\shell_uwp_main.cc`) interroge `https://fr.wikipedia.org/w/api.php?...&origin=*`. Journal : `Rhenium: HTTP 200` 375 ms après le JavaScript, `Rhenium: site Wikipédia` (JSON décodé), puis la page d'accueil de fr.wikipedia.org se charge et ses scripts tournent (message de console de `load.php`). DNS, TCP, TLS (certificat vérifié, sinon la requête échoue) et HTTP passent avec la capability `internetClient`.

Reste : le cache HTTP sur disque ne s'ouvre pas (`Unable to open or create cache entry`), sans effet sur le chargement.
## Ce qui change d'échelle

J1 ne touchait que V8 et //base. `content_shell` tire Blink, `//content`, `//net`, Skia, `//cc`, `//components/viz`, `//gpu`, `//ui`, `//media`, `//services`, Mojo, SQLite, ICU complet, ANGLE et une centaine de dépôts tiers : des dizaines de milliers de fichiers, plusieurs heures de compilation, un exe de plus de 100 Mo. Machine de build : Core Ultra 7 265KF (20 cœurs), 32 Go de RAM. F: a été agrandi à 150 Go (129 libres) : sources et sorties restent dans `F:\Rhenium\chromium`.

## Ce qu'il faut remplacer sous UWP

`content_shell` sous Windows passe par Aura, une fenêtre HWND et GDI. Rien de cela n'existe pour une app W10M (user32 n'exporte aucune fonction de fenêtres, vérifié en J1).

| Couche | Code M116 sous Windows | Sous UWP (Rhénium) |
| --- | --- | --- |
| Point d'entrée | `wWinMain` puis `ContentMain` (`content/shell/app`) | App `CoreApplication` (vue `IFrameworkView`) : `Run()` appelle `ContentMain`. La boucle du thread UI est le `RunLoop` de content, qui tourne par `CoreDispatcher::ProcessEvents` (pompe J1, `Run()` imbriqué vérifié sur le téléphone) |
| Processus | Navigateur, rendu, GPU, réseau, utilitaires dans des processus séparés | `--single-process` et `--in-process-gpu` : rendu, GPU (viz), réseau et utilitaires en threads. Lancement de processus et sandbox (`sandbox/win`) jamais appelés ; liés en chargement différé |
| Fenêtre | `ui/platform_window/win` (`WinWindow`, HWND), `ui/display/win` (moniteurs, user32) | `PlatformWindow` sur la `CoreWindow` (taille, DPI via `DisplayInformation`), écran unique |
| Affichage | Compositing logiciel : `SoftwareOutputDeviceWin` (GDI, `BitBlt` sur le HWND) | `SoftwareOutputDevice` qui présente le bitmap de Skia dans une swap chain D3D11 de la `CoreWindow` (feature level 11_1 mesuré en J0) |
| Entrées | Messages `WM_*` (`ui/events/win`) | Événements de la `CoreWindow` (pointeur, clavier) convertis en `ui::Event`. Toucher et défilement tactile : J3 |
| Polices | DirectWrite (Skia et proxy de polices de content) | DirectWrite, disponible pour les apps : à vérifier sur le téléphone |
| Réseau | Winsock, DNS (registre, iphlpapi), proxy (WinHTTP), changement de réseau (iphlpapi) | Winsock gardé ; DNS par `getaddrinfo` ; proxy direct ; notifications de réseau en bouchon |
| TLS | BoringSSL, vérification des certificats par CryptoAPI | BoringSSL ; vérificateur intégré et Chrome Root Store embarqué, sans CryptoAPI |
| Stockage | Cache disque, cookies (SQLite) | Dans LocalState ; SQLite sur les API de fichiers déjà validées |
| Médias, audio | FFmpeg, WASAPI par `IMMDeviceEnumerator` | Désactivés ou en bouchon (audio factice) pour J2 |
| Presse-papiers, curseur | user32 | Bouchons pour J2 |
| V8 | Compilé dans l'arbre Chromium | Correctifs J1 (`..\j1\patches`) appliqués à `src\v8`, JIT W^X |

## Sources : ce qu'il faut et comment (étude du 2026-10-16)

Sur les 482 entrées du `DEPS` de la balise, 124 sont actives sous Windows. Tailles mesurées sur les archives gitiles :

| Poste | Téléchargé | Sur disque (estimé) |
| --- | --- | --- |
| `chromium/src` utile (base, build, cc, components, content, gpu, media, mojo, net, services, ui, Blink sans `web_tests`, tiers internes…) | ~280 Mo | ~1,3 Go |
| Dépôts externes indispensables (V8, Skia, ANGLE, Dawn, ICU, HarfBuzz, FreeType, BoringSSL, QUICHE, WebRTC, Perfetto, SQLite, OTS, libjpeg-turbo, libwebp, libyuv, Vulkan…) | ~420 Mo | ~2 Go |
| **Total minimal** | **~0,7 Go** | **~3,3 Go** |

À ne pas prendre : `third_party/blink/web_tests` (496 Mo), `chrome/` (179 Mo, sauf `chrome/VERSION` et `version.gni`), catapult (159 Mo), devtools-frontend (92 Mo), SwiftShader, perl, tflite, pdfium, openh264, hunspell, NaCl, libc++.

**Méthode** : archives gitiles comme en J1, plutôt que gclient. gclient cloner `chromium/src` en entier (web_tests compris) et ses hooks téléchargeraient la toolchain Google, clang et Rust, inutiles ici. La liste des dépôts externes est produite depuis le `DEPS` par un script (évaluation restreinte, sans exécuter le `DEPS`), avec les `recursedeps` d'ANGLE, Dawn, vulkan-deps et openscreen. Fichiers normalement produits par les hooks, ajoutés à la main : `rc.exe` de Chromium (607 Ko, son `rc.py` le compare à celui du SDK), `gpu_lists_version.h`, `skia_commit_hash.h`, `DAWN_VERSION`. V8 : le même commit qu'en J1 (`47bf74e7`).

**Arguments GN** en plus de ceux de //base : `enable_plugins=false`, `enable_ppapi=false`, `enable_printing=false`, `enable_pdf=false`, `media_use_ffmpeg=false`, `media_use_libvpx=false`, `enable_dav1d_decoder=false`, `enable_library_cdms=false`, `enable_swiftshader=false`, `enable_swiftshader_vulkan=false`, `enable_vulkan=false`, `use_dawn=false`, `enable_screen_ai_service=false`, `use_viz_debugger=false`, `toolkit_views=false` (Aura sans Views), `proprietary_codecs=false` (déjà le défaut).

**Patchs prévisibles** :
- `content_shell` est `testonly` et tire les tests web (`support_web_tests`, simple variable) : à couper ;
- Crashpad (`crashpad_handler` en `data_deps`) : à retirer ;
- le pak embarque devtools, les WebUI en TypeScript (node) et `about:tracing` (catapult), sans option GN : à retirer du pak ;
- codecs : ffmpeg, libvpx, libaom et dav1d n'ont pas de configuration `win/arm`, d'où les arguments ci-dessus.

## Étapes

1. **Sources** : extraction de M116 suffisante pour `content_shell` (méthode et taille selon l'étude en cours : gclient sans historique ou archives gitiles comme en J1), dans `F:\Rhenium\chromium`. Critère : `gn gen` passe pour `//content/shell:content_shell` en `winuwp` ARM32, avec les arguments qui allègent (pas de NaCl, PDF, impression, codecs propriétaires…).
2. **Compilation en masse** : `ninja -k 0`, tri des échecs par domaine comme pour //base (tiers, Blink, puis couche plateforme). Correctifs regroupés par domaine dans `patches\`. Critère : la bibliothèque `content_shell` se construit.
3. **Démarrage** : coquille `CoreApplication` qui lance `ContentMain` en single-process, sans affichage. Imports vérifiés sur le téléphone (sonde `-ImportCheck`), journal de démarrage dans LocalState. Critère : `about:blank` se charge, le journal le montre.
4. **Réseau et TLS** : chargement d'une page HTTPS, octets et état TLS journalisés. Critère : `https://fr.wikipedia.org` répond 200, certificat vérifié.
5. **Affichage** : `PlatformWindow` et `SoftwareOutputDevice` UWP. Critère : **Wikipédia s'affiche** à l'écran du Lumia (capture).

## Risques

- **Volume** : le correctif de plateforme va grossir bien au-delà de celui de //base. Garder un correctif par domaine et préférer des fichiers `*_uwp.cc` neufs aux `#if` dans le code existant.
- **Exceptions C++** : Chromium n'en utilise pas, mais certains tiers peut-être ; LLVM ne les gère pas sur Windows ARM32.
- **Taille de l'exe et mémoire** : un exe ARM32 de plus de 100 Mo, chargé dans une limite de 1 536 Mo par app.
- **Temps de build** : premières compilations de plusieurs heures ; `symbol_level` minimal, composants inutiles coupés.
- **ARM32 jamais compilé pour Windows** dans Skia, Blink, ANGLE, libvpx… : de nouvelles branches `_M_ARM` à prévoir, comme pour //base.
