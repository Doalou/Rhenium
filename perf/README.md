# Performances : division entière dans V8, NEON dans libvpx

Deux gains repérés par la relecture du 2026-10-19, mesurés sur le Lumia 950.

## V8 : détection du processeur sous Windows ARM32

`v8/src/base/cpu.cc` ne lisait les fonctions du processeur ARM que sous Linux et QNX. Sous Windows, `has_idiva_`, `has_neon_` et `has_vfp3_d32_` restaient faux : le JIT s'en tenait à ce que le compilateur garantit (ARMv7 + NEON, sans `sdiv`/`udiv`) et faisait les divisions entières en virgule flottante (VFP), ou par appel de fonction en wasm.

Correctif (`j1\patches\0002`) : branche `V8_OS_WIN` qui lit `IsProcessorFeaturePresent` (division 24, NEON 19, 32 registres VFP 18, ARMv8 29). Sur le 950 (Cortex-A57/A53), V8 passe en `kArmv8` : `sdiv`/`udiv` et les instructions ARMv8 en AArch32. `--js-flags=--arm-arch=armv7` dans `rhenium-args.txt` revient à l'ancien comportement.

Banc : `banc-division.html` (division, modulo, division non signée, témoin sans division, et les mêmes en wasm), médiane de 5 tours.

## libvpx : intrinsèques NEON

libvpx était compilé en C pur sous winuwp (`cpu_arch_full = "generic"`), car son assembleur NEON passe par Perl et suppose le mode ARM. Chaque fichier `.asm` a un équivalent en intrinsèques (`vpx_dsp.mk`, branche sans `HAVE_NEON_ASM`) : la configuration `source/config/winuwp/arm-neon` est celle de `linux/arm-neon` avec `HAVE_NEON_ASM` et `HAVE_PTHREAD_H` à 0, et `BUILD.gn` remplace les fichiers `.asm` par leurs versions `.c`. Les rtcd sont les mêmes (mêmes noms de fonctions `_neon`). VP9 reste en 8 bits (profil 0), comme toutes les configurations ARM de Chromium hors ChromeOS.

Mesure : `media/filters/vpx_video_decoder.cc` écrit dans le journal, toutes les 300 trames, le temps moyen et maximal de `vpx_codec_decode` (`Rhenium : vp9 640x360, 300 trames, decodage moyen … ms`).

## Résultats

Banc de division du 2026-10-20 (paquet 0.4.26293.140, `processeur : division 1, NEON 1, VFP 32 registres 1, ARMv8 1` dans le journal), trois sessions de sept tours sur le Lumia 950 ; médiane des sessions, puis chaque session (médiane de ses tours) :

| Mesure | Médiane | Sessions |
| --- | ---: | --- |
| JS division | 41,3 ms | 25,5 / 41,3 / 41,4 |
| JS modulo | 20,9 ms | 20,9 / 24,4 / 20,8 |
| JS division non signée | 47,4 ms | 47,3 / 47,4 / 48,3 |
| JS témoin (décalage) | 10,2 ms | 10,2 / 10,1 / 40,6 |
| JS témoin entier | 6,6 ms | 6,6 / 6,6 / 41,0 |
| Wasm `i32.div_s` | 25,3 ms | 25,0 / 25,4 / 25,3 |
| Wasm `i32.rem_u` | 25,8 ms | 27,8 / 25,6 / 25,8 |

Les écarts d'une session à l'autre (témoins à 40 ms dans la troisième, division à 25 puis 41 ms) viennent sans doute du passage d'un cœur Cortex-A57 à un Cortex-A53 : le premier tour de chaque session est écarté par la médiane, pas ce changement de cœur. Le wasm, stable à 25 ms, utilise `sdiv`/`udiv`. La mesure « avant » (`--js-flags=--arm-arch=armv7` dans `rhenium-args.txt`) reste à faire pour chiffrer le gain, de même que le temps de décodage VP9.
