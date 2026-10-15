# Sonde Rhénium

Petite app UWP qui mesure, sur un vrai téléphone Windows 10 Mobile, les quatre inconnues du cahier des charges avant de figer l'architecture. C'est aussi le premier test du jalon J0 (toolchain ARM32).

| Test | Ce qu'il mesure | Pourquoi |
| --- | --- | --- |
| Appareil | Modèle, build W10M, architecture, cœurs | Situer les résultats |
| Mémoire | `AppMemoryUsageLimit`, usage, commit ; bouton de remplissage jusqu'à 90 % de la limite | Budget mémoire réel de Rhénium |
| Direct3D 11 | Feature level matériel et WARP, nom du GPU, mémoire vidéo | Faisabilité d'ANGLE D3D11 |
| JIT | `VirtualAllocFromApp` + `VirtualProtectFromApp` : écrire du code, le rendre exécutable, l'exécuter, le repasser en écriture, le modifier, le réexécuter | V8 peut-il utiliser son JIT, ou faut-il rester en jitless ? |
| clang-cl (`-Compiler clang`) | Module `src\clang_probe.cpp` compilé par clang-cl sans exceptions : STL, tri, division 64 bits | Toolchain de Chromium |
| Rust (`-WithRust`) | Bibliothèque statique `rust\` : Vec, tri, `format!`, thread | Composants Rust de Chromium |

Les résultats s'affichent à l'écran, sont copiables (bouton « Copier les résultats ») et sont écrits dans `LocalState\resultats.txt`. Avant chaque étape risquée, `LocalState\progression.txt` note l'étape en cours : si l'app se ferme brutalement, ce fichier dit où.

## État

- Build x64 validé sur PC le 2026-10-14 : tous les tests passent, et la variante sans `codeGeneration` est bien refusée (`VirtualProtectFromApp` → erreur 5).
- Build ARM32 validé le 2026-10-14 avec le Windows 10 SDK 10.0.19041 et MSVC 14.44, déployé sur Lumia 950. Le C++/WinRT récent a retiré ARM32 : `build.ps1` en patche une copie de `base.h` (branche `_M_ARM`), sans toucher au SDK.
- Build ARM32 complet (`-Compiler clang -WithRust`) validé sur Lumia 950 le 2026-10-15 : 1 536 Mo de limite mémoire (1 360 Mo réellement alloués), JIT W^X, Direct3D 11_1, module clang-cl et Rust corrects.
- LLVM ne sait pas compiler les exceptions C++ pour Windows ARM32 (« Funclet EH is not implemented ») : la coquille C++/WinRT reste compilée par MSVC, et seul le module sans exceptions passe par clang-cl, comme le serait Chromium.
- DirectXMath (inclus par C++/WinRT) utilise des intrinsèques NEON propres à MSVC : le module clang ne l'inclut pas.

## Prérequis

| Élément | État sur la machine de dev | À faire |
| --- | --- | --- |
| Toolset MSVC 14.44 (dernier avec cible ARM32) | Installé | — |
| Windows SDK avec bibliothèques ARM32 | 10.0.19041 installé (`winget install Microsoft.WindowsSDK.10.0.19041`) | — |
| clang-cl + lld-link | LLVM 23.1.3 (`winget install LLVM.LLVM`) | — |
| Rust nightly + rust-src | rustup (`winget install Rustlang.Rustup`) puis `rustup toolchain install nightly --profile minimal --component rust-src` | — |
| WinAppDeployCmd, MakeAppx, SignTool, MakePri | Installés (SDK 10.0.26100) | — |

## Préparer le téléphone

1. Paramètres › Mise à jour et sécurité › Pour les développeurs : activer le **mode développeur**.
2. Sur le même écran : activer **Découverte d'appareil** et **Portail d'appareil**, puis toucher « Associer » pour obtenir le code d'appairage.
3. Le brancher en USB (service IpOverUsb, adresse 127.0.0.1), ou à défaut le mettre sur le même réseau Wi-Fi que le PC.

## Compiler, déployer, lire

```powershell
./build.ps1 -Compiler clang -WithRust   # tout : out\RheniumSonde_arm-clang-rust.appx
./build.ps1                             # coquille MSVC seule : out\RheniumSonde_arm-msvc.appx
./build.ps1 -NoCodeGeneration           # variante de comparaison
./deploy.ps1 -Ip 127.0.0.1 -Appx out\RheniumSonde_arm-clang-rust.appx   # téléphone en USB
./deploy.ps1                            # sinon : trouve l'adresse IP du téléphone sur le réseau
```

Lancer « Sonde Rhénium » sur le téléphone, puis « Tester le remplissage mémoire ». L'app garde l'écran allumé tant qu'elle est ouverte (un verrouillage suspendrait les tests). Chaque série de tests écrit une copie horodatée `sonde-AAAAMMJJ-HHMMSS.txt` dans `Phone\Downloads\` (sous-dossier de l'app), visible depuis le PC par USB. Débrancher et rebrancher le câble si le fichier n'apparaît pas : le cache MTP de Windows ne se rafraîchit pas tout seul.

Avec un téléphone branché en USB, `deploy.ps1 -Ip 127.0.0.1` fonctionne directement (service IpOverUsb). Il utilise le WinAppDeployCmd du SDK 10.0.19041 : ceux des SDK Windows 11 échouent avec W10M (0x80070003).

Le paquet est signé avec un certificat de développement local (`certs\`, créé au premier build, hors du magasin de certificats Windows). Si le téléphone le refuse, installer `certs\RheniumDev.cer` sur le téléphone.

## Lire les résultats

- **JIT** : « Exécution : OK (43) » = V8 peut utiliser son JIT. Un échec à `VirtualProtectFromApp(RX)` avec la capability = jitless obligatoire, ou interop unlock à tester. Si l'app se ferme pendant « exécution du code généré », le système bloque l'exécution malgré la protection.
- **Direct3D** : 10_0 ou plus en matériel = ANGLE D3D11 envisageable ; 9_x = rendu logiciel.
- **Mémoire** : `AppMemoryUsageLimit` est le plafond à ne jamais dépasser ; la ligne « Alloué » du remplissage donne la marge réellement utilisable (plafonnée à 1 Go sur un PC).
- **clang-cl** : « 338350 » et « Division 64 bits : OK » = le code LLVM ARM32 tourne et se lie à la coquille MSVC.
- **Rust** : « OK (5050) » = la std UWP recompilée fonctionne, threads compris.
