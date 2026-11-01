# Jalon J1 : V8 seul sur Windows 10 Mobile

Objectif : compiler V8 (sans Chromium) pour Windows ARM32 avec clang-cl, le lier à une petite app UWP sur le modèle de `..\sonde`, et exécuter du JavaScript sur le Lumia 950, avec puis sans JIT.

## État au 2026-10-15 : atteint

**V8 11.6.189.22 exécute du JavaScript sur le Lumia 950 (W10M 15254.603), en jitless comme en JIT complet (Sparkplug, Maglev, TurboFan), et le JIT tourne en multi-thread** (compilation et GC concurrents) depuis le 2026-10-15. Les 15 tests du script de la sonde donnent les mêmes résultats, au bit près, que V8 sur PC : récursion, flottants, `JSON.stringify`/`parse`, RegExp.

Banc de la sonde (six épreuves, trois tours, `gc()` complet avant chaque tour), en ms, horloge à la milliseconde. Deux lancements par configuration ; chaque case donne les tours 2 et 3 des deux lancements (le tour 1 inclut la montée en régime du JIT).

| Épreuve (Lumia 950) | JIT multi-thread | JIT mono-thread | jitless |
| --- | --- | --- | --- |
| `fib(27)` | 13 à 18 | 7 | 92 à 100 |
| Crible d'Ératosthène jusqu'à 10⁶ (`Uint8Array`) | 24 à 47 | 19 à 32 | 279 à 349 |
| 200 000 objets créés puis parcourus | 126 à 170 | 146 à 185 | 247 à 695 |
| Tri de 50 000 nombres avec comparateur | 84 à 95 | 69 à 80 | 119 à 200 |
| `JSON.stringify` + `parse`, 20 000 objets | 59 à 61 | 58 à 74 | 74 à 124 |
| Concaténation + RegExp, 20 000 éléments | 9 à 22 | 10 à 13 | 20 à 27 |
| **Total d'un tour** | **327 à 397** | **310 à 391** | **861 à 1 456** |
| Attentes W^X par lancement (voir plus bas) | 5 à 9 (3,0 à 4,3 ms en tout, 1,8 ms au plus) | 0 | 0 |

Le JIT va environ 3 fois plus vite que jitless sur ce banc. Multi-thread et mono-thread font jeu égal : le banc est court et le thread principal reste le goulot ; la concurrence sert surtout à ne pas bloquer ce thread pendant les compilations TurboFan et le GC. Seuls, Sparkplug fait 659 à 751 ms par tour (10 attentes W^X), TurboFan 366 à 457 ms (1 attente), Maglev 875 à 1 096 ms (aucune attente, aucun gain net sur jitless : à creuser). Les totaux du banc ci-dessus datent d'avant la bascule par pages de 4 Ko ; après elle, le JIT multi-thread donne 361 à 461 ms par tour (tours 2 et 3 de trois lancements), dans le bruit de mesure.

## Lancer le test

```powershell
./build-v8.ps1                                          # applique patches\, gn gen, ninja v8_monolith
..\sonde\build.ps1 -Arch arm -WithV8                    # sonde + V8 (clang-cl, lld-link)
..\sonde\deploy.ps1 -Ip 127.0.0.1 -Appx ..\sonde\out\RheniumSonde_arm-clang-v8.appx
```

