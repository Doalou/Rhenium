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
  /* Page de départ ; nulle : page de test du moteur. */
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
} RheniumHost;

/* Journal de l'app (LocalState, recopié dans Phone\Downloads), à démarrer
 * dès wWinMain, et une ligne de la coquille. */
void RheniumEngineStartLog(void);
void RheniumEngineNote(const wchar_t* text);

/* Lance le moteur. Ne rend la main qu'à sa fermeture : sa boucle fait
 * tourner le CoreDispatcher, donc aussi XAML. */
int RheniumEngineRun(const RheniumHost* host);

/* Adresse ou recherche, telle que tapée dans la barre. */
void RheniumEngineNavigate(const wchar_t* text);
void RheniumEngineGoBack(void);
void RheniumEngineGoForward(void);
void RheniumEngineReload(void);
void RheniumEngineStop(void);

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

#ifdef __cplusplus
}
#endif

#endif /* RHENIUM_ENGINE_H_ */
