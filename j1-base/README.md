# Jalon J1, suite : plan du portage de Chromium //base sur Windows 10 Mobile

Objectif : compiler `//base` de Chromium M116 pour Windows 10 Mobile (ARM32, app UWP) avec la toolchain validée pour V8, puis le faire tourner sur le Lumia : threads, tâches, horloges, fichiers, JSON, et une boucle de messages branchée sur le thread XAML. C'est la seconde moitié du jalon J1 « Fondations » du cahier des charges, après V8 (`..\j1`).

Statut au 2026-10-16 : **`base.lib` se construit** pour Windows 10 Mobile ARM32 (option B : en-têtes de bureau, imports vérifiés sur le téléphone). Chemins, registre et processus sont adaptés à la source. **J1 atteint le 2026-10-16** : //base tourne sur le Lumia 950 (W10M 15254.603). Test de fumée sans échec, aucun import bloquant manquant, boucle UI sur le `CoreDispatcher` (tâches, tâches différées et `RunLoop` imbriqué sur le thread XAML), plus aucun import de USER32, et **1 201 tests de `base_unittests` sur 1 201 réussis** (étape 6, ci-dessous).

## Utilisation

```powershell
./fetch.ps1                    # sources dans ..\chromium\src (dépôt git local de référence, branche origine)
./fetch.ps1 -Missing <chemin>  # ajoute le dossier d'un fichier réclamé par gn gen
./build-base.ps1 -GenOnly      # correctifs de patches\, racine GN minimale (gn\), args.gn, gn gen
./build-base.ps1               # + ninja : rhenium_base.lib (//base + test de fumée), bibliothèques de tests
..\sonde\build.ps1 -WithBase   # sonde avec //base, test de fumée et test du thread UI
..\sonde\build.ps1 -BaseTests  # sonde avec le sous-ensemble de base_unittests
```

## Étape 1 : ce qui a été fait

- **Version** : 116.0.5845.228. Le GN de V8 (`..\v8\buildtools\win\gn.exe`) est exactement celui de cette version (`4bd1a77e`).
- **Sources** : 23 dossiers, 220 Mo, par archives gitiles. Pour les dépôts externes du `DEPS` qui ne servent qu'à résoudre des cibles (ICU, catapult, libxml, zlib, nasm, protobuf…), seuls les fichiers de build sont téléchargés. `fetch.ps1` trouve leur dépôt et leur révision dans le `DEPS` de la balise.
- **Fichiers normalement écrits par gclient** : `build/config/gclient_args.gni` (valeurs du `DEPS`, adaptées à l'arbre partiel) et `build/util/LASTCHANGE` (commit `b53ecfbe` de la balise).
- **Racine GN** : `gn\dot.gn` et `gn\BUILD.gn`, qui ne dépend que de `//base`. La racine de Chromium référence tout le navigateur.
- **Correctif `patches\0001-chromium-winuwp-arm32.patch`** : le correctif de build de V8 (`..\j1\patches\0001`), qui s'applique au `build/` de Chromium à une ligne près, et en plus :
  - `target_os = "winuwp"` passe par notre toolchain `win_clang_arm` (option `uwp`, `current_os = "winuwp"`) au lieu de la toolchain `uwp_*` figée sur MSVC ;
  - sous clang, `winuwp` n'active pas `/EHsc` (pas d'exceptions, que clang ne sait pas compiler pour ARM32) et garde la CRT statique ;
  - BoringSSL n'assemble pas avec NASM sur ARM32 (code C générique).
- **Arguments** : en plus de ceux du plan, `enable_backup_ref_ptr_support = false`, imposé quand PartitionAlloc ne remplace pas `malloc`.
- **Vérifié dans `obj\base\base.ninja`** : `--target=thumbv7-pc-windows-msvc`, `WINUWP`, `WINAPI_FAMILY=WINAPI_FAMILY_PC_APP`, `NTDDI_WIN10_RS2`, `/MT`, pas de `/EHsc`.

## Ce que l'on sait (recherche du 2026-10-16)

