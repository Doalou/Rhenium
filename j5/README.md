# Jalon J5 : v1

Objectif : un navigateur de tous les jours. Onglets, historique, favoris, téléchargements et profil persistant, chacun validé sur le Lumia 950. Le code reste dans l'arbre et les correctifs de J2 (`..\j2\patches\0003`), la coquille dans `..\j2\shell\shell_app.cpp`.

Déjà faits pendant J4 (voir `..\j4\README.md`) : onglets (8 au plus, avec aperçus), historique (`LocalState\history.txt`), restauration de session (`LocalState\tabs.txt`).

## Fait

**Profil persistant (2026-11-01)** : une connexion à un site survit à la fermeture de l'app ; validé sur le Lumia en 0.4.26305.882.

- content_shell ne donnait aucun fichier à son contexte réseau : les cookies restaient en mémoire. Sous `WINUWP` (`ShellContentBrowserClient::ConfigureNetworkContextParamsForShell`), cookies (`Cookies`) et HSTS (`TransportSecurity`) sont écrits dans le dossier du profil.
- Cookies de session gardés et rechargés (`persist_session_cookies`, `restore_old_session_cookies`) : W10M tue l'app sans prévenir, et une session ouverte doit survivre comme dans les navigateurs mobiles.
- Cookies non chiffrés : OSCrypt n'est pas initialisé dans content_shell, et le dossier de l'app est déjà privé.
- À la mise en arrière-plan (`RheniumEngineSetVisible(0)`), cookies (sinon écrits par lots toutes les 30 s) et stockage des sites (`StoragePartition::Flush`) sont écrits tout de suite, comme les préférences.
- Paramètres, « Confidentialité » : « Effacer les cookies et les données des sites » (`RheniumEngineClearBrowsingData`). Cookies, cache HTTP et stockage des sites (localStorage, IndexedDB, service workers…), sauf ce qui appartient aux extensions (`chrome-extension://`) : uBO Lite garde ses réglages et son service worker.

**Favoris (2026-11-01)** : validés sur le Lumia en 0.4.26305.896, dans la coquille seulement.

