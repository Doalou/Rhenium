# Rhénium

Un navigateur moderne pour Windows 10 Mobile, basé sur Chromium.

Le navigateur intégré à Windows 10 Mobile repose sur EdgeHTML, figé depuis 2017 : une part croissante du web s'y affiche mal ou pas du tout, et les navigateurs tiers du Store ne sont que des interfaces autour du même moteur. Rhénium porte Chromium (Blink et V8) sur les Lumia, pour redonner à ces téléphones un accès correct au web d'aujourd'hui. C'est un projet communautaire, destiné aux passionnés qui utilisent encore un Lumia et savent installer une app par sideload.

## Où en est le projet

Rhénium tourne sur un Lumia 950 : il affiche Wikipédia, YouTube et les sites courants dans leur version mobile, avec défilement et zoom tactiles, clavier virtuel, rotation et une barre d'adresse en bas de l'écran.

| Jalon | Contenu | État |
| --- | --- | --- |
| J0 · Toolchain | Code C++ et Rust compilé pour ARM32 qui tourne sur le téléphone | Atteint |
| J1 · Fondations | V8 avec JIT, `//base` de Chromium, 1 201 tests au vert sur le Lumia | Atteint |
| J2 · Moteur | `content_shell` en un seul processus, rendu logiciel, réseau et TLS | Atteint |
| J3 · Navigation tactile | Coquille XAML, toucher, saisie, rotation, cache disque, agent utilisateur mobile | Atteint |
| J4 · Extensions | Extensions Manifest V3, uBlock Origin Lite installé par défaut | À venir |
| J5 · v1 | Onglets, historique, favoris, téléchargements, profil persistant | À venir |

La lecture vidéo marche aussi : YouTube en 360p avec le son (VP9 par libvpx, AAC par Media Foundation).

Appareils visés : Lumia 950 et 950 XL en priorité (Windows 10 Mobile 1709), Lumia 650 et 640 au mieux.

## Partie technique

Chromium n'a jamais pris en charge Windows ARM32 ni les apps UWP. Rhénium part de **Chromium 116.0.5845.228** et ajoute une cible `target_os="winuwp"`, `target_cpu="arm"`.

- **Toolchain** : Chromium est compilé par clang-cl (sans exceptions C++, que LLVM ne gère pas en ARM32 Windows) avec la STL de MSVC 14.44 et le SDK 10.0.19041. La coquille XAML est en C++/WinRT, compilée par MSVC, et liée au moteur dans le même exécutable.
- **Un seul processus** : une app UWP mobile ne peut pas lancer de processus enfants. Navigateur, rendu, réseau et audio tournent comme des threads du même processus, sans sandbox.
- **JIT de V8** : W10M refuse la mémoire à la fois inscriptible et exécutable. V8 écrit son code dans des pages RW puis les passe en RX (W^X), grâce à la capability `codeGeneration`. Le JIT est environ 3 fois plus rapide que le mode sans JIT.
- **Couche plateforme** : `user32` n'exporte aucune fonction de fenêtres sur W10M. La boucle de messages passe par le `CoreDispatcher`, les entrées par les pointeurs de la `CoreWindow`, la saisie par `CoreTextEditContext`.
- **Rendu** : composition logicielle Skia, présentée dans une swap chain Direct3D 11 posée sur un `SwapChainPanel` XAML. Le rendu GPU par ANGLE reste à faire.
- **Correctifs post-édition de liens** : `j2\fix_vcall_thunks.py` réécrit les relais d'appels virtuels que clang génère mal en ARM32.

Les modifications de Chromium sont gardées sous forme de correctifs (`patches\` de chaque jalon), appliqués par les scripts de build sur un arbre Chromium vierge.

## Organisation du dépôt

| Dossier | Contenu |
| --- | --- |
| `sonde\` | App UWP de mesure sur le téléphone (mémoire, JIT, Direct3D, A32, clang-cl, Rust, V8) et script de déploiement par USB. Voir `sonde\README.md` |
| `j1\` | V8 11.6 pour Windows ARM32 : correctifs, cale et remplacements d'imports pour W10M. Voir `j1\README.md` |
| `j1-base\` | Chromium `//base` pour W10M, boucle UI sur le `CoreDispatcher`. Voir `j1-base\README.md` |
| `j2\` | Moteur (`content_shell`), coquille XAML, empaquetage de l'app. Voir `j2\README.md` |
| `j3\` | Notes du jalon de navigation tactile. Voir `j3\README.md` |

Non versionnés, recréés par les scripts : `v8\` (`j1\fetch.ps1`), `chromium\` (`j1-base\fetch.ps1`, `j2\fetch.ps1`), `toolchains\` (jonction vers LLVM), les dossiers `out\` et `sonde\certs\`.

## Compiler et installer

Il faut une machine Windows avec Visual Studio 2022 (MSVC 14.44 et outils ARM32), le SDK Windows 10.0.19041 et LLVM, et un téléphone en mode développeur.

```powershell
cd j2
.\build-content.ps1      # applique les correctifs, gn gen, ninja
.\package-shell.ps1      # paquet j2\out\RheniumShell_arm.appx
cd ..\sonde
.\deploy.ps1 -Ip 127.0.0.1 -Appx ..\j2\out\RheniumShell_arm.appx
```

Le chemin du projet ne doit pas contenir d'accent (`F:\Rhenium`) : GN et ninja de 2023 échouent sinon.

## Assistance par IA

Par souci de transparence : ce projet est développé avec l'assistance d'une IA (Claude, d'Anthropic), pour le code, les correctifs, les scripts et la documentation. Sur un projet de cette ampleur, porter Chromium sur une plateforme abandonnée et jamais prise en charge, tout faire à la main serait très difficile. Chaque étape est vérifiée sur un vrai téléphone.