| Fait | Conséquence |
| --- | --- |
| La dernière M116 du canal Extended, **116.0.5845.228**, épingle dans son `DEPS` exactement notre V8 (commit `47bf74e7`, 11.6.189.22). La dernière stable, 116.0.5845.190, est encore sur le 11.6.189.20 | Base : **116.0.5845.228**, la version que Google a livrée avec ce V8 |
| `base/` : environ 3 400 fichiers, 24,6 Mo (dont 524 tests), 5,65 Mo en archive ; gitiles sert chaque dossier en `+archive/refs/tags/<tag>/<dossier>.tar.gz` | Téléchargement léger, dossier par dossier, comme pour V8 |
| Dépendances de `base` sous Windows : abseil-cpp, modp_b64, `base/third_party/double_conversion` et `dynamic_annotations`, `partition_allocator:raw_ptr`, BoringSSL (MD5/SHA1 seulement) ; perfetto seulement avec `enable_base_tracing` ; ni ICU, ni libevent | Liste courte à télécharger |
| Rust désactivé par défaut pour `//base` en M116 (`enable_rust_json`, `enable_rust_base_conversions` faux) | Pas de Rust nécessaire |
| Le build Chromium a une cible `target_os = "winuwp"`, seule voie vers ARM32 sous Windows, qui pose `WINUWP` et `WINAPI_FAMILY` (app ou phone) | On réutilise sa configuration |
| …mais sa toolchain `uwp_<cpu>` est figée sur MSVC (`is_clang = false`), alors que Chromium M116 ne compile plus qu'avec clang | La brancher sur notre clang-cl 23 (correctif du build, comme `0001` pour V8) |
| Aucun code `WINUWP` ni `WINAPI_FAMILY` dans `base/` | Toute la couche plateforme UWP est à écrire |
| Seul précédent public : Edge Chromium sur HoloLens 2, « application Win32 native avec une couche d'adaptation UWP » | Pas de précédent de Chromium « pur UWP » ; sur W10M, pas de Win32 sous-jacent : on adapte `//base` lui-même |

## Étapes 2 et 3 : premières compilations

Trois passes de `ninja -k 0 base`. La première échouait sur 225 fichiers, dont l'essentiel en cascade à partir de causes communes, corrigées (`patches\0002-base-win-arm32.patch`, et l'en-tête de `0001`) :

| Cause | Correctif |
| --- | --- |
| `bind_internal.h` réserve à « Windows 32 bits » des spécialisations `__stdcall`/`__fastcall` ; sur ARM32 ces conventions sont ignorées et les spécialisations se dupliquent (126 fichiers) | Réservées à `ARCH_CPU_X86` |
| `int3`/`ud2` dans `immediate_crash.h` de `base` et de PartitionAlloc | `__debugbreak()` + `udf` sur ARM32 |
| La STL MSVC appelle `_CountLeadingZeros(64)` sur ARM, que clang ne déclare que pour ARM64 | `build/rhenium/msvc_arm32_intrinsics.h`, inclus de force (`/FI`) sur Windows ARM32 |
| `__rdtsc` dans les deux `time_win.cc` | Gardes ARM64 étendues à `ARCH_CPU_ARM_FAMILY` (`GetThreadTimes`) |
| Registres x86 de `CONTEXT` dans `profiler/register_context.h` | `Pc`, `Sp`, `R11` sur ARM32 |

### Les 73 fichiers restants après ces corrections (passe 3), par domaine

