# Montée de Chromium : de 116 à M156

Décision de l'utilisateur du 2026-11-07 : quitter Chromium 116.0.5845.228 pour une M15x qui reçoit des correctifs de sécurité, quoi qu'il en coûte. Point de départ : l'étude de faisabilité du 2026-10-31 (M156 chiffrée à 40 à 70 jours).

## État au 2026-11-09 : paliers 1 à 6 atteints, v1 comprise

| Palier | Contenu | État |
| --- | --- | --- |
| 1 | V8 15.6 seul dans la sonde, JIT avec W^X, banc de J1 | **atteint le 2026-11-07** : six configurations correctes sur le Lumia, JIT multi-thread compris |
| 2 | `//base` avec Rust, `base_unittests` sur le Lumia | **atteint le 2026-11-08** : test de fumée et test UI sans échec, 1 370/1 370 tests de base_unittests |
| 3 | content_shell (J2) | **atteint le 2026-11-09** : Wikipédia s'affiche et la navigation tient sur le Lumia, avec uBO Lite et PDF.js chargés |
| 4 | navigation tactile, vidéo (J3) | **atteint le 2026-11-09** : toucher, saisie, rotation, clavier ; YouTube en VP9 360p avec le son (décodage moyen 6 à 7 ms par trame) |
| 5 | extensions, uBO Lite, onglets (J4) | **atteint le 2026-11-09** : uBO Lite filtre (réseau et cosmétique), onglets avec aperçus, session rouverte |
| 6 | profil, favoris, téléchargements, PDF, version pour ordinateur, mode sombre (J5) | **atteint le 2026-11-09** : profil, favoris, téléchargements, PDF.js, puis la v1 (`9f91568`) reportée et validée : version pour ordinateur, mode sombre, ajout depuis le Chrome Web Store |

## Palier 1 : V8 15.6 sur le Lumia 950

V8 15.6.75.36 (`v8_monolith`, clang 24 de Chromium, STL MSVC 14.44, C++23, sans Rust ni Intl ni Wasm) tourne dans la sonde sur le Lumia 950 (W10M 15254.603). Les six configurations passent, une par lancement : JIT complet multi-thread, JIT complet mono-thread, jitless, Sparkplug, Maglev et TurboFan seuls. Les 15 tests du script donnent les mêmes résultats partout.

```powershell
./fetch-v8.ps1                                   # F:\Rhenium\chromium-next\v8
./build-v8.ps1                                   # applique patches\, gn gen, ninja v8_monolith
..\sonde\build.ps1 -WithV8 -V8Next               # puis ..\sonde\deploy.ps1 -Ip 127.0.0.1 -Appx out\RheniumSonde_arm-clang-v8.appx
```

Banc de J1 (un lancement par configuration, tours 2 et 3, en ms) :

| Configuration | V8 15.6 | V8 11.6 (J1) | Attentes W^X : nombre, plus longue | Pages ouvertes sur faute |
| --- | --- | --- | --- | --- |
| JIT complet multi-thread | 170 et 326 | 361 à 461 | 14, 0,65 ms | 292 |
| JIT complet mono-thread | 324 et 183 | 310 à 391 | 0 | 131 |
| jitless | 730 et 656 | 861 à 1 456 | 0 | 0 |
| Sparkplug seul | 492 et 436 | 659 à 751 | 4, 0,51 ms | 52 |
| Maglev seul | 191 et 441 | 875 à 1 096 | 16, 0,60 ms | 156 |
| TurboFan seul | 182 et 513 | 366 à 457 | 0 | 127 |

V8 15.6 va au moins aussi vite que 11.6 sur ce banc, et Maglev apporte enfin un vrai gain sur ARM32. La plus longue attente W^X tombe de 1,8 ms (J1, pages de 4 Ko) à 0,65 ms. Aucune écriture hors portée. Les pointes isolées (crible à 250 ms au tour 3) suivent un `gc()` complet ; un seul lancement par configuration, à confirmer.

