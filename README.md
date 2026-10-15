# Rhénium

Portage communautaire de Chromium sur Windows 10 Mobile (Lumia, ARM32, UWP), pour remplacer EdgeHTML. Cahier des charges : https://claude.ai/code/artifact/e9e0892d-71ff-4fd1-8fa3-60c273049038

| Dossier | Contenu |
| --- | --- |
| `sonde\` | App UWP de mesure sur le téléphone (mémoire, JIT, Direct3D, A32, clang-cl, Rust, V8) : build, déploiement par USB, résultats exportés dans Téléchargements. Voir `sonde\README.md` |
| `j1\` | Jalon J1 : V8 11.6 compilé pour Windows ARM32, correctifs (`patches\`), cale et remplacements d'imports pour W10M. Voir `j1\README.md` |

Non versionnés, recréés par les scripts : `v8\` (sources de V8, `j1\fetch.ps1`), `toolchains\` (jonction vers LLVM, `j1\build-v8.ps1`), les dossiers `out\` et `sonde\certs\`.

Le chemin du projet ne doit pas contenir d'accent (`F:\Rhenium`) : GN et ninja de 2023 échouent sinon.