| Domaine | Fichiers | Ce qui bloque |
| --- | --- | --- |
| En-têtes communs | `win/windows_version.h` (8 fichiers), `windows_version.cc` | Constantes et contrôle exigeant le SDK 10.0.22621, alors que l'ARM32 impose le 19041 |
| COM et threads | `com_init_balancer`, `scoped_com_initializer`, `threading/thread.cc`, `thread_pool/*` (6 fichiers) | `IInitializeSpy`, réservé au bureau, tire tout le démarrage COM |
| Modules et chargement | `native_library_win`, `command_line`, `scoped_winrt_initializer`, `platform_thread_win`, `nt_status`, `scoped_handle_verifier`, `platform_shared_memory_region_win`, `profiler/module_cache_win`, `debug/profiler`, `handle_hooks_win`, abseil `time_zone_lookup` | `GetModuleHandle(Ex)`, `LoadLibraryEx`, `LOAD_LIBRARY_SEARCH_SYSTEM32` |
| Fichiers | `file_win`, `logging`, `sync_socket_win`, `security_util`, `file_enumerator_win`, `file_path` | `CreateFileW` (seul `CreateFile2` est exposé), `WIN32_FIND_DATA`, `CharUpperW` |
| Processus | `process_iterator*`, `kill*`, `launch_win`, `startup_information`, `process_metrics_win`, `process_handle_win` | Toolhelp (`PROCESSENTRY32`), `STARTUPINFOEX`, `QueryProcessCycleTime` |
| Fenêtres et boucle UI | `message_pump_win`, `message_window`, `wrapped_window_proc` | `WNDCLASSEX`, `WNDPROC`, `PostMessageW` |
| Énergie | `power_monitor_device_source*`, `battery_level_provider_win`, `speed_limit_observer_win` | `HPOWERNOTIFY`, `CallNtPowerInformation`, en-têtes de périphériques |
| Temps et aléa | `time_win.cc` (base et PartitionAlloc), `rand_util_win.cc` (base et PartitionAlloc), `partition_alloc_base/logging.cc` | `timeBeginPeriod`/`timeEndPeriod`, `RtlGenRandom`, `lstrlenA` |
| Registre, shell, WMI, sécurité | `registry`, `default_apps_util`, `win_util`, `shortcut`, `base_paths_win`, `wmi`, `access_*`, `security_descriptor`, `sid`, `resource_util`, `file_version_info_win`, `scoped_variant` | API du registre et du shell, WMI, ACL, ressources |
| Débogage, traces, profileur | `stack_trace_win` (dbghelp), `invalid_access_win`, `gdi_debug_util_win`, `event_trace_provider` (ETW), `pe_image*`, `iat_patch_function`, `native_unwinder_win`, `win32_stack_frame_unwinder`, `suspendable_thread_delegate_win` | dbghelp, ETW, et un profileur prévu pour x64 et ARM64 seulement |
| Divers | `atl_throw` (ATL absent), `com_init_util` (`TEB`), `object_watcher` (`UnregisterWaitEx`), `dark_mode_support` | |

## Option B retenue : en-têtes de bureau, vérification sur le téléphone

Deux options étaient possibles : `WINAPI_FAMILY_APP` strict (chaque appel de bureau réécrit, environ 73 fichiers) ou, comme pour V8, les en-têtes de bureau (`target_winuwp_family = "desktop"`) avec vérification de chaque import sur le Lumia. La seconde est retenue, avec la boucle UI, les processus, le registre et les chemins traités à la source puisqu'on sait qu'ils ne peuvent pas marcher dans une app.

Passe 4 (en-têtes de bureau) : 20 fichiers en échec, corrigés ainsi :

| Cause | Correctif |
| --- | --- |
| La config `winuwp` définit une macro `WIN10`, qui écrase `Version::WIN10` de `windows_version.h` (12 fichiers) | Macro non définie sous clang (`build/config/win/BUILD.gn`) |
| `windows_version.cc` exige le SDK 10.0.22621 | Contrôle réservé aux builds hors `WINUWP` |
| `stack_trace_win.cc`, `invalid_access_win.cc` : x86, x64 et ARM64 seulement | Branche ARM32 : `IMAGE_FILE_MACHINE_ARMNT`, `Pc`/`R11`/`Sp` ; `nop` Thumb de 2 octets, retour par `bx lr` |
| Dérouleur du profileur : x64 et ARM64 seulement | ARM32 traité comme x86 : compile, échantillonnage inactif |
| `registry.cc` : `fixed_flat_map` suppose des itérateurs `std::array` qui sont des pointeurs (libc++) | Tableau simple et `std::find_if` |
| `atl_throw.cc` : pas d'ATL pour ARM32 | `win/atl*` retirés des sources (seuls des tests incluent `win/atl.h`) |

## Adaptations à la source