## Palier 2 : //base 156 avec Rust sur le Lumia 950

`//base` de la 156.0.8078.88 (target_os `winuwp`, famille `desktop` comme en J1-base, clang 24, STL MSVC 14.44, SDK 19041) se compile avec Rust : le Rust de Chromium (`rustc 1.100.0 1edd55dc`, `F:\Rhenium\toolchains\rust-156`, jonction `third_party\rust-toolchain`) recompile son std pour `thumbv7a-uwp-windows-msvc`, et `JSONReader` passe par `serde_json_lenient`. Sur le Lumia (2026-11-07 et 08) :

- test de fumée : 0 échec en 144 ms (chemins, fichiers, JSON lu par Rust, ruche HKCU, Thread, RunLoop, ThreadPool) ; test UI sur le `CoreDispatcher` : 0 échec ;
- imports : 280, tous présents sur le téléphone (`build.ps1 -ImportCheck`), y compris ceux qu'apporte le std de Rust (`WaitOnAddress`, console, `NtWriteFile`) ;
- base_unittests (45 fichiers, comme en J1-base) : 1 370 réussis sur 1 372 en un seul lancement de 27 s, sans plantage. Les deux échecs viennent de `PathServiceTest` : `DIR_GEN_TEST_DATA_ROOT` cherche un fichier généré qui n'est pas sur le téléphone, et `SetTestDataRootAsRelativePath` a révélé un vrai bug (le fournisseur UWP ignorait `DIR_SRC_TEST_DATA_ROOT`, donc `NOTREACHED`). Le fournisseur est corrigé et les deux tests sont exclus comme les autres tests qui demandent des données de test. Relancement : 1 370/1 370, 0 plantage.

```powershell
./fetch.ps1                                      # arbre partiel, branche git « origine »
./gen-loop.ps1; ./ninja-loop.ps1                 # récupèrent les fichiers manquants jusqu'à ce que gn et ninja passent
./build-base.ps1                                 # patches\0003, 0004, args.gn, ninja, liste des .rlib
..\sonde\build.ps1 -WithBase -BaseNext           # ou -BaseTests -BaseNext ; puis ..\sonde\deploy.ps1
```

