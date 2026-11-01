/* Copyright 2026 Doalo
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Rhénium : contrat entre la coquille XAML (compilée par MSVC, avec
 * exceptions) et le moteur (Chromium, compilé par clang sans exceptions).
 * API C : aucun type C++ ne traverse la frontière. Tout s'appelle sur le
 * thread UI, celui de la CoreWindow, sauf attach_swap_chain (thread du
 * compositeur).
 */

#ifndef RHENIUM_ENGINE_H_
#define RHENIUM_ENGINE_H_

#include <unknwn.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RheniumHost {
  void* context;
  /* Zone de contenu (le SwapChainPanel) : taille en DIP et échelle. */
  float width_dip;
  float height_dip;
  float scale;
  /* Page de départ (sinon, celle de content_shell). */
  const wchar_t* start_url;

  /* Donne la swap chain du moteur au SwapChainPanel. Appelé depuis le thread
   * du compositeur : la coquille la pose sur le thread UI. */
  void (*attach_swap_chain)(void* context, IUnknown* swap_chain);
  void (*url_changed)(void* context, const wchar_t* url);
  void (*loading_changed)(void* context, int loading, int can_go_back,
                          int can_go_forward);
  void (*title_changed)(void* context, const wchar_t* title);
  /* Avancement du chargement, de 0 à 1. */
  void (*progress_changed)(void* context, double progress);
  /* Onglet ouvert, fermé ou activé. Les rappels ci-dessus suivent l'onglet
   * actif. */
  void (*tabs_changed)(void* context, int count);
  /* Page http(s) chargée dans un onglet, puis de nouveau à l'arrivée de son
   * titre (même adresse). */
  void (*page_visited)(void* context, const wchar_t* url, const wchar_t* title);
  /* Aperçu d'un onglet, en BGRA, valable pendant l'appel. */
  void (*tab_thumbnail)(void* context, int id, const unsigned char* bgra,
                        int width, int height, int stride);
  /* Fin d'une installation d'extension : son nom, ou la raison de l'échec. */
  void (*extension_installed)(void* context, int ok, const wchar_t* text);
  /* Langue des pages (Accept-Language), des extensions et des messages du
   * moteur : « fr-FR », « en-US »… NULL : celle du téléphone. */
  const wchar_t* language;
  /* Téléchargement nouveau, en cours (quatre fois par seconde au plus) ou
   * fini : état RHENIUM_DOWNLOAD_*, octets reçus et total (-1 s'il est
   * inconnu), nom du fichier (vide tant qu'il n'est pas choisi) et, une fois
   * terminé, son chemin dans LocalState, à déplacer par la coquille. */
  void (*download_changed)(void* context, int id, int state,
                           long long received, long long total,
                           const wchar_t* name, const wchar_t* path);
} RheniumHost;

enum {
  RHENIUM_DOWNLOAD_IN_PROGRESS = 0,
  RHENIUM_DOWNLOAD_COMPLETE = 1,
  RHENIUM_DOWNLOAD_FAILED = 2 /* interrompu ou annulé */
};

/* Journal de l'app (LocalState, recopié dans Phone\Downloads), à démarrer
 * dès wWinMain, et une ligne de la coquille. */
void RheniumEngineStartLog(void);
void RheniumEngineNote(const wchar_t* text);

/* Lance le moteur. Ne rend la main qu'à sa fermeture : sa boucle fait
 * tourner le CoreDispatcher, donc aussi XAML. */
int RheniumEngineRun(const RheniumHost* host);

/* Adresse ou recherche, telle que tapée dans la barre, dans l'onglet actif. */
void RheniumEngineNavigate(const wchar_t* text);
/* Moteur de recherche : préfixe http(s) auquel s'ajoute le texte cherché
 * (« https://duckduckgo.com/?q= », celui par défaut). Possible avant
 * RheniumEngineRun. */
void RheniumEngineSetSearchUrl(const wchar_t* prefix);
/* Ferme la page d'extension, sinon recule dans l'onglet, sinon ferme l'onglet
 * ouvert par un lien et revient à celui du lien. 0 s'il n'y avait rien à
 * faire. */
int RheniumEngineGoBack(void);
void RheniumEngineGoForward(void);
void RheniumEngineReload(void);
void RheniumEngineStop(void);

