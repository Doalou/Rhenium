# Jalon J3 : navigation tactile

Suite de J2 (`..\j2`, content_shell M116 qui affiche Wikipédia sur le Lumia). Le code reste dans le même arbre, le même correctif (`..\j2\patches\0003`) et la même app (`..\j2\app`, `..\j2\package-shell.ps1`).

## Fait

**Toucher (2026-10-18)** : défilement, zoom à deux doigts et liens marchent sur le Lumia 950.

- `ui/platform_window/uwp/uwp_window.cc` s'abonne aux événements de pointeur de la CoreWindow (appui, déplacement, relâchement, perte de capture), reçus sur le thread UI pendant `CoreDispatcher::ProcessEvents`. Toucher et stylet deviennent des `ui::TouchEvent` en pixels (position en DIP × échelle) ; une souris (Continuum) des `ui::MouseEvent`. Le reste est le pipeline tactile d'Aura : reconnaissance des gestes, défilement, zoom.
- Les identifiants de pointeur de Windows (648 au premier essai) dépassent la limite de Chromium (masque de 32 bits, `CHECK` dans `bitset_32.h`) : `ui::SequentialIDGenerator` les renumérote de 0 à 31.
- Rendu mobile : `--enable-viewport` (meta viewport), `--use-mobile-user-agent`, barres de défilement superposées (`OverlayScrollbar`).
- Copie partielle : `SoftwareOutputDeviceUwp` ne recopie plus que la zone redessinée de l'image et de la précédente (modèle flip à deux tampons), et l'image entière pendant les deux présentations qui suivent une création ou un redimensionnement.

**Saisie de texte (2026-10-18)** : le clavier virtuel s'affiche sur les champs, frappe, effacement et validation marchent (recherche de Wikipédia).

- `ui/base/ime/win/input_method_uwp.*` remplace `MockInputMethod`. Elle crée un `CoreTextEditContext` (politique d'affichage manuelle) et suit le champ focalisé. Pour un champ, elle appelle `NotifyFocusEnter`, règle l'`InputScope` selon le type du champ (recherche, URL, adresse, nombre, mot de passe…) et affiche le clavier (`InputPane::TryShow`) ; elle le masque à la sortie du champ.
- Mesuré sur le Lumia : les lettres arrivent par `TextUpdating` (appliquées par `InsertText`, ou `SetEditableSelectionRange` puis `InsertText` hors du curseur) ; l'effacement arrive par la touche `VK_BACK` de la CoreWindow, transmise à Blink, et le modèle du système est mis à jour (`NotifyTextChanged`). Un effacement par le texte passe par `ExtendSelectionAndDelete` (`InsertText` ignore un texte vide). Entrée et effacement sont dédoublonnés entre les deux voies.
- `UwpWindow` transmet les touches spéciales de la CoreWindow (Entrée, effacement, Tab, Échap, flèches, Suppr, début, fin, pages) avec leur code et leur `DomKey` explicites.
- Focus : la vue web n'avait jamais le focus Aura (demandé trop tôt, et perdu à chaque nouvelle vue après une navigation), donc la méthode de saisie ignorait ses champs. Sous `WINUWP`, `ShellPlatformDataAura` donne le focus à la fenêtre touchée, comme `wm::FocusController`.
- Traces de la saisie en `VLOG(1)` (option `--v=1` dans `rhenium-args.txt`).

## Reste à faire

| Sujet | Piste |
| --- | --- |
| Saisie de texte, suite | Composition et suggestions du clavier (soulignement), position du clavier qui masque le champ (`InputPane.Showing`), modèle de texte resynchronisé sur les changements faits par la page |
| Rotation, changement de taille | `SizeChanged` de la CoreWindow : nouvelles bornes, écran et swap chain redimensionnés |
| Barre d'adresse, précédent, recharger | Coquille minimale au-dessus de content_shell (MVP du cahier des charges) |
| Cycle de vie | `Suspending` / `Resuming` : rendu suspendu quand l'app est masquée |
| Cache HTTP sur disque | `Unable to open or create cache entry`, sans effet sur le chargement |
| Fluidité | Mesurer le défilement (enregistrement d'écran), puis rendu GPU (ANGLE) si le logiciel ne suffit pas |