Les correctifs : `patches\0003-chromium-build-winuwp-arm32.patch` pour `build\` et `third_party\`, `patches\0004-base-winuwp-arm32.patch` pour `base\` (ceux de J1-base rebasés : chemins par ApplicationData, ruche HKCU privée, `launch_uwp.cc`, `message_pump_uwp`, plus les retouches ARM32 propres à la 156).

## Palier 3 : content_shell sur le Lumia 950

Arbre partiel complété par `fetch.ps1 -Content` : les dossiers de `..\j2\fetch.ps1`, les dépôts du DEPS au fil de `gen-loop.ps1` et `ninja-loop.ps1 -Build`, et ce qui ne vient pas de git (Node et `node_modules`, `clang-format`, `tsc`, le Go de Dawn, par GCS et CIPD). Les correctifs de J2 rebasés vont dans `patches\content\0005-content-shell-winuwp.patch` ; `regen-patches.ps1` les régénère tous (0002 compris) depuis l'arbre.

```powershell
./fetch.ps1 -Content
./build-content.ps1 -GenOnly
./ninja-loop.ps1 -Build -OutDir out\content-arm -Targets rhenium_content -Log $env:TEMP\ninja-content.log
```

SDK : en-têtes du 26100, bibliothèques ARM32 du 19041 (le dernier à en livrer). La 156 attend le 28000 et utilise des déclarations absentes du 19041 (`IDWriteFontSet4`, `COMPOSITION_FRAME_STATS`, `IP_RECVECN`…). `rhenium_winsdk_include_version` (GN) et `RHENIUM_WINSDK_INCLUDE_VERSION` (`setup_toolchain.py`) séparent les deux ; `NTDDI_VERSION` suit les en-têtes. Le contrôle des imports (`..\sonde\build.ps1 -ImportCheck`) ne trouve aucune fonction manquante : 922 imports présents sur le Lumia, 168 en chargement différé (USER32, WINMM, dhcpcsvc, pdh, wevtapi, dwmapi, UxTheme), sans effet tant qu'ils ne sont pas appelés.

Le code de l'app passe par deux surcouches, pour que le build 116 de `..\j2` reste intact : `app\` est copié par-dessus `..\j2\app` dans `src\rhenium` (sources portées vers l'API 156, `arm32_intrinsics.cc`), et `gn\content.gn` remplace `..\j2\gn\BUILD.gn` comme racine GN. `rhenium_content` : 9 983 étapes sans échec ; `rhenium_shell.exe` passe ensuite par `fix_vcall_thunks.py` (140 relais corrigés avec clang 24) et `..\j2\package-shell.ps1`.

Pour lancer ninja à la main, définir `RHENIUM_WINSDK_INCLUDE_VERSION=10.0.26100.0` : sans elle, la régénération automatique de ninja reprend les en-têtes du 19041 et tout recompile.

## Version visée

Relevé sur chromiumdash le 2026-11-07 (Windows) :

| Canal | Version | Date | V8 |
| --- | --- | --- | --- |
| Stable | 158.0.8127.18 | 2026-11-04 | 15.8.62.9 (`447efb37`) |
| Extended | 156.0.8078.88 | 2026-11-03 | 15.6 |

Cible choisie par l'utilisateur le 2026-11-07 : **156.0.8078.88**, l'Extended. La 158 avait été proposée (stable du jour, Extended de janvier à mars 2027) ; l'Extended a été préférée, plus facile à tenir. Les versions mineures de la 156 sont des reprises de correctifs, à appliquer en fin de parcours ; si la M156 quitte l'Extended avant la fin, passer à la 158 par le même chemin.

Ce que la 156.0.8078.88 exige (lu dans l'arbre à la balise) :

| | M156 |
| --- | --- |
| clang | `llvmorg-24-init-7747-g62397f8b-53` (paquet Google) |
| SDK (`setup_toolchain.py`) | 10.0.28000.0 |
| Rust | épinglé `1edd55dc`, cible `arm` Windows absente de `rust.gni` |
| V8 | 15.6.75.36 (`d72e817d`) : backend `arm` présent, `WritableJitAllocation`, `WritableJitPage`, `ThreadIsolation` ; plus de drapeau de W^X par changement de protection |
| GN, ninja (DEPS de V8) | `127dd2a6`, 1.12.1.chromium.4 |

## Dossiers

- `F:\Rhenium\chromium-next\v8` : V8 15.6 seul pour le palier 1 (`fetch-v8.ps1`), comme `F:\Rhenium\v8` en J1.
- `F:\Rhenium\chromium-next\src` : arbre de la 156, à partir du palier 2, récupéré à part. `F:\Rhenium\chromium` reste celui de la 116, qui n'est plus le build publié.
- `maj-chromium\` : cette note, les scripts et les correctifs (`patches\0001-v8-build-win-arm32.patch` pour `build\`, `patches\0002-v8-win-arm32.patch` pour `include\` et `src\` de V8 ; `0003` et `0004` pour l'arbre de Chromium), `gn\` (racine GN de Rhénium, tests de //base adaptés à la 156).
- `F:\Rhenium\toolchains\clang-156` : clang de Chromium (`llvmorg-24-init-7747-g62397f8b-53`, paquet `Win/clang-…tar.xz` de `commondatastorage.googleapis.com/chromium-browser-clang`).

## Choix

| Choix | Raison |
| --- | --- |
| V8 seul au palier 1, arbre partiel de Chromium ensuite | comme J1 puis J1-base : le risque du JIT d'abord, isolé ; `j2\deps.py` sert aux deux |
| clang 24 de Chromium d'abord, clang 23.1.3 en repli | le code de la 156 est écrit pour clang 24 ; le paquet Google tourne sur un hôte x64 et vise ARM32 |
| Palier 1 sans Rust (`v8_enable_temporal_support=false`) | séparer le risque du JIT de celui de Rust |
| W^X par portée et par faute (voir plus bas) | V8 15 encadre toute écriture dans le code par `RwxMemoryWriteScope`, comme sur les Mac M1 ; on émule ce modèle plutôt que de retoucher le tas |
| STL MSVC 14.44 conservée, C++23 | une seule STL pour la coquille C++/WinRT et le moteur ; trois écarts seulement en V8 15.6 (ci-dessous) |
| Clang de Chromium sans `clang_rt.builtins` ARM32 | le paquet n'en a pas ; J1 s'en passait déjà |

## W^X en V8 15.6

V8 12 et suivants n'ont plus `--write-protect-code-memory`. Hors PKU et Apple, ils demandent leurs pages de code en RWX et toute écriture dans le code passe par une `RwxMemoryWriteScope` (directement, ou par `WritableJitAllocation`, `WritableJitPage`, `WritableFreeSpace`). Sur Mac M1, cette portée bascule le thread courant en écriture (`pthread_jit_write_protect_np`) ; V8 s'y plie donc partout.

Rhénium émule ce modèle dans `base/platform/platform-win32.cc` (ARM32 seulement) :

- une demande RWX donne une page RX, marquée « code » (un bit par page de 4 Ko, comme en J1) ;
- `RwxMemoryWriteScope::SetWritable` et `SetExecutable` (`common/code-memory-access-inl.h`) appellent `base::EnterJitWriteScope` et `ExitJitWriteScope` : un compteur de profondeur par thread ;
- dans une portée, une faute d'écriture sur une page de code la passe en RW (gestionnaire vectorisé), compte le thread comme écrivain de la page et reprend l'instruction ;
- à la sortie de la portée la plus externe, chaque page tenue repasse en RX quand plus aucun thread ne la tient ;
- un thread qui exécute une page en RW attend son retour en RX (`WaitForCodePage` de J1) ;
- une écriture hors portée plante, comme sur Mac M1 ; la sonde affiche leur nombre et la dernière adresse fautive.

Seules les pages réellement écrites basculent, sans liste d'étendues à tenir à jour : c'est le « par pages de 4 Ko » de J1, obtenu sans toucher au tas. Le prix est une exception par page ouverte ; s'il pèse, `WritableJitAllocation` connaît son étendue et pourra ouvrir ses pages d'avance.

## Pièges

| Symptôme | Cause | Correctif |
| --- | --- | --- |
| `gn gen` : `third_party/icu/config.gni`, `protobuf`, `fuzztest`… introuvables | V8 seul réclame les fichiers de build de dépôts qu'il ne compile pas | `fetch-v8.ps1 -Missing` ne prend que le fichier `.gn`/`.gni` ; `gen-loop-v8.ps1` enchaîne |
| `gn gen` : `rust-toolchain/VERSION` | `enable_rust` vrai par défaut | `enable_rust=false` au palier 1 |
| `gn gen` : `clang_lib` sans cas pour Windows ARM32 | pas de runtime clang ARM32 dans le paquet | `build/config/clang/BUILD.gn` : config vide sur win/arm |
| `gn gen` : `expand_directory` refusé | modules clang unifiés, actifs pour l'hôte (`use_custom_libcxx_for_host`) | `use_custom_libcxx_for_host=false`, `use_clang_modules=false` |
| ninja : `llvm-libclang/.../cindex.py` | `metagen` analyse les en-têtes par libclang | `v8_use_metagen_instance_types=false` (sortie de Torque) |
| ninja : `icudtl.dat` | le snapshot dépend des données ICU | `v8_depend_on_icu_data_file=false` |
| `_CountLeadingZeros` non déclaré | STL MSVC ARM, intrinsèque que clang ne déclare que pour ARM64 (déjà vu en J1-base) | `build/rhenium/msvc_arm32_intrinsics.h` inclus de force |
| `std::span<const CFunction>` : type incomplet | en C++23, `span` de la STL MSVC exige un type complet dès la déclaration | `include/v8-template.h` inclut `v8-fast-api-calls.h` sous la STL MSVC |
| `FunctionRef` refuse un `std::function` | l'`operator()` de `std::function` (STL MSVC) est hérité d'une base | `bind-internal.h` : spécialisation sur toute classe propriétaire |
| Torque : `JSInterceptorMap` décalé de 3 octets | V8 loge les champs de la sous-classe dans le remplissage de fin d'`ExtendedMap` ; l'ABI Microsoft ne réutilise jamais ce remplissage | `ExtendedMap` déclare ces octets sous `V8_CC_MSVC`, `JSInterceptorMap` s'en sert ; Torque n'affirme plus ces deux dispositions sous MSVC |
| `IsUserCetAvailableInEnvironment` non déclaré | SDK 19041 | `UserShadowStackEnabled` rend faux sur ARM32 |
| `.size` refusé dans `push_registers_asm.cc` | directive ELF en COFF | retirée sous `_WIN32` |
| `gn gen` : triple Rust inconnu, `win_toolchain_data` sans cas `arm` | ARM32 Windows absent de Chromium | `known-target-triples.txt`, branche `arm` dans `win_toolchain_data.gni` |
| `gn gen` : `nocompile.gni`, `system_allocator_test_helper` | tests de non-compilation et toolchain `win_clang_arm_with_system_allocator` inexistants pour Windows ARM32 | `enable_nocompile_tests=false` ; test exclu pour `winuwp` dans `base/BUILD.gn` et `base/allocator/BUILD.gn` |
| `fetch.ps1 -Missing chrome/VERSION` rapatrie tout `chrome/` (860 Mo) | un chemin sans fichier de build était pris pour un dossier | le fichier seul est récupéré ; le `chrome/` déjà là est resté dans `origine` |
| std de Rust : `miniz_oxide`, `addr2line`, `object` absents | gnrt traduit `cfg(not(all(windows, msvc, not(uwp))))` par `!is_win`, or le std UWP en a besoin | `build/rust/std/rules/BUILD.gn` : `!is_win` devient `(!is_win \|\| current_os == "winuwp")` |
| assembleur BoringSSL refusé par `llvm-ml` | fichiers `.S` ARM32 au format GNU | `OPENSSL_NO_ASM` pour win/arm |
| `__rdtsc`, `int3` | x86 seulement | Perfetto : branche ARM ; allocateur Rust : `udf #0` |
| erreur de module dans `precompile.cc` | en-têtes précompilés avec la STL MSVC | `enable_precompiled_headers=false` |
| `fuzztest.h` introuvable | fuzztest non porté | `FUZZ_TEST` vide (`gn\fuzztest.h`), seules les variantes fuzz sont sautées |
| lien : les `.rlib` manquent dans `rhenium_base.lib` | GN ne passe les `.rlib` qu'au lien d'un exécutable, jamais dans une `static_library` | exécutable témoin `rhenium_rust_libs` : `ninja -d keeprsp` garde son fichier de réponse, `build-base.ps1` en tire la liste |
| lien : `__tls_used`, `_perfetto_thread_callback_base` | Perfetto décore comme x86 hors `_WIN64` | `platform_windows.cc` : pas de souligné sur ARM32 |
| lien : `__gnu_f2h_ieee`, `__gnu_h2f_ieee` | le core de Rust appelle les conversions f16 de compiler-rt, absent du paquet clang | `rhenium_builtins` compile `extendhfsf2.c` et `truncsfhf2.c` |
| test : `SEH exception 0xc000001d` | `NOTREACHED()` atteint : sur ARM32, `ImmediateCrash` est un `udf`, d'où une instruction illégale | chercher le `NOTREACHED` ; ici `DIR_SRC_TEST_DATA_ROOT` manquait au fournisseur UWP de `base_paths_win.cc` |
| la sonde semble ne rien écrire | copie MTP asynchrone, interrompue trop tôt | attendre l'arrivée de chaque fichier |
| `ninja -n` passe, la vraie compilation réclame des fichiers | des actions lisent des fichiers non déclarés | `ninja-loop.ps1 -Build` boucle sur la vraie compilation |
| `tflite` obligatoire, `devtools-frontend` entier | la 156 ne sait plus s'en passer | récupérés en entier |
| `gn gen` : dépendances `views` avec `toolkit_views=false` | `!is_android` utilisé pour dire « vues » | `if (toolkit_views)` dans `user_education/webui` et `ui/webui/tracked_element` |
| tint, en-têtes SPIR-V | Graphite (Dawn) dans Skia | `skia_use_dawn=false` et le code GPU qui suppose Dawn gardé par `SKIA_USE_DAWN` ; `spirv-headers` récupéré pour WebNN |
| `calling 'begin' with incomplete return type` (260 fichiers) | la STL MSVC évalue `noexcept(std::begin(String))` | `wtf_string.h` inclut `code_point_iterator.h` |
| `reverse_iterator`, `find_if`, `std::less<raw_ref>` refusés | STL MSVC plus exigeante que libc++ sur les itérateurs | boucles explicites, `NodeIterator` sans membres `const` |
| `#if V8_TARGET_OS_WIN` : expression vide | `winuwp` n'a pas de `V8_HAVE_TARGET_OS`, v8config.h définit la macro sans valeur | `#if defined(...)` aux trois endroits |
| `embedded.S` : `.hidden`, `.type` refusés | mksnapshot ne connaît pas `winuwp` et sort de l'ELF | `--target_os=win` (déjà en 116) |
| crashpad : `Unsupported Windows Arch` | `crash/core/app` dépend en 156 de `handler:common` | retiré sous `winuwp`, comme le gestionnaire en 116 |
| encodeurs AV1 : `ratectrl_rtc.h` | libaom désactivé (pas de config ARM32 Windows) | sources AV1 sous `enable_libaom`, appels sous `ENABLE_LIBAOM` |
| compilation tuée vers 05 h 30, `0xC0000409` en série | redémarrage de Windows Update | suspendre les mises à jour pendant les longues compilations |
| lien : `DawnSharedTextureCache`, `GetD3D12Device`… | le code d'images partagées appelle Dawn même sans `skia_use_dawn` | `dawn_stubs_winuwp.cc` (gpu/command_buffer/service) : corps vides ou `NOTREACHED` |
| lien : `IID_IMFCameraControlNotify`, `CLSID_CUIAutomationClientInfoSource` | GUID absents des `.lib` du 19041 | définis sous `WINUWP` dans les fichiers qui s'en servent |
| lien : `_AddressOfReturnAddress` | intrinsèque MSVC que clang ne fournit pas sur ARM32 | `app\arm32_intrinsics.cc`, par `__builtin_frame_address(1)` |
| lien : `SetFilterETWSessionByURL2Callback`, `cpuinfo_isa` | ETW coupé dans V8 ; cpuinfo ne définit pas la variable pour ARM32 Windows | appel sous `!defined(WINUWP)`, variable définie dans `init.c` |
| une retouche d'en-tête annulée recompile quand même Blink | ninja compare les dates, pas le contenu | remettre l'ancienne date (`LastWriteTime`) au fichier restauré |
| plantage au démarrage dans `webstore_private_api.cc` | la 156 construit la fabrique de `webstorePrivate` avec le délégué du Web Store, que seul `chrome/` fournit | délégué facultatif dans le constructeur |
| l'app revient à l'accueil sans trace, 3 à 4 s après le lancement | une fonction de bureau absente (`GetCaretBlinkTime`, puis `GetSysColor`…) appelée en chargement différé : le crochet d'échec de KERNELBASE bloque le thread, le système tue l'app | `app\delay_load_uwp.cc` définit `DelayLoadFailureHook` : retour de 0, fonction nommée au journal |
| pointeur nul dans `PermissionsUpdater::ApplyPolicyHostRestrictions` | la 156 interroge le client de gestion des extensions (politiques), nul par défaut | `RheniumExtensionManagementClient`, sans politique |
| `Check failed: !g_client` (extensions_client.cc) | navigateur et rendu installent le client d'extensions dans le même processus ; la 116 ignorait le second appel | `SetExtensionsClientOnce` |
| `Module resource registered as "permissions" not found`, au démarrage du service worker de uBO Lite | liaisons JS des API rangées dans un nouveau paquet | `extensions_renderer_generated_resources.pak` ajouté à `rhenium_extensions.pak` |
| arrêts sans trace dans le journal | `__fastfail`, `_exit` ou blocage tué par le système | `app\log_export_uwp.cc` : gestionnaires CRT, `terminate`, `exit`, sortie d'erreur (paniques de Rust), et journaux du lancement précédent joints au suivant |
| PDF.js : ses propres fichiers refusés (`ERR_BLOCKED_BY_CLIENT`) | la page d'extension tombe dans le SiteInstance par défaut, jamais inscrite dans la ProcessMap | `DoesSiteRequireDedicatedProcess` vrai pour les extensions, comme Chrome |
| uBO Lite filtre à moitié, son worker ne démarre pas côté rendu, puis `Error: No SW` | la 156 relie service workers et cadres par leurs propres interfaces associées | `RegisterAssociatedInterfaceBindersForServiceWorker` (RendererHost, ServiceWorkerHost, EventRouter) et les mêmes pour les cadres |
| ajout depuis le Chrome Web Store : `Check failed: delegate` (management_api.cc) | la 156 passe par `chrome.management` à chaque installation | `RheniumManagementAPIDelegate` : activation et suppression par le gestionnaire de Rhénium, sans dialogue ni applications |
| « Ajouter à Rhénium » disparaît du menu jusqu'au relancement | déjà en 116 : pour une extension déjà installée, `InstallFromWebStore` prévenait la coquille puis répondait « lancée » | réponse fausse dans ce cas |
| « Version pour ordinateur » laisse fr.m.wikipedia.org en mobile | la 156 n'a plus `ReloadType::ORIGINAL_REQUEST_URL` | page redirigée vers un hôte `m.` du même domaine : rouverte à son adresse d'origine par `LoadURLWithParams` ; sinon rechargement simple |
| Web Store en version mobile, sans « Ajouter à Rhénium », après la page de cookies de Google | l'agent utilisateur n'était choisi qu'au début de la navigation, pas à la redirection de `consent.google.com` | `ChooseUserAgent` aussi dans `DidRedirectNavigation` |
| la version lancée n'est pas celle déployée, données de l'app effacées | `deploy.ps1` désinstallait une version égale ; sortie tronquée pendant que WinAppDeployCmd installait encore ; téléphone verrouillé (0x80070005) | `deploy.ps1` ne touche plus à une version égale ni plus récente et s'arrête si la liste échoue ; vérifier par `WinAppDeployCmd list` |

## Suite

- La v1 est reportée (`app\engine_uwp.cc`, `app\rhenium_engine.h`, `shell\shell_app.cpp`, `rhenium_tab_model` dans l'arbre). Toute retouche de la coquille ou du moteur se fait désormais ici ; `..\j2\shell\shell_app.cpp` et `..\j2\app` restent ceux de la 116.
- À surveiller : `Failed to create shared context for virtualization` (GPU, sans effet visible), les fonctions absentes remplacées (journal), le dossier `_metadata` de uBO Lite refusé au rechargement.
- Le correctif V8 (`patches\0002`) remplacera `j1\patches\0002` dans l'arbre de Chromium ; la question de l'ABI Microsoft (remplissage de fin) peut revenir ailleurs dans Chromium.
- À mesurer plus tard : deux lancements de plus par configuration ; coût des fautes d'écriture (292 par lancement en JIT complet) face à une ouverture d'avance par `WritableJitAllocation`.