| Domaine | Fichier | Sous UWP |
| --- | --- | --- |
| Chemins | `base_paths_win.cc` | Fournisseur réécrit : exécutable, Windows et système comme avant ; `DIR_LOCAL_APP_DATA` (et `DIR_COMMON_APP_DATA`) = LocalState, `DIR_ROAMING_APP_DATA` = RoamingState, via `Windows.Storage.ApplicationData` ; tout le reste (dossiers du shell) échoue |
| Chemins | `files/file_util_win.cc` | `GetHomeDir()` = LocalState ; jamais administrateur (`IsUserAnAdmin`) |
| Registre | `win/registry.cc` | `HKEY_CURRENT_USER` redirigé vers une ruche privée `LocalState\hkcu.dat` (`RegLoadAppKeyW`), pour `RegKey` et les itérateurs ; les autres racines restent en lecture comme avant |
| Processus | `process/launch_uwp.cc` (nouveau, remplace `launch_win.cc`) | `LaunchProcess` et `GetAppOutput*` échouent proprement : Rhénium est mono-processus |
| Boucle UI | `message_pump_win.cc`, `message_window.cc` | Pas encore modifiés : on attend la vérification des imports `user32` sur le téléphone avant d'écrire la pompe `CoreDispatcher` (étape 5) |

## Étape 4 : liaison dans la sonde

`gn\BUILD.gn` définit `rhenium_base`, une bibliothèque complète (`complete_static_lib`) qui regroupe //base, ses dépendances et le test de fumée `gn\base_smoke.cc` (copié dans `src\rhenium\` par `build-base.ps1`, compilé avec les options de //base, exposé par deux fonctions C). La sonde la lie avec `-WithBase`. Corrections apparues à l'édition de liens :

| Cause | Correctif |
| --- | --- |
| `/INCLUDE:__tls_used` et `_p_thread_callback_*` : le souligné initial des symboles est propre à x86, pas à ARM32 | Variante sans souligné aussi pour `_M_ARM` (base, PartitionAlloc, `dllmain.cc`, BoringSSL) |
| `process_metrics_win.cc` lit le TSC hors ARM64 | Garde étendue à `ARCH_CPU_ARM_FAMILY` |
| `timeBeginPeriod`/`timeEndPeriod` passent, comme `timeGetTime`, par kernel32legacy, absente de W10M | Remplacés dans la cale commune `..\j1\app\w10m_imports.cc` |

L'exe lié ne garde que ce que le test utilise : 0,8 Mo, 250 imports directs (api sets, plus `__C_specific_handler` de KERNEL32) et 31 en chargement différé (user32 pour la pompe UI, ole32, oleaut32, shlwapi, advapi32, dbghelp).

### Résultats sur le Lumia 950 (2026-10-16)

**Imports** (sonde `-ImportCheck`) : 281 vérifiés, **0 bloquant manquant**. `USER32.dll` se charge mais n'exporte aucune des 16 fonctions de fenêtres et de messages utilisées par `message_pump_win` et `message_window` (`CreateWindowExW`, `PeekMessageW`, `PostMessageW`, `SetTimer`, `MsgWaitForMultipleObjectsEx`…). Tout le reste existe : api sets, `__C_specific_handler`, `RegLoadAppKeyW`, ole32, oleaut32, shlwapi, advapi32, dbghelp.

**Test de fumée** (sonde `-WithBase`) : 0 échec, 147 ms.

| Domaine | Résultat |
| --- | --- |
| Système | `OSInfo` 10.0.15254.603, 6 processeurs, 2 609 Mo, `RandUint64` OK |
| Horloges | `Sleep(20 ms)` = 27,4 ms, haute résolution ; `Time::Now` correct |
| Chemins | `FILE_EXE` dans `C:\data\Programs\WindowsApps\…` ; LocalState, RoamingState ; `DIR_TEMP` = `AC\Temp` ; `DIR_PROGRAM_FILES` refusé |
| Fichiers | écriture, lecture, taille, suppression, fichier temporaire |
| JSON | lecture et écriture, UTF-8 compris |
| Registre | HKCU écrit et relu dans la ruche `LocalState\hkcu.dat` ; HKLM lisible (`ProductName` = `MobileCoreProd`) |
| Processus | `LaunchProcess` refusé comme prévu |
| Tâches | `RunLoop` (36 ms pour un délai de 30 ms), `base::Thread`, `ThreadPool` (16 tâches) |

Conséquence pour l'étape 5 : `MessagePumpForUI` ne peut pas s'appuyer sur une fenêtre HWND.

## Étape 5 : boucle UI sur le CoreDispatcher