- « … » : l'étoile « Favori » de la rangée d'icônes ajoute la page de l'onglet actif ou la retire (pleine si elle y est, grisée hors http(s)) ; la commande « Favoris » ouvre le panneau.
- Panneau plein écran comme l'historique, du plus récent au plus ancien. Un toucher ouvre le favori, un appui long propose « Renommer » et « Supprimer ».
- `LocalState\favorites.txt` : adresse et titre par ligne, écrit hors du thread UI puis renommé (`SaveLater`, commun avec l'historique).

**Téléchargements (2026-11-01)** : validés sur le Lumia en 0.4.26305.922.

- Pas de question : sous `WINUWP`, le délégué de content_shell enregistre dans le dossier `Downloads` du profil, avec un nom unique (`base::GetUniquePath`) et un fichier intermédiaire `.crdownload`.
- `extensions\rhenium\rhenium_downloads.cc` suit le `DownloadManager` du profil et prévient la coquille (`download_changed`) : nouveau, avancement (toutes les 250 ms au plus), terminé ou en échec. `RheniumEngineCancelDownload` annule.
- Fichier terminé : la coquille le déplace dans Téléchargements (`DownloadsFolder.CreateFileAsync` avec `GenerateUniqueName`, puis `MoveAndReplaceAsync`) et garde un jeton `FutureAccessList` pour le rouvrir. Il arrive dans le sous-dossier que Windows réserve à l'app, `Téléchargements\Rhenium.Shell_h0km98jdnqqew!App`.
- Bandeau en bas de l'écran : nom, avancement, « Annuler » puis « Ouvrir » (`Launcher.LaunchFileAsync` : les PDF vont au lecteur du système) ; il disparaît 8 s après la fin.
- « … », « Téléchargements » : panneau plein écran, du plus récent au plus ancien. Un toucher ouvre le fichier, un appui long propose « Annuler », « Supprimer le fichier » ou « Retirer de la liste » ; « Vider la liste » retire les téléchargements finis.
- `LocalState\downloads.txt` : 50 téléchargements au plus, les plus anciens finis s'en vont.

**Liseuse PDF (2026-11-01)** : validée sur le Lumia en 0.4.26305.1049.

- PDF.js 5.4.624 de Mozilla, version « legacy » : la dernière qui vise Chrome 110 (les suivantes demandent Chrome 118). `fetch-pdfjs.ps1` la récupère, vérifie son empreinte et en fait l'extension `out\pdfjs\PdfViewer` (7 Mo sans les cartes de sources, le débogueur, le bac à sable des scripts et les langues autres que fr et en-US), copiée par `package-shell.ps1` dans `Extensions\PdfViewer` comme uBO Lite. Licence Apache 2.0 : `pdfjs\SOURCES.txt`.
- Manifeste avec une clé fixe : identifiant `jofiphiaggcjbefdfkpajonmlakcbkng`, quel que soit le dossier. Changer le quatrième nombre de la version quand `pdfjs\` change, sinon le profil garde l'ancienne copie.
- `extensions\rhenium\rhenium_pdf.cc` : une réponse `application/pdf` du cadre principal, ni jointe (`Content-Disposition: attachment`) ni issue d'un POST, est annulée et l'onglet ouvre `web/viewer.html?url=<adresse>`. Le reste, ou tout PDF si l'extension est désactivée dans les paramètres, est téléchargé.
- `pdfjs\rhenium.js` ouvre le PDF une fois la liseuse prête : son paramètre `file` refuse une autre origine que la sienne. Les requêtes passent grâce à `<all_urls>` (liste d'accès CORS de l'extension), avec les cookies (`withCredentials`, et le schéma `chrome-extension` compté comme premier niveau, comme dans Chrome). Préférences, scripts des PDF, `eval` et annotations désactivés ; `rhenium.css` masque impression, ouverture de fichier, plein écran et outils d'édition.
- Barre d'adresse, historique, favoris et onglets restaurés gardent l'adresse du PDF (`PdfSourceUrl`, et `ShellUwpHost::displayed_url` pour la barre).

**Vidéo fluide (2026-11-01)** : YouTube à 24 images/s sans image perdue, en 360p comme en 720p, mesuré sur le Lumia en 0.4.26305.1100.

- Mesures dans le journal : décodage VP9 (`vpx_video_decoder.cc`, toutes les 300 trames), images décodées et perdues (`video_renderer_impl.cc`), et pour chaque série de 120 images la cadence, le dessin par Skia, la copie vers la swap chain et la présentation (`SoftwareOutputDeviceUwp`).
- Le décodage VP9 (libvpx NEON) n'était pas en cause : 6 ms par image en 360p, 11 à 13 ms en 720p, pour 41,7 ms par image à 24 images/s. Le dessin de l'image vidéo prenait 30 ms sur 32 : Skia l'agrandissait (640x360 vers 1083x609 pixels) avec un filtrage bilinéaire en virgule flottante. 3 à 15 % des images étaient perdues.
- `SoftwareRenderer::DrawTextureQuad` (viz) : une image vidéo sous une transformation d'échelle et de translation est mise à sa taille à l'écran par `libyuv::ARGBScale` (bilinéaire, NEON), puis recopiée telle quelle par Skia. L'image mise à l'échelle est gardée tant que l'image source et sa place ne changent pas. Dessin de la vidéo : 7 ms en 360p, 12 ms en 720p ; image entière 9 ms et 15 à 17 ms.
- Pas de décodeur matériel exposé aux apps sur le Lumia 950 : Media Foundation n'y propose que les décodeurs logiciels de Microsoft (H.264 et HEVC), aucun pour VP9. Le H.264 n'apporterait rien.
- YouTube choisit lui-même le 360p dans la page ; le 720p se choisit dans le menu de qualité du lecteur.

**Onglets inactifs (2026-11-01)** : déchargés faute de mémoire, pas encore vus à l'œuvre (la limite de l'app, 1 536 Mo au premier plan, n'a pas été atteinte).

- La coquille suit `MemoryManager` : au niveau `High` ou au-dessus, ou à plus de 90 % de la limite (y compris quand elle baisse, à la mise en arrière-plan), `RheniumEngineDiscardTab` vide les caches du moteur (`MemoryPressureListener`, niveau critique) et décharge l'onglet inactif utilisé le moins récemment ; un autre toutes les 2 s tant que la mémoire reste haute.
- Déchargement (`rhenium::DiscardTab`) : un Shell vide remplace celui de l'onglet et reprend son historique (`NavigationController::CopyStateFrom`), puis l'ancien est fermé. À l'activation, la page se recharge (`SetNeedsReload`, `LoadIfNecessary`) ; précédent et suivant restent possibles, l'aperçu reste dans la liste des onglets.

**Version pour ordinateur (2026-11-01)** : validée sur le Lumia en 0.4.26305.1175.

- Paramètres, « Version des sites » : Mobile (Chrome pour Android, par défaut) ou Ordinateur (Chrome pour Windows). Réglage `desktop_sites` de l'app, qui vaut pour les onglets ouverts dès leur prochain chargement (`RheniumEngineSetDesktopSites`).
- « … », « Version pour ordinateur » : bascule l'onglet actif et recharge sa page à son adresse d'origine (`ReloadType::ORIGINAL_REQUEST_URL`), coche quand elle est active. Un onglet ouvert par un lien prend la version de celui du lien.
- `rhenium_tab_model.cc` : chaque onglet porte son choix, appliqué à chaque navigation du cadre principal (`SetIsOverridingUserAgent`), comme pour le Chrome Web Store. Les indices (`Sec-CH-UA-Platform`, `Sec-CH-UA-Mobile`) suivent l'agent utilisateur : certains sites, dont Google, s'y fient plus qu'à lui.

**Mode sombre (2026-11-01)** : validé sur le Lumia en 0.4.26305.1223.

- Paramètres, « Thème » : Comme le téléphone (par défaut), Clair ou Sombre (réglage `theme`). La coquille suit le thème du téléphone quand il change, ou au retour au premier plan (`UISettings.ColorValuesChanged`).
- Pages : `prefers-color-scheme` vaut `dark` ou `light` (`RheniumContentBrowserClient::OverrideWebkitPrefs`, `rhenium::ApplyDarkMode`) ; un changement s'applique tout de suite aux onglets ouverts et à la page d'extension (`NotifyPreferencesChanged`), sans rechargement.
- « Assombrir aussi les pages claires » (réglage `force_dark`), en thème sombre seulement : assombrissement automatique de Blink (`force_dark_mode_enabled`).
- Coquille : couleurs en deux palettes (`kDarkInks`, `kLightInks`) et pinceaux partagés, recolorés sans reconstruire l'interface ; `RequestedTheme` de la racine, barre d'état du téléphone.

## Pièges

- Les menus surgissants (`MenuFlyout`) et les boîtes de dialogue ne suivent pas le `RequestedTheme` de la racine, seulement celui de l'app, qui ne se règle qu'au lancement. Les boîtes reçoivent le thème de la coquille à leur ouverture, les menus d'appui long par le style de leur `MenuFlyoutPresenter` ; les listes déroulantes des Paramètres gardent celui du lancement.

- Appui long sur un bouton XAML au toucher : `RightTapped` arrive au relâchement, avec le `Click` du bouton, qui ouvrait la page en plus du menu. Le menu s'ouvre au début de l'appui (`Holding`, état `Started`) et le `Click` qui suit est ignoré ; `RightTapped` ne sert plus qu'à la souris (`ListItem`, pour les favoris, l'historique et les téléchargements).
- Tous les téléchargements échouaient à la fin : `BaseFile::MoveFileAndAdjustPermissions` renomme le `.crdownload` par `IFileOperation` (shell32), absent pour les apps UWP. Sous `WINUWP`, `base::Move` le remplace (`base_file_win.cc`) ; la raison d'une interruption est maintenant écrite dans le journal.
- Le dernier onglet n'était pas toujours rouvert : `tabs.txt` ne s'écrivait qu'à la mise en arrière-plan, et une app tuée au premier plan (mise à jour, plantage) gardait une session ancienne. Les onglets s'écrivent maintenant aussi à chaque page chargée et à chaque changement d'onglet (`SaveTabs`, hors du thread UI) ; l'écriture immédiate de la mise en arrière-plan abandonne les écritures en attente, plus anciennes. Rien n'est écrit tant que les onglets de la session précédente ne sont pas tous rouverts.
- Une première mise à l'échelle dans `VideoResourceUpdater`, appelée par `VideoLayerImpl`, ne servait à rien : dans Chromium 116, la vidéo passe par `VideoFrameSubmitter` et sa propre surface, sans connaître sa taille à l'écran. C'est viz qui la connaît.

## Reste à faire

| Sujet | Piste |
| --- | --- |
| Vidéo en plein écran | `EnterFullscreenModeForTab` dans la coquille (barre masquée, paysage) ; vérifier le coût de la mise à l'échelle vers 1920x1080 |
| Démarrage d'une vidéo | Lent sur YouTube : à mesurer (chargement de la page, du lecteur, premier tampon) |
| Qualité vidéo par défaut | YouTube choisit le 360p dans la page ; le 720p tient, à proposer plus tôt si possible |
| Continuum | Non traité (prévu au cahier des charges en v1, sans garantie) : fenêtre sur le second écran, taille, clavier et souris |
| Thème des listes déroulantes | Suivre un changement de thème sans relancer l'app |
