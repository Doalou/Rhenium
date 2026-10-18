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

**Coquille XAML (2026-10-19)** : barre en bas comme Edge sur mobile (précédent, adresse ou recherche, recharger ou arrêter, fil de progression), bouton retour du téléphone.

- `..\j2\shell\shell_app.cpp` : app C++/WinRT (`Application`), interface construite en code, compilée par MSVC avec exceptions (`..\j2\shell\build-shell.ps1`, appelé par `..\j2\build-content.ps1`) dans `rhenium_xaml_shell.lib`, liée avec le moteur clang dans `rhenium_shell.exe`.
- Frontière en C (`..\j2\app\rhenium_engine.h`) : la coquille donne la taille de la zone de contenu et des rappels (swap chain, URL, chargement, progression) ; le moteur (`..\j2\app\engine_uwp.cc`, ex-`shell_uwp_main.cc`) expose navigation, pointeurs et focus. Aucune exception ne traverse.
- Le moteur tourne dans un rappel du `CoreDispatcher` : sa boucle (`MessagePumpForUI`) appelle `ProcessEvents`, donc XAML continue de dessiner et de recevoir les entrées.
- Mode hébergé : swap chain de composition (`CreateSwapChainForComposition`, `SetMatrixTransform` à 1/échelle) posée sur un `SwapChainPanel`. Le panneau refuse un fond, donc une grille transparente au-dessus reçoit les pointeurs et les transmet au moteur.
- Zone visible : fenêtre en `UseCoreWindow`, marges calées sur `VisibleBounds` (barre d'état 24 DIP, boutons du téléphone 48 DIP) et barre d'état aux couleurs de la barre.
- Barre d'adresse : le domaine hors saisie, l'adresse complète à la saisie ; un mot avec un point devient `https://…`, sinon une recherche DuckDuckGo. Progression réelle par `LoadProgressChanged`, étendu de content_shell Android à `WINUWP`.
- Journal : la coquille y note ses étapes ; une exception XAML non gérée y est écrite avec son message (ainsi trouvé : « Setting 'Background' property is not supported on SwapChainPanel »).

**Fin de J3 (2026-10-19)** : validé sur le Lumia (rotation, clavier, défilement, retour depuis l'accueil, YouTube, sites en version mobile).

- Redimensionnement : `SizeChanged` du `SwapChainPanel` (rotation, boutons du téléphone masqués) appelle `RheniumEngineResize`. Le moteur met à jour la zone hébergée (`gfx::uwp::UpdateHostedSize`), l'écran (`display::ScreenBase`), la fenêtre de plateforme (`ui::ResizeHostedWindow`, qui redimensionne aussi la swap chain) et la fenêtre de la page.
- Clavier sur un champ de la page : la coquille garde sa mise en page (`EnsuredFocusedElementInView`) et passe la hauteur masquée à `RenderWidgetHostView::SetInsets`, qui fait défiler le champ au-dessus du clavier. Sur la barre d'adresse, XAML remonte lui-même l'interface.
- Cycle de vie : `VisibilityChanged` de la fenêtre appelle `WebContents::WasHidden` / `WasShown` (écran d'accueil, verrouillage, suspension).
- Saisie : le modèle de texte du système est resynchronisé quand la page change le texte ou le curseur (`InputMethodUwp::ResyncWithClient`, ignoré 500 ms après une frappe, le temps que Blink la reflète).
- Cache HTTP : content_shell n'avait qu'un cache en mémoire (le message `Unable to open or create cache entry` est un `DLOG` bénin). Sous `WINUWP`, cache disque de 64 Mo dans `data\Cache`, en backend simple (blockfile projette ses fichiers par `CreateFileMapping`, refusé aux apps).
- Fluidité : rendu à l'échelle 3 au plus (au lieu de 4 sur le Lumia 950), agrandi par la composition : 44 % de pixels en moins à rastériser. `--rhenium-render-scale=4` dans `rhenium-args.txt` rend la définition native. Le journal note la cadence par seconde d'activité (`Cadence : N images/s`), de 15 à 40 images/s au défilement en paysage.
- Agent utilisateur : celui, réduit, de Chrome 116 pour Android, mobile, et des indices client cohérents (plateforme Android) ; avec « Windows NT 10.0 », les sites servaient leur version bureau. `Accept-Language` : langue du téléphone d'abord.
- YouTube : en single-process, `MediaDevicesManager` abonnait le service audio intégré aux changements de périphériques, ce qu'il refuse (`DCHECK` dans `audio::Service::BindDeviceNotifier`) ; l'abonnement distant est réservé au service hors processus.

## Reste à faire

| Sujet | Piste |
| --- | --- |
| Lecture vidéo | Décodeurs coupés à la compilation (`media_use_ffmpeg`, `media_use_libvpx`, `enable_dav1d_decoder` à false) : VP9 / H.264 logiciels, ou décodage matériel par Media Foundation |
| Saisie de texte, suite | Composition et suggestions du clavier (soulignement) ; champs longs (Blink n'expose qu'une partie du texte, pas de resynchronisation) |
| Coquille, suite | Avancer, onglets, favoris, menu ; barre masquée au défilement |
| Fluidité | Rendu GPU (ANGLE sur D3D11) si le logiciel ne suffit pas ; mesurer à l'échelle 3 et 4 |