| Élément | Sous UWP |
| --- | --- |
| `message_loop/message_pump_uwp.{h,cc}` (nouveau) | `MessagePumpForUI` sans fenêtre : `ScheduleWork` poste sur le `CoreDispatcher` du thread (`RunAsync`, un seul envoi en attente à la fois), le travail différé passe par un `ThreadPoolTimer` qui repasse par le `CoreDispatcher`. Une passe de `DoWork` par envoi, puis la main revient au système pour que les entrées et le rendu XAML s'intercalent |
| Attachement | Comme sur Android et iOS, la boucle du thread appartient au système : un `SingleThreadTaskExecutor(MessagePumpType::UI)` s'attache tout seul (`Attach`, branché dans `sequence_manager_impl.cc` et les `ThreadController`), sans `Run()` |
| `RunLoop` imbriqué | `Run()` appelle `CoreDispatcher::ProcessEvents(ProcessOneAndAllPending)` jusqu'à `Quit()` ; accepté par XAML sur W10M |
| `message_pump_win` | Partie UI (HWND) exclue sous `WINUWP` ; `MessagePumpForIO` (ports de complétion) inchangée |
| `win/message_window` | Retiré du build UWP |
| ThreadPool, threads COM STA | Sans file de messages : attente simple sur l'événement de réveil |
| Moniteur d'énergie | Fenêtre cachée et `WM_POWERBROADCAST` neutralisés ; à refaire avec `Windows.System.Power` |

Sur un thread sans `CoreWindow` (un `base::Thread` de type UI, les tests), la pompe délègue tout à une `MessagePumpDefault` : ni fenêtre ni dispatcher, `Run()` attend sur un événement comme les autres threads.

Résultats sur le Lumia (test `rhenium_base_ui_test`, lancé dans `OnLaunched`) : 0 échec. Tâche postée du ThreadPool vers le thread UI exécutée sur le bon thread ; `PostDelayedTask` de 50 ms en 87,8 ms (au démarrage, pendant le premier rendu XAML : la précision du `ThreadPoolTimer` sera à mesurer à froid) ; `RunLoop` imbriqué de 20 ms en 27,7 ms, tâche interne exécutée.

## Étape 6 : base_unittests sur le Lumia

Critère de sortie de J1 (« tests base et V8 au vert sur appareil »). `//base/test:test_support` tire ICU et libxml en entier : à la place, deux bibliothèques dans `gn\BUILD.gn`.
- `rhenium_base_tests` (complète) : //base, gtest, gmock, les aides de `base/test/` dont les tests ont besoin et l'exécuteur `gn\base_tests.cc`.
- `rhenium_base_unittests` : 51 fichiers `*_unittest.cc` de //base (chaînes, conteneurs, JSON, `Value`, fichiers, chemins, fonctions et callbacks, mémoire, threads, synchronisation, `RunLoop`, `SingleThreadTaskExecutor`, minuteurs, hachages, version de Windows…). La sonde la lie en entier (`/WHOLEARCHIVE`), sinon l'éditeur de liens écarte les tests, que rien ne référence.