/* Onglets, identifiés par un entier stable. Nouvel onglet actif sur
 * l'adresse ou la recherche ; 0 au-delà de la limite (8 onglets). */
int RheniumEngineNewTab(const wchar_t* text);
/* Onglet d'une session précédente, en arrière-plan : sa page ne se charge
 * qu'à son activation. 0 au-delà de la limite. */
int RheniumEngineRestoreTab(const wchar_t* url, const wchar_t* title);
void RheniumEngineActivateTab(int id);
/* 0 pour le dernier onglet, qui reste. */
int RheniumEngineCloseTab(int id);
/* Parcourt les onglets dans l'ordre ; les chaînes valent pendant l'appel. */
void RheniumEngineListTabs(void (*visit)(void* context, int id, int active,
                                         const wchar_t* title,
                                         const wchar_t* url),
                           void* context);
/* Demande un aperçu de l'onglet actif (rappel tab_thumbnail). Celui d'un
 * onglet qu'on quitte est pris tout seul. */
void RheniumEngineCaptureTab(void);
/* Mémoire critique : caches du moteur vidés et page de l'onglet inactif
 * utilisé le moins récemment déchargée ; elle garde son historique et se
 * recharge à son activation. 0 s'il n'y avait plus d'onglet à décharger. */
int RheniumEngineDiscardTab(void);

/* Popup ou page d'options de l'extension (uBlock Origin Lite), par-dessus la
 * page et fermés par GoBack ; 0 si aucune extension n'en a. */
int RheniumEngineOpenExtensionPopup(void);
int RheniumEngineOpenExtensionOptions(void);

/* Extensions installées, celles du paquet d'abord. Identifiant de 32
 * lettres ; « removable » vaut 1 pour celles du Chrome Web Store. Les chaînes
 * valent pendant l'appel. */
void RheniumEngineListExtensions(void (*visit)(void* context, const char* id,
                                               const wchar_t* name,
                                               const wchar_t* version,
                                               int enabled, int removable,
                                               int has_popup, int has_options),
                                 void* context);
int RheniumEngineSetExtensionEnabled(const char* id, int enabled);
int RheniumEngineUninstallExtension(const char* id);
int RheniumEngineOpenExtensionPopupFor(const char* id);
int RheniumEngineOpenExtensionOptionsFor(const char* id);
/* Identifiant (33 caractères avec le zéro final) de l'extension dont `url`
 * est la page sur le Chrome Web Store ; 0 si ce n'en est pas une. */
int RheniumEngineWebStoreId(const wchar_t* url, char id[33]);
/* Télécharge, vérifie et installe l'extension (rappel extension_installed).
 * 0 si une autre installation est en cours. */
int RheniumEngineInstallExtension(const char* id);

void RheniumEngineCancelDownload(int id);

/* Version de Chromium, par exemple « 116.0.5845.228 ». */
const wchar_t* RheniumEngineChromiumVersion(void);

/* Pointeurs sur la zone de contenu, en DIP. */
enum {
  RHENIUM_POINTER_PRESSED = 0,
  RHENIUM_POINTER_MOVED = 1,
  RHENIUM_POINTER_RELEASED = 2,
  RHENIUM_POINTER_CANCELED = 3
};
void RheniumEnginePointer(int kind, unsigned int id, float x_dip, float y_dip,
                          int is_mouse);

/* La page a-t-elle le clavier (sinon la barre d'adresse) ? */
void RheniumEngineSetWebFocus(int focused);

/* Nouvelle taille de la zone de contenu (rotation, boutons du téléphone). */
void RheniumEngineResize(float width_dip, float height_dip);
/* Hauteur de la zone de contenu masquée par le clavier, en DIP : la page
 * fait défiler le champ focalisé au-dessus. */
void RheniumEngineSetKeyboardInset(float bottom_dip);
/* App visible ou masquée (écran d'accueil, verrouillage) : rendu suspendu,
 * et à la mise en arrière-plan, préférences, cookies et stockage des sites
 * écrits tout de suite. */
void RheniumEngineSetVisible(int visible);

/* Efface cookies, cache et données des sites (pas celles des extensions),
 * puis appelle `done` sur le thread UI. */
void RheniumEngineClearBrowsingData(void (*done)(void* context),
                                    void* context);

#ifdef __cplusplus
}
#endif

#endif /* RHENIUM_ENGINE_H_ */
