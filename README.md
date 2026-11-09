# Rhénium

Chromium sur Windows 10 Mobile.

Le navigateur de W10M repose sur EdgeHTML, qui n'a plus évolué depuis 2017, et les navigateurs du Store ne font qu'habiller le même moteur. Rhénium fait tourner Blink et V8 sur un Lumia, pour que ces téléphones affichent le web d'aujourd'hui.

Le projet s'adresse à ceux qui utilisent encore un Lumia et savent installer une app hors du Store.

## Ce qui marche

Sur un Lumia 950 :

- les sites courants, dans leur version mobile, avec défilement, zoom, clavier virtuel et rotation ;
- plusieurs onglets, l'historique, les favoris et les téléchargements ;
- les PDF, ouverts dans PDF.js ;
- YouTube en 360p et 720p, avec le son ;
- uBlock Origin Lite, actif dès le premier lancement, et l'ajout d'autres extensions depuis le Chrome Web Store ;
- les cookies et les données des sites, gardés d'un lancement à l'autre ;
- la version pour ordinateur des sites, pour tous les onglets ou un seul ;
- un mode sombre pour l'interface et les pages ;
- une interface en français ou en anglais.

L'interface reprend celle d'Edge sur W10M : l'adresse en bas et un menu « … ».

Les 950 et 950 XL sont les appareils visés. Les 650 et 640 n'ont pas encore été essayés.

Ce qui manque encore : le rendu par le GPU (tout est dessiné par le processeur) et Continuum. Il n'y a pas de sandbox : tout tourne dans un seul processus.

## Installer

Il faut le mode développeur sur le téléphone (Paramètres, Mise à jour et sécurité, Pour les développeurs) et le Portail d'appareil activé dans le même écran.

1. Télécharger `RheniumShell_arm.appx` et `RheniumDev.cer` depuis la page Releases.
2. Ouvrir dans un navigateur du PC l'adresse donnée par le Portail d'appareil.
3. Dans Apps, choisir le paquet et le certificat, puis installer.

Toucher le `.appx` dans l'Explorateur de fichiers du téléphone installe l'app, mais elle plante au lancement.

Une nouvelle version s'installe par-dessus l'ancienne sans effacer les données.

## Compiler

Ce n'est pas un petit build : il faut environ 40 Go de disque et plusieurs heures pour la première compilation.

Outils :

- Windows et PowerShell 7 ;
- Visual Studio 2022 avec le toolset MSVC **14.44** et ses outils ARM32. Les toolsets suivants ne savent plus compiler pour ARM32 : il faut garder une installation du 14.44 qu'on ne met plus à jour. ATL ARM32 vient du toolset 14.30. Les scripts trouvent eux-mêmes ces toolsets avec vswhere ;
- le SDK Windows 10.0.19041 ;
- LLVM 23 (testé avec clang-cl 23.1.3), installé dans `C:\Program Files\LLVM` ;
- Python 3.

Le dépôt doit être dans un chemin sans accent ni espace, par exemple `F:\Rhenium`.

Récupérer les sources, une seule fois :

```powershell
.\j1-base\fetch.ps1     # Chromium 116.0.5845.228, gn et ninja
.\j2\fetch.ps1          # dépendances du DEPS
.\j4\fetch-ubol.ps1     # uBlock Origin Lite
.\j5\fetch-pdfjs.ps1    # PDF.js
```

Compiler, empaqueter et installer sur un téléphone branché en USB :

```powershell
.\j2\build-content.ps1 -KeepGoing
.\j2\package-shell.ps1
.\sonde\deploy.ps1 -Ip 127.0.0.1 -Appx .\j2\out\RheniumShell_arm.appx
```

`build-content.ps1` applique les correctifs à l'arbre Chromium, lance gn puis ninja. Le paquet est signé par un certificat de développement créé au premier empaquetage dans `sonde\certs`.

## Comment c'est fait

Chromium n'a jamais tourné ni sur Windows ARM32 ni en app UWP. Rhénium part de Chromium 116.0.5845.228 et ajoute une cible `target_os="winuwp"`, `target_cpu="arm"`.

- Chromium est compilé par clang-cl, sans exceptions C++ (LLVM ne les gère pas sur ARM32 Windows). L'interface est en C++/WinRT, compilée par MSVC, dans le même exécutable.
- Une app UWP sur mobile ne peut pas lancer d'autres processus : navigateur, rendu, réseau et audio sont des threads d'un seul processus.
- W10M refuse la mémoire inscriptible et exécutable à la fois. V8 écrit son code dans des pages en lecture-écriture, puis les passe en exécution. Le JIT est environ trois fois plus rapide que l'interpréteur seul.
- `user32` n'a pas de fenêtres sur W10M. La boucle de messages passe par le `CoreDispatcher`, le toucher par la `CoreWindow`, la saisie par `CoreTextEditContext`.
- L'image est composée par Skia sur le processeur, puis affichée dans une swap chain Direct3D 11.

Les modifications de Chromium sont gardées sous forme de correctifs, appliqués par les scripts sur un arbre vierge. Chaque dossier de jalon a sa note détaillée :

| Dossier | Contenu |
| --- | --- |
| `sonde\` | App de mesure sur le téléphone et script de déploiement |
| `j1\` | V8 pour Windows ARM32 |
| `j1-base\` | `//base` de Chromium pour W10M, environnement de la toolchain |
| `j2\` | Moteur (`content_shell`), interface, empaquetage |
| `j3\` | Toucher, saisie, vidéo |
| `j4\` | Extensions et uBlock Origin Lite |
| `j5\` | Profil, favoris, téléchargements, PDF, version pour ordinateur, mode sombre |
| `perf\` | Bancs de performance |

| Jalon | État |
| --- | --- |
| J0, toolchain ARM32 | Atteint |
| J1, V8 et `//base` | Atteint |
| J2, moteur | Atteint |
| J3, navigation tactile | Atteint |
| J4, extensions | Atteint |
| J5, v1 | Atteint (Continuum non traité) |

## Licence

Le code de Rhénium est sous licence BSD à trois clauses (voir `LICENSE`), comme Chromium. Chromium garde ses propres licences. uBlock Origin Lite (GPLv3) et PDF.js (Apache 2.0) sont livrés comme composants séparés, avec leurs licences et leurs sources (`j4\ubol\SOURCES.txt`, `j5\pdfjs\SOURCES.txt`).

## Développé avec une IA

Ce projet est développé avec l'aide de Claude, d'Anthropic, pour le code, les correctifs, les scripts et la documentation. Chaque étape est vérifiée sur un vrai téléphone.