L'exécuteur tourne sur un thread de la sonde. Sans processus enfants, un test qui plante tue l'app : chaque test est noté dans `LocalState\base-tests.txt` avant de démarrer, avec les messages `CHECK`/`LOG(ERROR)` et les exceptions matérielles (adresse relative à l'exe). Au lancement suivant, un test resté « en cours » compte comme planté et est sauté. Le bilan est exporté avant de relancer les tests.

Corrections apportées par les tests :

| Constat | Correctif |
| --- | --- |
| `SetCurrentThreadQualityOfService` (W10M 15254 = RS3) appelait `SetThreadInformation` sans vérifier qu'il l'avait trouvée dans kernel32 : appel à l'adresse nulle dès qu'un thread change de priorité | Recherche dans kernel32 puis kernelbase, et rien si absente (`SetThreadDescription` aussi) |
| La pompe UI s'arrêtait sur un `CHECK` sur un thread sans `CoreWindow` | Repli sur `MessagePumpDefault` |
| Tests de mort : ils exigent un processus enfant | Désactivés dans gtest sous `WINUWP` (`GTEST_HAS_DEATH_TEST`, comme sur iOS) |
| `OSInfo.GetWowStatusForProcess` n'a pas de cas ARM32 | Branche ARM32 ajoutée au test (`IsWowDisabled`), vérifiée sur le téléphone |
| `values_unittest` : taille de `base::Value` prévue pour libc++ | Taille calculée pour la STL MSVC |
| Tests de la pompe HWND (`SetTimer`, `PostMessage`) dans `single_thread_task_executor_unittest` | Exclus sous `WINUWP` |

Exclus d'office, sans objet sur le téléphone : `fixed_flat_map_unittest` (suppose libc++), `CheckDbgHelpVersion` (dbghelp absent de System32), `PathServiceTest.Get` et `GetProgramFiles` (ni bureau ni Program Files pour une app), `JSONReaderTest.ReadFromFile` (données de test absentes), tests multiprocessus et `DISABLED_`.

**Résultat le 2026-10-16 : 1 201 tests sur 1 201 réussis, en un seul lancement de 29,5 s.**

## Imports de bureau restants

Tri statique : symboles `__imp_*` de `base.lib` absents de `WindowsApp.lib` (19041). Sur 310 imports, 61 sont des API de bureau (66 avant les adaptations ; `SHGetFolderPathW`, `ShellExecuteExW`, `CreateProcess*`, `IsUserAnAdmin`, `AllowSetForegroundWindow`, `GetActiveWindow` ont disparu). Ils ne comptent que si l'objet qui les appelle est lié, et `WindowsApp.lib` du SDK 19041 ne garantit pas la présence sur W10M (15254) : la vérification sur le téléphone reste nécessaire pour les 310.

| Domaine | Imports | Objets |
| --- | --- | --- |
| Fenêtres et boucle UI (user32) | `CreateWindowExW`, `PeekMessageW`, `DispatchMessageW`, `SetTimer`, `MsgWaitForMultipleObjectsEx`… (17) | `message_pump_win`, `message_window`, `pooled_single_thread_task_runner_manager`, `power_monitor_device_source_win` |
| dbghelp | `StackWalk64`, `Sym*` (11) | `stack_trace_win` (chargement différé, comme V8) |
| Sécurité | `AccessCheck`, `CreateRestrictedToken`, `IsTokenRestricted`, `MapGenericMask`, `BuildTrusteeWithSidW`, `NtQueryObject` | `access_token`, `security_descriptor`, `access_control_list`, `security_util` |
| Divers Windows | `IsOS`, `SetPropW`, `GetThreadDesktop`, `SetProcessDPIAware`, `SetupDi*`… | `win_util`, `battery_level_provider_win` |
| Énergie | `RegisterSuspendResumeNotification` | `power_monitor_device_source_win` |
| Diagnostic | `RegisterEventSourceA`, `ReportEventA`, `TraceEvent`, `CreateDIBSection`, `GetGuiResources` | `syslog_logging`, `event_trace_provider`, `gdi_debug_util_win` |
| Fichiers et processus | `PathMatchSpecW`, `PrefetchVirtualMemory`, `QueryDosDeviceW`, `QueryProcessCycleTime` | `file_enumerator_win`, `file_util_win`, `process_metrics_win` |
| COM et shell | `CoRegisterInitializeSpy`, `CoAllowSetForegroundWindow`, `SHChangeNotify`, `VariantCompare` | `com_init_balancer`, `default_apps_util`, `shortcut`, `scoped_variant` |

## Ce qu'il faut adapter dans //base

Compiler en `WINAPI_FAMILY_APP` fera apparaître chaque API de bureau dès la compilation, comme pour la sonde.

| Domaine | Fichiers M116 | Remplacement envisagé |
| --- | --- | --- |
| Boucle de messages UI | `message_loop/message_pump_win.cc`, `win/message_window.cc` (fenêtre HWND cachée, `GetMessage`) | Nouvelle `MessagePumpForUI` alimentée par le `CoreDispatcher` du thread XAML |
| Boucles par défaut et E/S | `message_pump_win.cc` (événements, ports de complétion) | À vérifier : `CreateIoCompletionPort` et les attentes sont disponibles pour les apps |
| Processus | `process/launch_win.cc`, `process_iterator_win.cc`, `kill_win.cc` | Bouchons : Rhénium est mono-processus |
| Registre | `win/registry.cc` | HKCU vers une ruche privée de l'app (fait) |
| Chemins | `base_paths_win.cc` (`SHGetKnownFolderPath`, `GetModuleFileName`) | Dossiers de l'app (LocalState, TempState, dossier d'installation) via WinRT |
| Fichiers | `files/file_util_win.cc`, `file_path_watcher_win.cc` | `CreateFile2` (les variantes `…FromApp` datent de la 1803, absentes de la 15254) ; surveillance de dossier en bouchon |
| Bibliothèques | `native_library_win.cc` (`LoadLibraryExW`) | `LoadPackagedLibrary` |
| Débogage, traces | `debug/stack_trace_win.cc` (dbghelp), ETW (`trace_event_etw_export_win.cc`, `win/event_trace_*`) | dbghelp en chargement différé (comme V8) ; ETW en bouchon ; `enable_base_tracing=false` |
| Divers | `power_monitor_device_source_win.cc`, `win/shortcut.cc`, `com_init_*` | Bouchons, ou équivalents WinRT si utiles |
| Mémoire | `allocator/partition_allocator` (`VirtualAlloc`) | PartitionAlloc et la cale d'allocateur désactivés pour commencer ; plus tard, PartitionAlloc sur `VirtualAllocFromApp` (Blink en aura besoin) |

