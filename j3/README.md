# Jalon J3 : navigation tactile

Suite de J2 (`..\j2`, content_shell M116 qui affiche Wikipédia sur le Lumia). Le code reste dans le même arbre, le même correctif (`..\j2\patches\0003`) et la même app (`..\j2\app`, `..\j2\package-shell.ps1`).

## Fait

**Toucher (2026-10-18)** : défilement, zoom à deux doigts et liens marchent sur le Lumia 950.

- `ui/platform_window/uwp/uwp_window.cc` s'abonne aux événements de pointeur de la CoreWindow (appui, déplacement, relâchement, perte de capture), reçus sur le thread UI pendant `CoreDispatcher::ProcessEvents`. Toucher et stylet deviennent des `ui::TouchEvent` en pixels (position en DIP × échelle) ; une souris (Continuum) des `ui::MouseEvent`. Le reste est le pipeline tactile d'Aura : reconnaissance des gestes, défilement, zoom.
- Les identifiants de pointeur de Windows (648 au premier essai) dépassent la limite de Chromium (masque de 32 bits, `CHECK` dans `bitset_32.h`) : `ui::SequentialIDGenerator` les renumérote de 0 à 31.
- Rendu mobile : `--enable-viewport` (meta viewport), `--use-mobile-user-agent`, barres de défilement superposées (`OverlayScrollbar`).
- Copie partielle : `SoftwareOutputDeviceUwp` ne recopie plus que la zone redessinée de l'image et de la précédente (modèle flip à deux tampons), et l'image entière pendant les deux présentations qui suivent une création ou un redimensionnement.

## Reste à faire

| Sujet | Piste |
| --- | --- |
| Saisie de texte | Clavier virtuel de W10M sur les champs HTML (`InputPane`, `CoreTextEditContext`) à la place de `MockInputMethod` |
| Rotation, changement de taille | `SizeChanged` de la CoreWindow : nouvelles bornes, écran et swap chain redimensionnés |
| Barre d'adresse, précédent, recharger | Coquille minimale au-dessus de content_shell (MVP du cahier des charges) |
| Cycle de vie | `Suspending` / `Resuming` : rendu suspendu quand l'app est masquée |
| Cache HTTP sur disque | `Unable to open or create cache entry`, sans effet sur le chargement |
| Fluidité | Mesurer le défilement (enregistrement d'écran), puis rendu GPU (ANGLE) si le logiciel ne suffit pas |