À chaque lancement, la sonde teste une configuration de V8, à tour de rôle (`build.ps1 -V8First N`, base 0, choisit celle du premier lancement après installation ; une relance automatique est impossible : `CoreApplication.RequestRestartAsync` renvoie `E_NOINTERFACE` sous W10M 15254) : JIT complet multi-thread, JIT complet mono-thread (`--single-threaded`), jitless, puis Sparkplug, Maglev et TurboFan seuls (multi-thread). Ne pas presser « Tester le remplissage mémoire » entre deux lancements : le processus qui se ferme rend encore ses 1,35 Go et W10M tue la relance avant même les tests. Elle se ferme d'elle-même 3 s après la fin, sinon W10M reprendrait l'instance suspendue sans refaire les tests. Elle écrit dans `Phone\Downloads\Rhenium.Sonde…\` :
- `sonde-….txt` : le rapport, exporté avant et après V8 ;
- `v8-journal-….txt` : chaque étape de V8 avec son heure, ses messages (`OutputDebugStringA` intercepté), ses erreurs fatales et toute exception matérielle (code, PC, mode A32/Thumb, registres). Ce journal est écrit de façon synchrone, il est donc complet même après un plantage.

`out\arm-clang\RheniumSonde.map` situe un PC remonté par le journal.

## Les six obstacles franchis sur le téléphone

| Symptôme | Cause | Correctif |
| --- | --- | --- |
| L'app ne démarre pas (écran de démarrage, puis retour à l'accueil) | Trois fonctions importées par V8 (`VerifyVersionInfoW`, `CreateSemaphoreA`, `timeGetTime`) passent par des api sets qui pointent vers `kernel32legacy.dll`, absente de W10M | `app\w10m_imports.cc` définit leurs pointeurs d'import (`__imp_…`) avec des équivalents pour apps. Méthode : la sonde vérifie sur le téléphone chaque import d'un exe (`build.ps1 -ImportCheck`) et lit le schéma des api sets dans le PEB |
| L'app est tuée au démarrage | W10M tue une app qui tarde à afficher sa fenêtre ; la sonde exécutait V8 avant | V8 tourne sur son propre thread après `Window.Activate` |
| Plantage au premier appel d'un builtin | lld ajoute le bit Thumb à toute adresse de symbole d'une section exécutable : V8 exécutait ses builtins A32 en mode Thumb | `DefaultEmbeddedBlobCode()` retire ce bit (`execution/isolate.cc`) |
| `JSON.stringify(x)` renvoie `undefined` | Bug de V8 propre à Windows ARM32 : dans `InvokePrologue`, la boucle qui recopie les arguments réutilise des drapeaux du processeur que la variante Windows d'`AllocateStackSpace` (sondage de pile, `cmp`) écrase. Quand une fonction reçoit moins d'arguments que prévu, la copie est sautée | `cmp` explicite avant la boucle (`codegen/arm/macro-assembler-arm.cc`). Reproduit et vérifié sur PC avec `mksnapshot` |
| JIT : violation d'exécution sur du code neuf | `write_code_using_rwx` (vrai par défaut) veut des pages RWX, refusées par W10M ; et sans `write_protect_code_memory`, V8 ne repasse jamais ses pages de code en RX | `--no-write-code-using-rwx --write-protect-code-memory` |
| JIT multi-thread : violation d'exécution | En W^X, V8 bascule en RW les pages de code qu'il écrit pendant qu'un autre thread de V8 y écrit (balayeur concurrent, Sparkplug concurrent) ; le thread principal qui exécute du code de cette page prend une violation d'exécution | Gestionnaire d'exceptions vectorisé `WaitForCodePage` (`base/platform/platform-win32.cc`) : le thread attend que la page redevienne exécutable, puis reprend l'instruction. Voir « JIT multi-thread en W^X » |

## JIT multi-thread en W^X

En W^X, toute écriture de V8 11.6 dans le code passe par `CodePageMemoryModificationScope` (`heap/heap-inl.h`) : il prend le mutex de la page (`MemoryChunk`), passe sa zone d'objets en RW, écrit, la remet en RX et relâche le mutex. Les écrivains sont donc déjà sérialisés par page, et leurs portées sont courtes (sous `DisallowGarbageCollection`, sans attente). Seul le thread principal exécute du code JIT ; les builtins, dans `.text`, ne sont jamais basculés. Le conflit se limite donc à un cas : le thread principal exécute une page de 4 Ko qu'un autre thread est en train d'écrire.

**Choix : synchroniser côté exécutant.** `WaitForCodePage`, posé en tête des gestionnaires d'exceptions vectorisés au premier passage d'une page en exécution, intercepte la violation d'exécution (code 8) sur une page que V8 a rendue exécutable (un bit par page de 4 Ko, 128 Ko de `.bss`), attend par `VirtualQuery` que la page redevienne exécutable (`YieldProcessor`, puis `Sleep(0)`, puis `Sleep(1)`, 10 s au plus) et reprend l'instruction fautive (`EXCEPTION_CONTINUE_EXECUTION`). Toute autre faute suit son cours. Le retour en RX après une écriture se fait aussi en un seul `VirtualProtectFromApp`, sans repasser par RW. `rhenium_wx_stats()` donne le nombre et la durée des attentes ; la sonde les écrit dans son rapport.

**Bascule par pages de 4 Ko (2026-10-15).** V8 passait en RW toute la zone d'objets du `MemoryChunk` (256 Ko, 64 pages) pour chaque écriture, même de 8 octets : le thread principal attendait dès qu'il exécutait n'importe où dans ces 256 Ko. Sous W10M seulement (macro `RHENIUM_NARROW_CODE_WRITES`, `common/globals.h` : Windows + ARM, pas de W^X par pthread), `CodePageMemoryModificationScope` connaît maintenant l'étendue écrite et ne bascule que les pages de 4 Ko qui la couvrent (`MemoryChunk::SetCodeModificationPermissions(start, size)` et `SetDefaultCodePermissions(start, size)` : arrondi aux pages, borné à la zone d'objets). Le mutex de la page (`page_protection_change_mutex_`) reste pris de l'entrée à la sortie de la portée, comme avant : les écrivains d'un même chunk restent sérialisés, il n'y a toujours pas de compteur, seul le nombre de pages en RW change. Ailleurs, les nouvelles variantes retombent sur la page entière.

| Appelant | Étendue passée |
| --- | --- |
| `Factory::CodeBuilder::BuildInternal` (en-tête et corps de l'`InstructionStream`) | `[objet, objet + SizeFor(body_size))` ; la taille est calculée à la main avant l'initialisation, car `Size()` lit `body_size` |
| Les deux `set_map_after_allocation` de l'`InstructionStream` (Sparkplug concurrent compris) | un mot, en tête de l'objet |
| `CodePageMemoryModificationScope(InstructionStream)` (`Code::ClearEmbeddedObjects`, relocations des builtins) | `[objet, objet + Size())` |
| `CreateFillerObjectAtImpl` (balayeur, liste libre, tas) | 4 mots ; tout le bloc si l'effacement est demandé |
| `FreeListCategory::Free` et `SearchForNodeInList` | 4 mots (carte, taille, suivant du `FreeSpace`) |
| Ramasse-miettes (évacuation, mise à jour des pointeurs), balayeur avec remplissage de zap, scavenger | inchangés : page entière (le thread principal n'exécute pas de JS pendant ces phases) |

Piège : une écriture qui sort de l'étendue annoncée n'est plus masquée par le RW de la page entière ; elle plante en écriture. Toute nouvelle portée doit donc donner une étendue exacte (ou la page entière).

| Mesure (Lumia 950, par lancement) | Avant | Après |
| --- | --- | --- |
| JIT complet multi-thread | 7 attentes, 6 à 9 ms, max 4,4 ms | 5 à 9 attentes, 3,0 à 4,3 ms, max 0,65 à 1,8 ms (3 lancements) |
| Sparkplug seul | 16 à 18 attentes, max 10,8 ms | 10 attentes, 5,9 ms, max 1,8 ms (1 lancement) |
| TurboFan seul | 12 à 13 attentes | 1 attente, 0,9 ms |
| Maglev seul, mono-thread, jitless | 0 | 0 |

Le nombre d'attentes baisse surtout avec Sparkplug et TurboFan ; avec le JIT complet il ne baisse pas (les écritures restent sur les mêmes pages de 4 Ko que le code exécuté), mais leur durée cumulée et surtout la plus longue sont divisées par 2 à 7. Les sept configurations passées (une par lancement) donnent le même résultat que sur PC aux 15 tests, statut 0. Ces chiffres viennent de un à trois lancements par configuration : à confirmer avec davantage.

| Piste | Verdict |
| --- | --- |
| Double mappage (une vue RW, une vue RX de la même mémoire) | **Possible sur W10M** : `CreateFileMappingFromApp(PAGE_EXECUTE_READWRITE)` puis deux `MapViewOfFileFromApp` (`FILE_MAP_WRITE`, `FILE_MAP_READ \| FILE_MAP_EXECUTE`) passent avec `codeGeneration`, et du code écrit par la vue RW s'exécute par la vue RX (testé par la sonde). Écarté pour V8 11.6 : il écrit dans son code à l'adresse d'exécution depuis des dizaines d'endroits (factory, GC, relocations, balayeur, liste libre) ; il faudrait les traduire tous. À reconsidérer avec V8 12+ (`WritableJitAllocation`) |
| Synchroniser bascules et exécution | **Retenu**, sous forme d'attente sur faute : rien à changer dans le tas, attentes rares (7 à 18 par lancement) |
| Restreindre la concurrence aux tâches qui n'écrivent pas de code | Écarté : partiel (balayeur, Sparkplug concurrent, liste libre écrivent tous) et coûteux |

## Risque A32 (levé sur Lumia 950)

Le JIT ARM32 de V8 et ses builtins embarqués émettent de l'**A32** (« mode ARM »), pas du Thumb-2, y compris les handlers de l'interpréteur Ignition. La doc Microsoft (*Overview of ARM32 ABI Conventions*) prévient : « The instruction set for Windows on ARM is strictly limited to Thumb-2… An attempt to switch into the legacy ARM instruction set may succeed. However, if it does, any exceptions or interrupts that occur may lead to an application fault in user mode. » Sur le Lumia 950, `app\a32_probe.s` (`..\sonde\build.ps1 -WithA32`) passe sous interruptions, préemptions, défauts de page et pages de garde, et V8 tourne. La sonde a même intercepté une exception levée en A32 : la remise des exceptions fonctionne aussi dans ce mode.

## Choix faits

| Choix | Pourquoi |
| --- | --- |
| V8 11.6.189.22 | Version de Chromium M116, base visée ; pas de Rust dans V8 à cette époque |
| Pas de gclient | Demande de l'utilisateur d'alléger le téléchargement (188 Mo au lieu de 2 à 4 Go) |
| ICU omis, `v8_enable_i18n_support=false` | J1 n'a pas besoin d'`Intl` ; Chromium récupère ICU avec ses propres sources (`..\j2\fetch.ps1`) |
| `v8_enable_webassembly=false` | Hors périmètre J1 |
| LLVM 23.1.3 et non le clang de Chromium | La STL MSVC 14.44 exige clang 19 ou plus récent (celui de M116 est en 17) |
| `use_custom_libcxx=false` | V8 utilise la STL MSVC 14.44, la même que la coquille MSVC |
| Code qui appelle V8 isolé derrière une API C (`app\v8_shim.cc`), compilé par clang-cl sans exceptions | Évite tout écart d'ABI entre la coquille C++/WinRT (MSVC, avec exceptions) et V8 |
| API visée sur ARM32 : `NTDDI_WIN10_RS2` (15063) | Les fonctions absentes de W10M sont refusées dès la compilation |
| Pile du thread V8 : 4 Mo engagés d'avance | V8 ARM32 ne sonde pas les pages de garde de la pile Windows |

## Correctifs du build (`patches\0001-build-win-arm32-toolchain.patch`, dépôt `v8\build`)

1. `toolchain/win/setup_toolchain.py` : avec `RHENIUM_MSVC_DIR`, environnement construit sans `vcvarsall.bat` (celui de VS 2026 ne cible plus ARM32) ; MSVC 14.44 et SDK 10.0.19041.
2. `toolchain/win/*.gni`, `BUILD.gn` : toolchains `win_clang_arm` (cible), `win_clang_x64` (hôte) et `clang_x86_v8_arm` (mksnapshot 32 bits avec le simulateur ARM).
3. `config/win/BUILD.gn` : `--target=thumbv7-pc-windows-msvc` ; `-fmsc-version=1944` ; `NTDDI_WIN10_RS2` sur ARM32 ; sous-système 10.0.
4. `config/win/visual_studio_version.gni` : `visual_studio_runtime_dirs = "none"`.
5. `config/rust.gni` : cible Rust `thumbv7a-uwp-windows-msvc` pour win/arm.
6. `config/win/BUILD.gn` : `/clang:-masm=att` sur x86/x64 (clang-cl récent lit l'asm GNU en syntaxe Intel).

Hors correctif, `build-v8.ps1` crée la jonction `..\toolchains\llvm` → `C:\Program Files\LLVM` (le GN de 2023 ne gère pas un chemin sur un autre lecteur, et ninja lance clang-cl sans guillemets) et copie `stubs\gtest_prod.h` à la place de googletest. Le chemin du projet doit rester sans accent (GN et ninja 2023 échouent sur `F:\Rhénium`). Pour lancer ninja en arrière-plan : `cmd /c "… < NUL"`. Régénérer un correctif avec `git diff --output=…`, jamais par un pipe PowerShell (il abîme les « é »).

## Correctifs V8 (`patches\0002-v8-win-arm32.patch`, dépôt `v8`)

V8 11.6 n'a aucun cas Windows ARM32. Chaque modification est marquée « Rhénium » dans le code.

| Fichier | Correctif |
| --- | --- |
| `codegen/arm/macro-assembler-arm.cc` | `InvokePrologue` : `cmp` avant la boucle de copie des arguments (bug ci-dessus) |
| `execution/isolate.cc` | Retire le bit Thumb de l'adresse du blob des builtins |
| `base/platform/platform-win32.cc` | `VirtualAllocFromApp`/`VirtualProtectFromApp` : allocation sans exécution puis bascule de protection, jamais RWX ; `WaitForCodePage` et `rhenium_wx_stats` (JIT multi-thread en W^X) ; `OS::ArmUsingHardFloat` ; `__builtin_frame_address` au lieu de `_AddressOfReturnAddress` |
| `codegen/arm/constants-arm.h` | Garde EABI levée sous `_WIN32` : Windows ARM32 suit l'AAPCS à flottants matériels (`__ARM_PCS_VFP`) |
| `codegen/arm/cpu-arm.cc` | `FlushICache` via `FlushInstructionCache` |
| `utils/memcopy.cc` | Copie mémoire ARM optimisée (builtin) activée sous Windows |
| `utils/utils.cc` | `GetCurrentStackPosition` sans `_AddressOfReturnAddress` (absent de clang ARM32) |
| `base/platform/time.cc`, `time.h` | Pas de TSC (`__rdtsc`) sur ARM32 |
| `base/immediate-crash.h` | `__debugbreak()` + `udf` au lieu de `int3`/`ud2` |
| `base/debug/stack_trace_win.cc`, `libsampler/sampler.cc` | Registres ARM32 de `CONTEXT` |
| `heap/base/asm/arm/push_registers_asm.cc` | Directives COFF au lieu des directives ELF |
| `snapshot/embedded/platform-embedded-file-writer-win.cc` | Pas de préfixe `_` sur ARM32 |
| `common/globals.h`, `heap/memory-chunk.{h,cc}`, `heap/heap.h`, `heap/heap-inl.h`, `heap/factory.cc`, `heap/free-list.cc`, `heap/heap.cc` | Bascule W^X par pages de 4 Ko (`RHENIUM_NARROW_CODE_WRITES`, voir « JIT multi-thread en W^X ») |

## Limites actuelles et suite

- **JIT multi-thread : rétabli** (2026-10-15). Drapeaux : `--no-write-code-using-rwx --write-protect-code-memory`, sans `--single-threaded`. Les six configurations de la sonde passent, deux fois chacune. Bascule restreinte aux pages de 4 Ko écrites depuis le 2026-10-15 : la plus longue attente passe de 10,8 ms à 1,8 ms (Sparkplug). Reste : les attentes sur une même page de 4 Ko (code exécuté à côté d'un objet écrit) ne se réduisent plus que par un double mappage (V8 12+) ; mesures à refaire sur plus de lancements.
- **Maglev seul** n'apporte presque rien sur jitless sur ce banc : à creuser (couverture de Maglev ARM32 en 11.6).
- **Banc encore court** (1 s par tour en jitless) : un vrai banc (JetStream, Speedometer) viendra avec le navigateur.
- **Téléphone sans interop unlock** : `codeGeneration` n'a été testé que sur un Lumia déverrouillé.
- **V8 compilé en `WINAPI_FAMILY_DESKTOP_APP`** : passer en `WINAPI_FAMILY_APP` révélerait à la compilation les autres API de bureau qu'il utilise.
- **Bug InvokePrologue** : à signaler en amont (V8), il touche toute build Windows ARM32.