Les remplacements des imports absents de W10M (`kernel32legacy`), du bit Thumb et du chargement différé de dbghelp sont déjà connus grâce à V8 (`..\j1\README.md`).

## Configuration de build envisagée

`target_os = "winuwp"`, `target_cpu = "arm"`, `target_winuwp_family = "desktop"` (option B), toolchain clang-cl 23 + STL MSVC 14.44 + SDK 10.0.19041, CRT statique (`/MT`, comme la sonde, sans dépendance à VCLibs), `is_component_build = false`, `use_custom_libcxx = false`, `enable_base_tracing = false`, `use_partition_alloc_as_malloc = false`, `use_allocator_shim = false`, `enable_rust = false`, `treat_warnings_as_errors = false`.

## Étapes

1. **Sources** : `fetch.ps1` télécharge par archives gitiles, à la balise 116.0.5845.228, `base`, `build`, `buildtools`, `testing`, `third_party/abseil-cpp`, `third_party/googletest`, `third_party/modp_b64`, `third_party/boringssl` et ce que `gn gen` réclamera. Critère : `gn gen` passe.
2. **Toolchain** : correctif du build (`patches\0001-…`) qui donne à `uwp_arm` notre clang-cl, MSVC 14.44, le SDK 19041 et `/MT`. Critère : un premier `.obj` de `base` compile en `WINAPI_FAMILY_APP`.
3. **Compilation de `//base`** : correctifs regroupés par domaine (tableau ci-dessus), chaque modification marquée « Rhénium ». Critère : `base.lib` se construit.
4. **Premier essai sur le Lumia** : `..\sonde\build.ps1 -WithBase`, qui lie `base.lib` et exécute un test de fumée (`base::Thread`, `PostTask`, `TimeTicks`, `FilePath`, `JSONReader`/`JSONWriter`, `ThreadPool`). Critère : résultats corrects dans le rapport.
5. **Boucle de messages UWP** : `MessagePumpForUI` sur le `CoreDispatcher`, tâches postées depuis d'autres threads vers le thread XAML. Critère : `RunLoop` et `PostTask` vers l'UI fonctionnent dans la sonde.
6. **Tests unitaires** : un sous-ensemble de `base_unittests` exécuté dans la sonde, résultats exportés. Critère : à définir après l'étape 3 (quels tests ont un sens sans processus ni registre).

## Risques

- **Toolchain `winuwp` jamais utilisée avec clang** : on ne sait pas ce qu'elle casse ailleurs dans le build. Repli : rester en `target_os = "win"` (comme V8) et poser `WINAPI_FAMILY` à la main.
- **Volume des adaptations** : environ 1 Mo de `base/win` et les fichiers `*_win.cc`. Le correctif grossit vite ; une série de correctifs par domaine reste lisible.
- **Dépendances tierces** (BoringSSL, abseil) qui appellent des API de bureau : à découvrir à la compilation.
- **Cohérence avec V8** : à terme, V8 et `//base` devront être compilés dans le même build Chromium, avec les mêmes options.
