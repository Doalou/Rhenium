// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : coquille XAML de l'app (C++/WinRT, compilée par MSVC avec
// exceptions). Le moteur (Chromium, clang, sans exceptions) s'affiche dans un
// SwapChainPanel et se commande par l'API C de rhenium_engine.h. Aucune
// exception ne doit remonter vers le moteur : tout rappel venu de lui est
// enveloppé.
//
// Interface construite en code, comme Edge sur Windows 10 Mobile : en bas,
// l'adresse, le bouton des onglets et « … », qui déplie au-dessus un menu
// (icônes puis commandes). Onglets, favoris, historique, téléchargements et
// paramètres s'ouvrent en plein écran ; un bandeau suit le dernier
// téléchargement. Le précédent passe par le bouton retour du téléphone.

#include <windows.h>

#include <dxgi.h>
#include <stdio.h>
#include <unknwn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <windows.ui.xaml.media.dxinterop.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.System.Threading.h>
#include <winrt/Windows.System.UserProfile.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Interop.h>
#include <winrt/Windows.UI.Xaml.Media.Animation.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include "rhenium_engine.h"

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using Windows::ApplicationModel::DataTransfer::DataTransferManager;
using Windows::UI::Color;
using Windows::UI::Core::CoreDispatcher;
using Windows::UI::Core::CoreDispatcherPriority;
using Windows::UI::ViewManagement::ApplicationView;
using Windows::UI::ViewManagement::InputPane;
using Windows::UI::ViewManagement::InputPaneVisibilityEventArgs;

namespace {

constexpr wchar_t kHomeUrlSetting[] = L"home_url";
// « fr-FR » ou « en-US » ; absent : langue du téléphone.
constexpr wchar_t kLanguageSetting[] = L"language";
// Présent (vrai) : sites en version pour ordinateur.
constexpr wchar_t kDesktopSitesSetting[] = L"desktop_sites";
// « light » ou « dark » ; absent : thème du téléphone.
constexpr wchar_t kThemeSetting[] = L"theme";
// Présent (vrai) : pages claires assombries en thème sombre.
constexpr wchar_t kForceDarkSetting[] = L"force_dark";

// Moteurs de recherche proposés, le premier par défaut ; le texte cherché
// s'ajoute au préfixe.
struct SearchEngine {
  wchar_t const* name;
  wchar_t const* prefix;
};
constexpr SearchEngine kSearchEngines[] = {
    {L"DuckDuckGo", L"https://duckduckgo.com/?q="},
    {L"Bing", L"https://www.bing.com/search?q="},
    {L"Ecosia", L"https://www.ecosia.org/search?q="},
    {L"Google", L"https://www.google.com/search?q="},
    {L"Qwant", L"https://www.qwant.com/?q="},
};
constexpr wchar_t kSearchEngineSetting[] = L"search_engine";  // son nom
constexpr wchar_t kWebStoreUrl[] = L"https://chromewebstore.google.com/";
constexpr wchar_t kUbolSourcesUrl[] =
    L"https://github.com/gorhill/uBlock/tree/"
    L"d8a77f38ceac203c8f146213e40b976e55ccfbe3";

constexpr wchar_t kHistoryFile[] = L"history.txt";
constexpr wchar_t kTabsFile[] = L"tabs.txt";
constexpr wchar_t kFavoritesFile[] = L"favorites.txt";
constexpr wchar_t kDownloadsFile[] = L"downloads.txt";
// Au-delà, les téléchargements les plus anciens quittent la liste (pas le
// dossier Téléchargements).
constexpr size_t kMaxDownloads = 50;
// Au-delà, les visites les plus anciennes sont oubliées.
constexpr size_t kMaxHistory = 500;
// Visites affichées d'un coup ; « Plus » en ajoute autant.
constexpr size_t kHistoryPage = 60;
constexpr int kMaxTabs = 8;  // limite du moteur (rhenium_tab_model.h)

// Segoe MDL2 Assets.
constexpr wchar_t kGlyphForward[] = L"\xE72A";
constexpr wchar_t kGlyphReload[] = L"\xE72C";
constexpr wchar_t kGlyphCancel[] = L"\xE711";  // Arrêter, fermer
constexpr wchar_t kGlyphShare[] = L"\xE72D";
constexpr wchar_t kGlyphShield[] = L"\xEA18";
constexpr wchar_t kGlyphMore[] = L"\xE712";
constexpr wchar_t kGlyphAdd[] = L"\xE710";
constexpr wchar_t kGlyphFavorite[] = L"\xE734";
constexpr wchar_t kGlyphFavoriteFill[] = L"\xE735";
constexpr wchar_t kGlyphCheck[] = L"\xE73E";

// Teintes de la coquille. Leurs pinceaux sont partagés : un changement de
// thème change leur couleur partout.
enum Ink {
  kBarColor,
  kMenuColor,
  kFieldColor,
  kFieldFocusedColor,
  kFieldFocusedTextColor,
  kTextColor,
  kDimTextColor,
  kInkCount
};
constexpr Color kDarkInks[kInkCount] = {
    {0xFF, 0x1F, 0x1F, 0x1F}, {0xFF, 0x2B, 0x2B, 0x2B},
    {0xFF, 0x33, 0x33, 0x33}, {0xFF, 0xF2, 0xF2, 0xF2},
    {0xFF, 0x1F, 0x1F, 0x1F}, {0xFF, 0xE6, 0xE6, 0xE6},
    {0xFF, 0x99, 0x99, 0x99}};
constexpr Color kLightInks[kInkCount] = {
    {0xFF, 0xF2, 0xF2, 0xF2}, {0xFF, 0xE6, 0xE6, 0xE6},
    {0xFF, 0xDE, 0xDE, 0xDE}, {0xFF, 0xFF, 0xFF, 0xFF},
    {0xFF, 0x1A, 0x1A, 0x1A}, {0xFF, 0x1A, 0x1A, 0x1A},
    {0xFF, 0x6B, 0x6B, 0x6B}};
constexpr double kBarHeight = 48;

struct HistoryEntry {
  long long time;  // FILETIME, UTC
  std::wstring url;
  std::wstring title;
};

struct SavedTab {
  std::wstring url;
  std::wstring title;
};

struct Favorite {
  std::wstring url;
  std::wstring title;
};

// Téléchargement de cette session (id du moteur) ou d'une précédente (id 0).
// Une fois déplacé dans Téléchargements, le fichier se retrouve par son jeton
// de la FutureAccessList.
struct DownloadEntry {
  int id = 0;
  long long time = 0;  // FILETIME, UTC : identifie la ligne
  std::wstring name;
  int state = RHENIUM_DOWNLOAD_IN_PROGRESS;
  long long received = 0;
  long long total = -1;
  bool moving = false;  // terminé, en cours de déplacement
  std::wstring token;
};

struct ShellState {
  Border root{nullptr};
  SwapChainPanel panel{nullptr};
  Grid input_surface{nullptr};  // reçoit les pointeurs au-dessus du panneau
  TextBox address{nullptr};
  ContentControl focus_sink{nullptr};  // garde le focus XAML pendant la page
  ProgressBar progress{nullptr};
  std::vector<TextBlock> tab_counts;  // dans les boutons des onglets
  Border menu{nullptr};
  Border dismiss_layer{nullptr};  // ferme le menu au toucher de la page
  Button forward{nullptr};
  FontIcon reload_icon{nullptr};
  TextBlock reload_label{nullptr};
  // Panneaux plein écran, fermés par le bouton retour.
  Grid settings{nullptr};
  TextBox home_url{nullptr};
  StackPanel extension_list{nullptr};
  Button add_extension{nullptr};  // menu, sur une page du Chrome Web Store
  FontIcon desktop_check{nullptr};  // menu, onglet en version pour ordinateur
  bool installing = false;
  Grid tabs{nullptr};
  Grid tab_cards{nullptr};
  Button new_tab{nullptr};
  Grid history{nullptr};
  StackPanel history_list{nullptr};
  Grid favorites_panel{nullptr};
  StackPanel favorite_list{nullptr};
  FontIcon favorite_icon{nullptr};  // étoile du menu, pleine sur un favori
  Grid downloads_panel{nullptr};
  StackPanel download_list{nullptr};
  std::map<long long, TextBlock> download_details;  // lignes affichées
  // Bandeau du dernier téléchargement, au bas de la page.
  Grid banner{nullptr};
  TextBlock banner_name{nullptr};
  TextBlock banner_detail{nullptr};
  ProgressBar banner_progress{nullptr};
  Button banner_action{nullptr};
  DispatcherTimer banner_timer{nullptr};
  long long banner_time = 0;
  DispatcherTimer memory_timer{nullptr};
  CoreDispatcher dispatcher{nullptr};
  std::vector<SolidColorBrush> inks;  // une par Ink
  bool dark = true;
  Windows::UI::ViewManagement::UISettings ui_settings{nullptr};
  RheniumHost host{};
  std::wstring url;
  std::wstring title;
  std::wstring start_url;
  // Langue passée au moteur (réglage, vide pour celle du téléphone), qui ne
  // change qu'au lancement suivant.
  std::wstring engine_language;
  bool engine_english = false;
  bool engine_started = false;
  bool loading = false;
  bool can_go_forward = false;
  int tab_count_value = 1;
  // Aperçus des onglets, et leurs images dans le sélecteur ouvert.
  std::map<int, Imaging::WriteableBitmap> thumbnails;
  std::map<int, Image> card_images;
  std::vector<HistoryEntry> visits;  // de la plus ancienne à la plus récente
  std::vector<SavedTab> restore;  // onglets à rouvrir après le premier
  std::vector<Favorite> favorites;  // du plus ancien au plus récent
  std::vector<DownloadEntry> downloads;  // du plus ancien au plus récent
  size_t history_shown = 0;
  int history_day = 0;  // dernier jour affiché, AAAAMMJJ
};

ShellState* g_shell = nullptr;  // une seule fenêtre, pour toute la vie de l'app

// Interface en anglais US, sinon en français.
bool g_english = false;

wchar_t const* Tr(wchar_t const* french, wchar_t const* english) {
  return g_english ? english : french;
}

std::wstring LanguageSetting() {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  return std::wstring(unbox_value_or<hstring>(
      values.TryLookup(kLanguageSetting), hstring()));
}

// Sans réglage : français si c'est la première langue du téléphone.
bool EnglishWanted() {
  const std::wstring tag = LanguageSetting();
  if (!tag.empty())
    return tag != L"fr-FR";
  try {
    auto languages =
        Windows::System::UserProfile::GlobalizationPreferences::Languages();
    return languages.Size() == 0 ||
           std::wstring(languages.GetAt(0)).rfind(L"fr", 0) != 0;
  } catch (...) {
    return false;
  }
}

wchar_t const* DefaultHomeUrl(bool english = g_english) {
  return english ? L"https://en.m.wikipedia.org/"
                 : L"https://fr.m.wikipedia.org/";
}

void Note(wchar_t const* text, unsigned value) {
  wchar_t line[128];
  swprintf_s(line, L"%s 0x%08X", text, value);
  RheniumEngineNote(line);
}

SolidColorBrush Brush(Color color) {
  return SolidColorBrush(color);
}

SolidColorBrush Brush(Ink ink) {
  return g_shell->inks[ink];
}

Color InkColor(Ink ink) {
  return (g_shell->dark ? kDarkInks : kLightInks)[ink];
}

FontIcon Glyph(wchar_t const* glyph) {
  FontIcon icon;
  icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
  icon.FontSize(18);
  icon.Glyph(glyph);
  return icon;
}

TextBlock Label(wchar_t const* text, double size, Ink ink = kTextColor) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  block.Foreground(Brush(ink));
  block.TextWrapping(TextWrapping::Wrap);
  return block;
}

Button FlatButton() {
  Button button;
  button.Padding(Thickness{0, 0, 0, 0});
  button.Background(Brush(Windows::UI::Colors::Transparent()));
  button.BorderThickness(Thickness{0, 0, 0, 0});
  return button;
}

std::wstring HomeUrl() {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  const hstring url =
      unbox_value_or<hstring>(values.TryLookup(kHomeUrlSetting), hstring());
  return url.empty() ? DefaultHomeUrl() : std::wstring(url);
}

int SearchEngineIndex() {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  const hstring name = unbox_value_or<hstring>(
      values.TryLookup(kSearchEngineSetting), hstring());
  for (int i = 0; i < static_cast<int>(std::size(kSearchEngines)); ++i) {
    if (name == kSearchEngines[i].name)
      return i;
  }
  return 0;
}

void SetSearchEngine(int index) {
  if (index < 0 || index >= static_cast<int>(std::size(kSearchEngines)))
    return;
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  if (index == 0) {
    if (values.HasKey(kSearchEngineSetting))
      values.Remove(kSearchEngineSetting);
  } else {
    values.Insert(kSearchEngineSetting, box_value(kSearchEngines[index].name));
  }
  RheniumEngineSetSearchUrl(kSearchEngines[index].prefix);
}

bool DesktopSitesWanted() {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  return values.HasKey(kDesktopSitesSetting);
}

void SetDesktopSites(bool desktop) {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  if (desktop)
    values.Insert(kDesktopSitesSetting, box_value(true));
  else if (values.HasKey(kDesktopSitesSetting))
    values.Remove(kDesktopSitesSetting);
  RheniumEngineSetDesktopSites(desktop ? 1 : 0);
}

// Hors saisie, la barre ne montre que le domaine, comme Edge.
std::wstring DisplayText(std::wstring const& url) {
  try {
    Windows::Foundation::Uri uri(url);
    std::wstring host(uri.Host());
    if (host.empty())
      return url;
    if (host.rfind(L"www.", 0) == 0)
      host.erase(0, 4);
    return host;
  } catch (...) {
    return url;
  }
}

// Couleur d'accentuation du téléphone, comme Edge.
Color AccentColor() {
  try {
    return Windows::UI::ViewManagement::UISettings().GetColorValue(
        Windows::UI::ViewManagement::UIColorType::Accent);
  } catch (...) {
    return Color{0xFF, 0x00, 0x78, 0xD7};
  }
}

long long Now() {
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  return (static_cast<long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
}

std::wstring LocalPath(wchar_t const* name) {
  return std::wstring(
             Windows::Storage::ApplicationData::Current().LocalFolder().Path()) +
         L"\\" + name;
}

// Fichiers de LocalState : une ligne par élément, champs séparés par des
// tabulations, en UTF-8.
std::vector<std::vector<std::wstring>> ReadLines(wchar_t const* name,
                                                 size_t fields) {
  std::vector<std::vector<std::wstring>> lines;
  std::ifstream file(LocalPath(name), std::ios::binary);
  std::string line;
  while (std::getline(file, line)) {
    std::vector<std::wstring> values;
    size_t begin = 0;
    for (size_t field = 0; field < fields; ++field) {
      const size_t tab =
          field + 1 < fields ? line.find('\t', begin) : line.size();
      if (tab == std::string::npos)
        break;
      values.emplace_back(to_hstring(line.substr(begin, tab - begin)));
      begin = tab + 1;
    }
    if (values.size() == fields)
      lines.push_back(std::move(values));
  }
  return lines;
}

std::mutex g_writing;  // une écriture à la fois

// Dans un fichier à côté, puis renommé : un arrêt brutal laisse l'ancien.
bool WriteLocalFile(std::wstring const& path, std::string const& data) {
  const std::wstring partial = path + L".partial";
  {
    std::ofstream file(partial, std::ios::binary | std::ios::trunc);
    file.write(data.data(), data.size());
    if (!file)
      return false;
  }
  return MoveFileExW(partial.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING) != 0;
}

// Historique : heure, adresse, titre.
void LoadHistory() {
  for (auto const& line : ReadLines(kHistoryFile, 3)) {
    g_shell->visits.push_back(
        {std::wcstoll(line[0].c_str(), nullptr, 10), line[1], line[2]});
  }
}

std::string HistoryText() {
  std::string data;
  for (HistoryEntry const& entry : g_shell->visits) {
    data += std::to_string(entry.time) + '\t' + to_string(entry.url) + '\t' +
            to_string(entry.title) + '\n';
  }
  return data;
}

// Hors du thread UI ; pour chaque fichier, seule la dernière demande compte.
void SaveLater(wchar_t const* name,
               std::string data,
               std::atomic<unsigned>& latest) {
  const unsigned request = ++latest;
  Windows::System::Threading::ThreadPool::RunAsync(
      [data = std::move(data), name, path = LocalPath(name), &latest,
       request](auto&&) {
        std::lock_guard<std::mutex> lock(g_writing);
        if (request != latest || WriteLocalFile(path, data))
          return;
        g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal, [name] {
          RheniumEngineNote(
              (L"coquille : " + std::wstring(name) + L" non écrit").c_str());
        });
      });
}

// Dernière écriture demandée pour l'historique et les onglets.
std::atomic<unsigned> g_history_saves{0};
std::atomic<unsigned> g_tabs_saves{0};

void SaveHistory() {
  SaveLater(kHistoryFile, HistoryText(), g_history_saves);
}

// Titre ou adresse dans un champ de fichier : sans tabulation ni saut.
std::wstring CleanField(std::wstring text) {
  for (wchar_t& c : text) {
    if (c == L'\t' || c == L'\r' || c == L'\n')
      c = L' ';
  }
  return text;
}

// Favoris : adresse, titre.
void LoadFavorites() {
  for (auto const& line : ReadLines(kFavoritesFile, 2))
    g_shell->favorites.push_back({line[0], line[1]});
}

void SaveFavorites() {
  static std::atomic<unsigned> latest{0};
  std::string data;
  for (Favorite const& favorite : g_shell->favorites)
    data += to_string(favorite.url) + '\t' + to_string(favorite.title) + '\n';
  SaveLater(kFavoritesFile, std::move(data), latest);
}

std::vector<Favorite>::iterator FindFavorite(std::wstring const& url) {
  std::vector<Favorite>& favorites = g_shell->favorites;
  return std::find_if(favorites.begin(), favorites.end(),
                      [&](Favorite const& favorite) { return favorite.url == url; });
}

bool CanBeFavorite(std::wstring const& url) {
  return url.rfind(L"https://", 0) == 0 || url.rfind(L"http://", 0) == 0;
}

// Onglets : actif (1 ou 0), adresse, titre.
void AddSavedTab(void* data,
                 int,
                 int active,
                 wchar_t const* title,
                 wchar_t const* url) noexcept {
  try {
    if (*url) {
      *static_cast<std::string*>(data) += (active ? "1\t" : "0\t") +
                                          to_string(CleanField(url)) + '\t' +
                                          to_string(CleanField(title)) + '\n';
    }
  } catch (...) {
  }
}

std::string TabsText() {
  std::string tabs;
  RheniumEngineListTabs(&AddSavedTab, &tabs);
  return tabs;
}

// À chaque page et à chaque changement d'onglet : l'app peut être tuée sans
// passer par l'arrière-plan (mise à jour, plantage). Pas tant que les onglets
// de la session précédente ne sont pas tous rouverts.
void SaveTabs() {
  if (g_shell->restore.empty())
    SaveLater(kTabsFile, TabsText(), g_tabs_saves);
}

// Avant que le système ne suspende puis, souvent, ne tue l'app : historique
// et onglets, tout de suite. Les écritures en attente, plus anciennes, sont
// abandonnées.
void SaveSession() {
  const bool tabs_ready = g_shell->restore.empty();
  const std::string tabs = tabs_ready ? TabsText() : std::string();
  ++g_history_saves;
  if (tabs_ready)
    ++g_tabs_saves;
  std::lock_guard<std::mutex> lock(g_writing);
  if (!WriteLocalFile(LocalPath(kHistoryFile), HistoryText()) ||
      (tabs_ready && !WriteLocalFile(LocalPath(kTabsFile), tabs))) {
    RheniumEngineNote(L"coquille : session non écrite");
  }
}

// L'onglet actif de la dernière session s'ouvre au lancement, les autres le
// suivent sans se charger (RestoreTabs).
void LoadTabs() {
  ShellState& shell = *g_shell;
  shell.start_url = HomeUrl();
  for (auto const& line : ReadLines(kTabsFile, 3)) {
    if (line[0] == L"1")
      shell.start_url = line[1];
    else
      shell.restore.push_back({line[1], line[2]});
  }
  RheniumEngineNote((L"session : " + std::to_wstring(shell.visits.size()) +
                     L" visites, " + std::to_wstring(shell.restore.size()) +
                     L" onglets en attente")
                        .c_str());
}

void RestoreTabs() {
  const std::vector<SavedTab> tabs = std::move(g_shell->restore);
  g_shell->restore.clear();
  for (SavedTab const& tab : tabs)
    RheniumEngineRestoreTab(tab.url.c_str(), tab.title.c_str());
}

void ShowAddress() {
  ShellState& shell = *g_shell;
  shell.address.Text(shell.address.FocusState() == FocusState::Unfocused
                         ? DisplayText(shell.url)
                         : shell.url);
}

// Rend le clavier à la page : la barre d'adresse perd le focus XAML.
void FocusWeb() {
  // Pas sur un bouton : Entrée tapé dans la page le déclencherait.
  g_shell->focus_sink.Focus(FocusState::Programmatic);
  RheniumEngineSetWebFocus(1);
}

bool MenuOpen() {
  return g_shell->menu.Visibility() == Visibility::Visible;
}

bool SettingsOpen() {
  return g_shell->settings.Visibility() == Visibility::Visible;
}

void CloseMenu() {
  g_shell->menu.Visibility(Visibility::Collapsed);
  g_shell->dismiss_layer.Visibility(Visibility::Collapsed);
}

// Le menu monte depuis la barre, comme celui d'Edge.
void OpenMenu() {
  ShellState& shell = *g_shell;
  shell.forward.IsEnabled(shell.can_go_forward);
  shell.reload_icon.Glyph(shell.loading ? kGlyphCancel : kGlyphReload);
  shell.reload_label.Text(shell.loading ? Tr(L"Arrêter", L"Stop")
                                       : Tr(L"Actualiser", L"Refresh"));
  shell.favorite_icon.Glyph(FindFavorite(shell.url) != shell.favorites.end()
                                ? kGlyphFavoriteFill
                                : kGlyphFavorite);
  shell.favorite_icon.Foreground(
      Brush(CanBeFavorite(shell.url) ? kTextColor : kDimTextColor));
  char id[33];
  shell.add_extension.Visibility(
      !shell.installing && RheniumEngineWebStoreId(shell.url.c_str(), id)
          ? Visibility::Visible
          : Visibility::Collapsed);
  shell.desktop_check.Visibility(RheniumEngineIsTabDesktop()
                                     ? Visibility::Visible
                                     : Visibility::Collapsed);
  shell.dismiss_layer.Visibility(Visibility::Visible);
  shell.menu.Visibility(Visibility::Visible);

  Animation::DoubleAnimation slide;
  slide.From(kBarHeight);
  slide.To(0.0);
  slide.Duration(DurationHelper::FromTimeSpan(
      std::chrono::milliseconds(150)));
  Animation::CubicEase ease;
  ease.EasingMode(Animation::EasingMode::EaseOut);
  slide.EasingFunction(ease);
  Animation::Storyboard storyboard;
  Animation::Storyboard::SetTarget(slide, shell.menu.RenderTransform());
  Animation::Storyboard::SetTargetProperty(slide, L"Y");
  storyboard.Children().Append(slide);
  storyboard.Begin();
}

// La page et la barre tiennent dans la zone visible ; autour, la couleur de la
// barre passe sous la barre d'état et les boutons du téléphone.
void FitVisibleBounds() {
  auto bounds = Window::Current().Bounds();
  auto visible = ApplicationView::GetForCurrentView().VisibleBounds();
  g_shell->root.Padding(Thickness{
      visible.X - bounds.X, visible.Y - bounds.Y,
      (bounds.X + bounds.Width) - (visible.X + visible.Width),
      (bounds.Y + bounds.Height) - (visible.Y + visible.Height)});
}

void StyleStatusBar() {
  if (!Windows::Foundation::Metadata::ApiInformation::IsTypePresent(
          L"Windows.UI.ViewManagement.StatusBar")) {
    return;
  }
  auto status_bar = Windows::UI::ViewManagement::StatusBar::GetForCurrentView();
  status_bar.BackgroundColor(InkColor(kBarColor));
  status_bar.BackgroundOpacity(1);
  status_bar.ForegroundColor(InkColor(kTextColor));
}

std::wstring ThemeSetting() {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  return std::wstring(
      unbox_value_or<hstring>(values.TryLookup(kThemeSetting), hstring()));
}

bool ForceDarkWanted() {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  return values.HasKey(kForceDarkSetting);
}

bool PhoneIsDark() {
  try {
    return Windows::UI::ViewManagement::UISettings()
               .GetColorValue(
                   Windows::UI::ViewManagement::UIColorType::Background)
               .R < 0x80;
  } catch (...) {
    return true;
  }
}

ElementTheme ShellTheme() {
  return g_shell->dark ? ElementTheme::Dark : ElementTheme::Light;
}

// Coquille et pages au thème réglé, ou à celui du téléphone. Les menus
// surgissants et les boîtes de dialogue prennent le leur à leur ouverture.
void ApplyTheme() {
  ShellState& shell = *g_shell;
  const std::wstring theme = ThemeSetting();
  shell.dark = theme.empty() ? PhoneIsDark() : theme == L"dark";
  for (int i = 0; i < kInkCount; ++i) {
    const Color color = InkColor(static_cast<Ink>(i));
    if (static_cast<int>(shell.inks.size()) <= i)
      shell.inks.push_back(SolidColorBrush(color));
    else
      shell.inks[i].Color(color);
  }
  if (shell.root)
    shell.root.RequestedTheme(ShellTheme());
  try {
    StyleStatusBar();
  } catch (...) {
  }
  RheniumEngineSetDarkMode(shell.dark ? 1 : 0, ForceDarkWanted() ? 1 : 0);
}

void FollowPhoneTheme() {
  if (ThemeSetting().empty() && PhoneIsDark() != g_shell->dark)
    ApplyTheme();
}

// Rappels du moteur, sur le thread UI sauf attach_swap_chain. Aucune
// exception ne doit en sortir.

void OnAttachSwapChain(void*, IUnknown* swap_chain) noexcept {
  try {
    com_ptr<IUnknown> chain;
    chain.copy_from(swap_chain);
    g_shell->dispatcher.RunAsync(CoreDispatcherPriority::High, [chain] {
      HRESULT hr = E_FAIL;
      try {
        auto native = g_shell->panel.as<ISwapChainPanelNative>();
        hr = native->SetSwapChain(chain.as<IDXGISwapChain>().get());
      } catch (hresult_error const& e) {
        hr = e.code();
      } catch (...) {
      }
      if (FAILED(hr))
        Note(L"coquille : SetSwapChain", hr);
    });
  } catch (...) {
  }
}

void OnUrlChanged(void*, wchar_t const* url) noexcept {
  try {
    g_shell->url = url;
    if (g_shell->address.FocusState() == FocusState::Unfocused)
      ShowAddress();
  } catch (...) {
  }
}

void OnLoadingChanged(void*,
                      int loading,
                      int /*can_go_back*/,
                      int can_go_forward) noexcept {
  try {
    ShellState& shell = *g_shell;
    if (loading && !shell.loading)
      shell.progress.Value(0.05);
    shell.loading = loading != 0;
    shell.can_go_forward = can_go_forward != 0;
    shell.progress.Visibility(shell.loading ? Visibility::Visible
                                            : Visibility::Collapsed);
  } catch (...) {
  }
}

void OnProgressChanged(void*, double progress) noexcept {
  try {
    if (progress > g_shell->progress.Value())
      g_shell->progress.Value(progress);
  } catch (...) {
  }
}

void OnTitleChanged(void*, wchar_t const* title) noexcept {
  try {
    g_shell->title = title;
  } catch (...) {
  }
}

bool TabsOpen();
void RebuildTabs();
void OnExtensionInstalled(void*, int ok, wchar_t const* text) noexcept;
void OnDownloadChanged(void*,
                       int id,
                       int state,
                       long long received,
                       long long total,
                       wchar_t const* name,
                       wchar_t const* path) noexcept;

void CollectTabId(void* ids, int id, int, wchar_t const*, wchar_t const*) noexcept {
  try {
    static_cast<std::vector<int>*>(ids)->push_back(id);
  } catch (...) {
  }
}

void OnTabsChanged(void*, int count) noexcept {
  try {
    ShellState& shell = *g_shell;
    shell.tab_count_value = count;
    for (TextBlock const& text : shell.tab_counts)
      text.Text(std::to_wstring(count));
    // Oublie les aperçus des onglets fermés.
    std::vector<int> ids;
    RheniumEngineListTabs(&CollectTabId, &ids);
    for (auto it = shell.thumbnails.begin(); it != shell.thumbnails.end();) {
      if (std::find(ids.begin(), ids.end(), it->first) == ids.end())
        it = shell.thumbnails.erase(it);
      else
        ++it;
    }
    if (TabsOpen())
      RebuildTabs();
    SaveTabs();
    if (!shell.restore.empty()) {
      shell.dispatcher.RunAsync(CoreDispatcherPriority::Low,
                                [] { RestoreTabs(); });
    }
  } catch (...) {
  }
}

void OnTabThumbnail(void*,
                    int id,
                    unsigned char const* bgra,
                    int width,
                    int height,
                    int stride) noexcept {
  try {
    Imaging::WriteableBitmap bitmap(width, height);
    uint8_t* pixels = bitmap.PixelBuffer().data();
    for (int y = 0; y < height; ++y) {
      uint8_t* row = pixels + y * width * 4;
      memcpy(row, bgra + y * stride, width * 4);
      for (int x = 3; x < width * 4; x += 4)
        row[x] = 0xFF;  // opaque
    }
    bitmap.Invalidate();
    g_shell->thumbnails.insert_or_assign(id, bitmap);
    if (auto it = g_shell->card_images.find(id); it != g_shell->card_images.end())
      it->second.Source(bitmap);
  } catch (...) {
  }
}

// Une visite par page ; le titre, arrivé ensuite, complète la dernière.
void OnPageVisited(void*, wchar_t const* url, wchar_t const* title) noexcept {
  try {
    std::vector<HistoryEntry>& visits = g_shell->visits;
    std::wstring clean = CleanField(title);
    if (!visits.empty() && visits.back().url == url) {
      if (clean.empty() || clean == visits.back().title)
        return;
      visits.back().title = std::move(clean);
    } else {
      visits.push_back({Now(), url, std::move(clean)});
      if (visits.size() > kMaxHistory)
        visits.erase(visits.begin(), visits.end() - kMaxHistory);
    }
    SaveHistory();
    SaveTabs();
  } catch (...) {
  }
}

void NoteMemory(wchar_t const* text, unsigned long long limit) {
  wchar_t line[128];
  swprintf_s(line, L"coquille : %s, %llu Mo sur %llu Mo", text,
             Windows::System::MemoryManager::AppMemoryUsage() >> 20,
             limit >> 20);
  RheniumEngineNote(line);
}

// Mémoire de l'app près de sa limite : l'onglet inactif utilisé le moins
// récemment est déchargé, puis un autre toutes les 2 s tant qu'elle y reste.
void RelieveMemory(unsigned long long limit) {
  using Windows::System::AppMemoryUsageLevel;
  using Windows::System::MemoryManager;
  ShellState& shell = *g_shell;
  shell.memory_timer.Stop();
  if (MemoryManager::AppMemoryUsageLevel() < AppMemoryUsageLevel::High &&
      MemoryManager::AppMemoryUsage() < limit / 10 * 9) {
    return;
  }
  NoteMemory(L"mémoire critique", limit);
  if (RheniumEngineDiscardTab())
    shell.memory_timer.Start();
}

// Les événements arrivent sur un autre thread.
void WatchMemory() {
  using Windows::System::MemoryManager;
  ShellState& shell = *g_shell;
  shell.memory_timer = DispatcherTimer();
  shell.memory_timer.Interval(std::chrono::seconds(2));
  shell.memory_timer.Tick([](auto&&, auto&&) {
    RelieveMemory(Windows::System::MemoryManager::AppMemoryUsageLimit());
  });
  MemoryManager::AppMemoryUsageIncreased([](auto&&, auto&&) {
    g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal, [] {
      RelieveMemory(Windows::System::MemoryManager::AppMemoryUsageLimit());
    });
  });
  // Limite abaissée, à la mise en arrière-plan par exemple.
  MemoryManager::AppMemoryUsageLimitChanging(
      [](auto&&,
         Windows::System::AppMemoryUsageLimitChangingEventArgs const& args) {
        const unsigned long long limit = args.NewLimit();
        g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal,
                                     [limit] { RelieveMemory(limit); });
      });
  NoteMemory(L"mémoire", MemoryManager::AppMemoryUsageLimit());
}

// Lance le moteur une fois la taille du panneau connue. RheniumEngineRun ne
// rend la main qu'à la fermeture : la boucle de Chromium fait tourner le
// CoreDispatcher, donc XAML continue de vivre.
void StartEngine() {
  ShellState& shell = *g_shell;
  shell.engine_started = true;
  LoadTabs();
  RheniumEngineSetSearchUrl(kSearchEngines[SearchEngineIndex()].prefix);
  RheniumEngineSetDesktopSites(DesktopSitesWanted() ? 1 : 0);
  shell.host.context = &shell;
  shell.host.scale = shell.panel.CompositionScaleX();
  shell.host.start_url = shell.start_url.c_str();
  shell.host.language =
      shell.engine_language.empty() ? nullptr : shell.engine_language.c_str();
  shell.host.attach_swap_chain = &OnAttachSwapChain;
  shell.host.url_changed = &OnUrlChanged;
  shell.host.loading_changed = &OnLoadingChanged;
  shell.host.title_changed = &OnTitleChanged;
  shell.host.progress_changed = &OnProgressChanged;
  shell.host.tabs_changed = &OnTabsChanged;
  shell.host.page_visited = &OnPageVisited;
  shell.host.tab_thumbnail = &OnTabThumbnail;
  shell.host.extension_installed = &OnExtensionInstalled;
  shell.host.download_changed = &OnDownloadChanged;
  WatchMemory();
  shell.url = shell.start_url;
  ShowAddress();
  shell.dispatcher.RunAsync(CoreDispatcherPriority::Low,
                            [] { RheniumEngineRun(&g_shell->host); });
}

// Clavier sur un champ de la page : la mise en page ne bouge pas, la page
// fait défiler le champ au-dessus du clavier. Sur la barre d'adresse, XAML
// remonte lui-même l'interface.
void OnKeyboardShowing(InputPaneVisibilityEventArgs const& args) {
  ShellState& shell = *g_shell;
  if (!shell.engine_started ||
      shell.address.FocusState() != FocusState::Unfocused) {
    return;
  }
  args.EnsuredFocusedElementInView(true);
  const auto top = shell.panel.TransformToVisual(nullptr).TransformPoint({0, 0});
  const double bottom = top.Y + shell.panel.ActualHeight();
  const double covered = bottom - args.OccludedRect().Y;
  RheniumEngineSetKeyboardInset(static_cast<float>(covered > 0 ? covered : 0));
}

void ForwardPointer(int kind, PointerRoutedEventArgs const& args) {
  auto point = args.GetCurrentPoint(g_shell->input_surface);
  auto position = point.Position();
  const bool mouse = args.Pointer().PointerDeviceType() ==
                     Windows::Devices::Input::PointerDeviceType::Mouse;
  RheniumEnginePointer(kind, point.PointerId(), position.X, position.Y,
                       mouse ? 1 : 0);
  args.Handled(true);
}

// Champ sombre, clair pendant la saisie, sans bordure (ressources du modèle
// de TextBox redéfinies localement).
void StyleAddress(TextBox const& address) {
  auto resources = address.Resources();
  auto set = [&](wchar_t const* key, Ink ink) {
    resources.Insert(box_value(key), Brush(ink));
  };
  set(L"TextControlBackground", kFieldColor);
  set(L"TextControlBackgroundPointerOver", kFieldColor);
  set(L"TextControlBackgroundFocused", kFieldFocusedColor);
  set(L"TextControlForeground", kTextColor);
  set(L"TextControlForegroundPointerOver", kTextColor);
  set(L"TextControlForegroundFocused", kFieldFocusedTextColor);
  set(L"TextControlPlaceholderForeground", kDimTextColor);
  set(L"TextControlPlaceholderForegroundPointerOver", kDimTextColor);
  set(L"TextControlPlaceholderForegroundFocused", kDimTextColor);
  set(L"TextControlBorderBrush", kFieldColor);
  set(L"TextControlBorderBrushPointerOver", kFieldColor);
  set(L"TextControlBorderBrushFocused", kFieldFocusedColor);
  address.BorderThickness(Thickness{0, 0, 0, 0});
  address.FontSize(15);
  address.Padding(Thickness{10, 6, 6, 5});
}

void SetUrlInputScope(TextBox const& box) {
  InputScope scope;
  InputScopeName url_scope;
  url_scope.NameValue(InputScopeNameValue::Url);
  scope.Names().Append(url_scope);
  box.InputScope(scope);
  box.IsSpellCheckEnabled(false);
  box.IsTextPredictionEnabled(false);
}

// Commande du menu : le menu se ferme, puis l'action.
template <typename Action>
Button MenuButton(UIElement const& content, Action action) {
  Button button;
  button.Content(content);
  button.Background(Brush(Windows::UI::Colors::Transparent()));
  button.BorderThickness(Thickness{0, 0, 0, 0});
  button.HorizontalAlignment(HorizontalAlignment::Stretch);
  button.Click([action](auto&&, auto&&) {
    CloseMenu();
    action();
  });
  return button;
}

// Icône et libellé, en haut du menu.
template <typename Action>
Button MenuIcon(FontIcon const& icon, TextBlock const& label, Action action) {
  StackPanel content;
  icon.HorizontalAlignment(HorizontalAlignment::Center);
  label.HorizontalAlignment(HorizontalAlignment::Center);
  label.Margin(Thickness{0, 6, 0, 0});
  content.Children().Append(icon);
  content.Children().Append(label);
  Button button = MenuButton(content, action);
  button.Padding(Thickness{0, 12, 0, 10});
  return button;
}

template <typename Action>
Button MenuItem(wchar_t const* text, Action action) {
  Button button = MenuButton(Label(text, 15), action);
  button.HorizontalContentAlignment(HorizontalAlignment::Left);
  button.Height(kBarHeight);
  button.Padding(Thickness{16, 0, 16, 0});
  return button;
}

void OpenSettings();
void OpenHistory();
void OpenFavorites();
void OpenDownloads();
void ToggleFavorite();
void NewTab();
void RebuildExtensions();
void SetLanguage(int index);
void SetTheme(int index);
void SetForceDark(bool force_dark);

// Boîte de message ; une seule à la fois (XAML refuse d'en ouvrir deux).
void ShowMessage(std::wstring const& title, std::wstring const& text) {
  try {
    ContentDialog dialog;
    dialog.RequestedTheme(ShellTheme());
    dialog.Title(box_value(title));
    dialog.Content(box_value(text));
    dialog.CloseButtonText(L"OK");
    dialog.ShowAsync();
  } catch (...) {
  }
}

// Extension de la page du Chrome Web Store ouverte dans l'onglet actif.
void InstallFromPage() {
  ShellState& shell = *g_shell;
  char id[33];
  if (shell.installing || !RheniumEngineWebStoreId(shell.url.c_str(), id))
    return;
  shell.installing = RheniumEngineInstallExtension(id) != 0;
}

void OnExtensionInstalled(void*, int ok, wchar_t const* text) noexcept {
  try {
    g_shell->installing = false;
    if (ok) {
      ShowMessage(Tr(L"Extension ajoutée", L"Extension added"),
                  Tr(L"« ", L"“") + std::wstring(text) +
                      Tr(L" » est installée. Elle se règle dans Paramètres, "
                         L"Extensions.",
                         L"” is installed. You can manage it in Settings, "
                         L"Extensions."));
    } else {
      ShowMessage(Tr(L"Extension non ajoutée", L"Extension not added"), text);
    }
    if (SettingsOpen())
      RebuildExtensions();
  } catch (...) {
  }
}

Border BuildMenu() {
  ShellState& shell = *g_shell;
  shell.forward = MenuIcon(Glyph(kGlyphForward),
                           Label(Tr(L"Suivant", L"Forward"), 11),
                           [] { RheniumEngineGoForward(); });
  shell.reload_icon = Glyph(kGlyphReload);
  shell.reload_label = Label(Tr(L"Actualiser", L"Refresh"), 11);
  shell.favorite_icon = Glyph(kGlyphFavorite);
  Grid icons;
  int column = 0;
  for (Button const& button :
       {shell.forward,
        MenuIcon(shell.reload_icon, shell.reload_label,
                 [] {
                   if (g_shell->loading)
                     RheniumEngineStop();
                   else
                     RheniumEngineReload();
                 }),
        MenuIcon(shell.favorite_icon, Label(Tr(L"Favori", L"Favorite"), 11),
                 ToggleFavorite),
        MenuIcon(Glyph(kGlyphShare), Label(Tr(L"Partager", L"Share"), 11),
                 [] { DataTransferManager::ShowShareUI(); }),
        MenuIcon(Glyph(kGlyphShield), Label(L"uBlock", 11), [] {
          RheniumEngineOpenExtensionPopup();
        })}) {
    icons.ColumnDefinitions().Append(ColumnDefinition());
    Grid::SetColumn(button, column++);
    icons.Children().Append(button);
  }

  StackPanel items;
  items.Children().Append(icons);
  shell.add_extension =
      MenuItem(Tr(L"Ajouter à Rhénium", L"Add to Rhénium"), InstallFromPage);
  shell.add_extension.Visibility(Visibility::Collapsed);
  items.Children().Append(shell.add_extension);
  items.Children().Append(MenuItem(Tr(L"Nouvel onglet", L"New tab"), NewTab));
  items.Children().Append(MenuItem(Tr(L"Favoris", L"Favorites"), OpenFavorites));
  items.Children().Append(MenuItem(Tr(L"Historique", L"History"), OpenHistory));
  items.Children().Append(
      MenuItem(Tr(L"Téléchargements", L"Downloads"), OpenDownloads));
  items.Children().Append(MenuItem(Tr(L"Accueil", L"Home"), [] {
    RheniumEngineNavigate(HomeUrl().c_str());
  }));
  wchar_t const* desktop_text =
      Tr(L"Version pour ordinateur", L"Desktop site");
  Button desktop = MenuItem(desktop_text, [] {
    RheniumEngineSetTabDesktop(!RheniumEngineIsTabDesktop());
  });
  shell.desktop_check = Glyph(kGlyphCheck);
  shell.desktop_check.HorizontalAlignment(HorizontalAlignment::Right);
  Grid desktop_row;
  desktop_row.Children().Append(Label(desktop_text, 15));
  desktop_row.Children().Append(shell.desktop_check);
  desktop.HorizontalContentAlignment(HorizontalAlignment::Stretch);
  desktop.Content(desktop_row);
  items.Children().Append(desktop);
  items.Children().Append(
      MenuItem(Tr(L"Paramètres", L"Settings"), OpenSettings));

  Border menu;
  menu.Background(Brush(kMenuColor));
  menu.Child(items);
  menu.VerticalAlignment(VerticalAlignment::Bottom);
  menu.Visibility(Visibility::Collapsed);
  menu.RenderTransform(TranslateTransform());
  return menu;
}

void OpenSettings() {
  g_shell->home_url.Text(HomeUrl());
  RebuildExtensions();
  g_shell->settings.Visibility(Visibility::Visible);
}

void CloseSettings() {
  const hstring text = g_shell->home_url.Text();
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  if (text.empty() || text == DefaultHomeUrl()) {
    if (values.HasKey(kHomeUrlSetting))
      values.Remove(kHomeUrlSetting);
  }
  else {
    values.Insert(kHomeUrlSetting, box_value(text));
  }
  g_shell->settings.Visibility(Visibility::Collapsed);
}

TextBlock PanelHeader(wchar_t const* text) {
  TextBlock header = Label(text, 15);
  header.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
  return header;
}

TextBlock SectionTitle(wchar_t const* text) {
  TextBlock title = Label(text, 20);
  title.Margin(Thickness{0, 24, 0, 8});
  return title;
}

// Lien qui ferme les paramètres puis agit.
template <typename Action>
HyperlinkButton Link(wchar_t const* text, Action action) {
  HyperlinkButton link;
  link.Content(box_value(text));
  link.Padding(Thickness{0, 4, 0, 4});
  link.Click([action](auto&&, auto&&) {
    CloseSettings();
    action();
  });
  return link;
}

struct ExtensionEntry {
  std::string id;
  std::wstring name;
  std::wstring version;
  bool enabled;
  bool removable;  // installée depuis le Chrome Web Store
  bool has_popup;
  bool has_options;
};

void CollectExtension(void* entries,
                      char const* id,
                      wchar_t const* name,
                      wchar_t const* version,
                      int enabled,
                      int removable,
                      int has_popup,
                      int has_options) noexcept {
  try {
    static_cast<std::vector<ExtensionEntry>*>(entries)->push_back(
        {id, name, version, enabled != 0, removable != 0, has_popup != 0,
         has_options != 0});
  } catch (...) {
  }
}

void ConfirmUninstall(ExtensionEntry const& entry) {
  try {
    ContentDialog dialog;
    dialog.RequestedTheme(ShellTheme());
    dialog.Title(box_value(Tr(L"Supprimer l'extension", L"Remove extension")));
    dialog.Content(box_value(Tr(L"Supprimer « ", L"Remove “") + entry.name +
                             Tr(L" » ?", L"”?")));
    dialog.PrimaryButtonText(Tr(L"Supprimer", L"Remove"));
    dialog.CloseButtonText(Tr(L"Annuler", L"Cancel"));
    dialog.PrimaryButtonClick([id = entry.id](auto&&, auto&&) {
      RheniumEngineUninstallExtension(id.c_str());
      RebuildExtensions();
    });
    dialog.ShowAsync();
  } catch (...) {
  }
}

void OnBrowsingDataCleared(void*) noexcept {
  ShowMessage(Tr(L"Données effacées", L"Data cleared"),
              Tr(L"Les cookies, le cache et les données des sites sont "
                 L"effacés.",
                 L"Cookies, cache and site data have been cleared."));
}

void ConfirmClearData() {
  try {
    ContentDialog dialog;
    dialog.RequestedTheme(ShellTheme());
    dialog.Title(box_value(Tr(L"Effacer les données", L"Clear data")));
    dialog.Content(box_value(
        Tr(L"Effacer les cookies, le cache et les données des sites ? Vous "
           L"serez déconnecté des sites.",
           L"Clear cookies, cache and site data? You will be signed out of "
           L"websites.")));
    dialog.PrimaryButtonText(Tr(L"Effacer", L"Clear"));
    dialog.CloseButtonText(Tr(L"Annuler", L"Cancel"));
    dialog.PrimaryButtonClick([](auto&&, auto&&) {
      RheniumEngineClearBrowsingData(OnBrowsingDataCleared, nullptr);
    });
    dialog.ShowAsync();
  } catch (...) {
  }
}

// Une ligne par extension : nom, version, interrupteur, puis ses pages et,
// pour celles du Web Store, la suppression.
void RebuildExtensions() {
  ShellState& shell = *g_shell;
  std::vector<ExtensionEntry> entries;
  RheniumEngineListExtensions(&CollectExtension, &entries);
  shell.extension_list.Children().Clear();
  for (ExtensionEntry const& entry : entries) {
    Grid row;
    row.Margin(Thickness{0, 8, 0, 8});
    ColumnDefinition text_column;
    text_column.Width(GridLength{1, GridUnitType::Star});
    row.ColumnDefinitions().Append(text_column);
    ColumnDefinition toggle_column;
    toggle_column.Width(GridLength{1, GridUnitType::Auto});
    row.ColumnDefinitions().Append(toggle_column);

    StackPanel text;
    text.Children().Append(Label(entry.name.c_str(), 15));
    text.Children().Append(
        Label((L"Version " + entry.version).c_str(), 13, kDimTextColor));
    StackPanel links;
    links.Orientation(Orientation::Horizontal);
    auto add_link = [&](HyperlinkButton const& link) {
      link.Margin(Thickness{0, 0, 20, 0});
      links.Children().Append(link);
    };
    const std::string id = entry.id;
    if (entry.enabled && entry.has_popup) {
      add_link(Link(Tr(L"Ouvrir", L"Open"),
                    [id] { RheniumEngineOpenExtensionPopupFor(id.c_str()); }));
    }
    if (entry.enabled && entry.has_options) {
      add_link(Link(L"Options",
                    [id] { RheniumEngineOpenExtensionOptionsFor(id.c_str()); }));
    }
    if (entry.removable) {
      HyperlinkButton remove;
      remove.Content(box_value(Tr(L"Supprimer", L"Remove")));
      remove.Padding(Thickness{0, 4, 0, 4});
      remove.Click([entry](auto&&, auto&&) { ConfirmUninstall(entry); });
      add_link(remove);
    }
    text.Children().Append(links);
    row.Children().Append(text);

    ToggleSwitch toggle;
    toggle.IsOn(entry.enabled);
    toggle.OnContent(box_value(L""));
    toggle.OffContent(box_value(L""));
    toggle.MinWidth(0);
    toggle.VerticalAlignment(VerticalAlignment::Top);
    toggle.Toggled(
        [id](Windows::Foundation::IInspectable const& sender, auto&&) {
          RheniumEngineSetExtensionEnabled(id.c_str(),
                                           sender.as<ToggleSwitch>().IsOn());
          // Hors du gestionnaire : la ligne et son interrupteur sont recréés.
          g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal,
                                       [] { RebuildExtensions(); });
        });
    Grid::SetColumn(toggle, 1);
    row.Children().Append(toggle);
    shell.extension_list.Children().Append(row);
  }
}

// Paramètres, en plein écran par-dessus la page, fermés par le bouton retour.
Grid BuildSettings() {
  ShellState& shell = *g_shell;
  StackPanel content;
  content.Padding(Thickness{20, 12, 20, 24});
  auto add = [&](UIElement const& child) { content.Children().Append(child); };

  add(PanelHeader(Tr(L"PARAMÈTRES", L"SETTINGS")));

  add(SectionTitle(Tr(L"Page d'accueil", L"Home page")));
  shell.home_url = TextBox();
  SetUrlInputScope(shell.home_url);
  shell.home_url.PlaceholderText(DefaultHomeUrl());
  add(shell.home_url);

  add(SectionTitle(Tr(L"Moteur de recherche", L"Search engine")));
  ComboBox engines;
  engines.HorizontalAlignment(HorizontalAlignment::Stretch);
  for (SearchEngine const& engine : kSearchEngines)
    engines.Items().Append(box_value(engine.name));
  engines.SelectedIndex(SearchEngineIndex());
  engines.SelectionChanged([engines](auto&&, auto&&) {
    SetSearchEngine(engines.SelectedIndex());
  });
  add(engines);

  add(SectionTitle(Tr(L"Version des sites", L"Site version")));
  ComboBox versions;
  versions.HorizontalAlignment(HorizontalAlignment::Stretch);
  versions.Items().Append(box_value(Tr(L"Mobile", L"Mobile")));
  versions.Items().Append(box_value(Tr(L"Ordinateur", L"Desktop")));
  versions.SelectedIndex(DesktopSitesWanted() ? 1 : 0);
  versions.SelectionChanged([versions](auto&&, auto&&) {
    SetDesktopSites(versions.SelectedIndex() == 1);
  });
  add(versions);
  TextBlock per_tab =
      Label(Tr(L"Un onglet peut changer de version par « … », « Version pour "
               L"ordinateur ».",
               L"A tab can switch with “…”, “Desktop site”."),
            13, kDimTextColor);
  per_tab.Margin(Thickness{0, 8, 0, 0});
  add(per_tab);

  add(SectionTitle(Tr(L"Thème", L"Theme")));
  ComboBox themes;
  themes.HorizontalAlignment(HorizontalAlignment::Stretch);
  for (wchar_t const* name :
       {Tr(L"Comme le téléphone", L"Phone setting"), Tr(L"Clair", L"Light"),
        Tr(L"Sombre", L"Dark")}) {
    themes.Items().Append(box_value(name));
  }
  const std::wstring theme = ThemeSetting();
  themes.SelectedIndex(theme == L"light" ? 1 : theme == L"dark" ? 2 : 0);
  themes.SelectionChanged([themes](auto&&, auto&&) {
    SetTheme(themes.SelectedIndex());
  });
  add(themes);
  ToggleSwitch force_dark;
  force_dark.Header(box_value(
      Tr(L"Assombrir aussi les pages claires", L"Darken light pages too")));
  force_dark.OnContent(box_value(L""));
  force_dark.OffContent(box_value(L""));
  force_dark.IsOn(ForceDarkWanted());
  force_dark.Margin(Thickness{0, 12, 0, 0});
  force_dark.Toggled([](Windows::Foundation::IInspectable const& sender,
                        auto&&) {
    SetForceDark(sender.as<ToggleSwitch>().IsOn());
  });
  add(force_dark);
  add(Label(Tr(L"En thème sombre seulement. Les couleurs de certaines pages "
               L"peuvent en souffrir.",
               L"Dark theme only. Some pages may look off."),
            13, kDimTextColor));

  add(SectionTitle(Tr(L"Langue", L"Language")));
  ComboBox languages;
  languages.HorizontalAlignment(HorizontalAlignment::Stretch);
  for (wchar_t const* name :
       {Tr(L"Langue du téléphone", L"Phone language"), L"Français",
        L"English (US)"}) {
    languages.Items().Append(box_value(name));
  }
  const std::wstring language = LanguageSetting();
  languages.SelectedIndex(language == L"fr-FR"   ? 1
                          : language == L"en-US" ? 2
                                                 : 0);
  languages.SelectionChanged(
      [](Windows::Foundation::IInspectable const& sender, auto&&) {
        SetLanguage(sender.as<ComboBox>().SelectedIndex());
      });
  add(languages);
  if (g_english != shell.engine_english) {
    TextBlock restart = Label(
        Tr(L"Les pages web et uBlock Origin Lite passeront au français au "
           L"prochain lancement.",
           L"Web pages and uBlock Origin Lite will switch to English the "
           L"next time Rhénium starts."),
        13, kDimTextColor);
    restart.Margin(Thickness{0, 8, 0, 0});
    add(restart);
  }

  add(SectionTitle(L"Extensions"));
  add(Label(Tr(L"uBlock Origin Lite, livré avec Rhénium, bloque les "
               L"publicités et les traqueurs.",
               L"uBlock Origin Lite, included with Rhénium, blocks ads and "
               L"trackers."),
            15, kDimTextColor));
  shell.extension_list = StackPanel();
  add(shell.extension_list);
  add(Label(Tr(L"Pour en ajouter une : sa page sur le Chrome Web Store, puis "
               L"« … » et « Ajouter à Rhénium ». Une extension qui demande "
               L"des fonctions absentes de Rhénium peut ne pas marcher.",
               L"To add one: open its page on the Chrome Web Store, then "
               L"“…” and “Add to Rhénium”. An extension that needs features "
               L"Rhénium lacks may not work."),
            15, kDimTextColor));
  add(Link(L"Chrome Web Store", [] { RheniumEngineNavigate(kWebStoreUrl); }));

  add(SectionTitle(Tr(L"Confidentialité", L"Privacy")));
  add(Label(Tr(L"Les cookies et les données des sites sont gardés d'un "
               L"lancement à l'autre.",
               L"Cookies and site data are kept between launches."),
            15, kDimTextColor));
  HyperlinkButton clear_data;
  clear_data.Content(box_value(Tr(L"Effacer les cookies et les données des "
                                  L"sites",
                                  L"Clear cookies and site data")));
  clear_data.Padding(Thickness{0, 4, 0, 4});
  clear_data.Click([](auto&&, auto&&) { ConfirmClearData(); });
  add(clear_data);

  add(SectionTitle(Tr(L"À propos", L"About")));
  const auto version =
      Windows::ApplicationModel::Package::Current().Id().Version();
  wchar_t about[128];
  swprintf_s(about,
             Tr(L"Rhénium %u.%u.%u.%u, moteur Chromium %s",
                L"Rhénium %u.%u.%u.%u, Chromium engine %s"),
             version.Major, version.Minor, version.Build, version.Revision,
             RheniumEngineChromiumVersion());
  add(Label(about, 15, kDimTextColor));
  TextBlock license = Label(
      Tr(L"uBlock Origin Lite, de Raymond Hill et ses contributeurs, est un "
         L"logiciel libre sous licence GPLv3, livré sans modification.",
         L"uBlock Origin Lite, by Raymond Hill and contributors, is free "
         L"software under the GPLv3 license, included unmodified."),
      15, kDimTextColor);
  license.Margin(Thickness{0, 8, 0, 0});
  add(license);
  add(Link(Tr(L"Sources de uBlock Origin Lite",
              L"uBlock Origin Lite source code"),
           [] { RheniumEngineNavigate(kUbolSourcesUrl); }));

  ScrollViewer scroll;
  scroll.Content(content);
  Grid settings;
  settings.Background(Brush(kBarColor));
  settings.Children().Append(scroll);
  settings.Visibility(Visibility::Collapsed);
  return settings;
}

bool IsShown(UIElement const& element) {
  return element.Visibility() == Visibility::Visible;
}

TextBlock Trimmed(TextBlock block) {
  block.TextWrapping(TextWrapping::NoWrap);
  block.TextTrimming(TextTrimming::CharacterEllipsis);
  return block;
}

// Carré de l'icône des onglets d'Edge, avec leur nombre.
Button TabsButton() {
  TextBlock count = Label(std::to_wstring(g_shell->tab_count_value).c_str(), 12);
  count.FontWeight(Windows::UI::Text::FontWeights::SemiBold());
  count.TextWrapping(TextWrapping::NoWrap);
  count.HorizontalAlignment(HorizontalAlignment::Center);
  count.VerticalAlignment(VerticalAlignment::Center);
  g_shell->tab_counts.push_back(count);
  Border box;
  box.Width(22);
  box.Height(22);
  box.BorderThickness(Thickness{1.5, 1.5, 1.5, 1.5});
  box.BorderBrush(Brush(kTextColor));
  box.CornerRadius(CornerRadius{2, 2, 2, 2});
  box.Child(count);
  Button button = FlatButton();
  button.Width(kBarHeight);
  button.Height(kBarHeight);
  button.Content(box);
  return button;
}

// Onglets

bool TabsOpen() {
  return g_shell->tabs && IsShown(g_shell->tabs);
}

void CloseTabs() {
  g_shell->tabs.Visibility(Visibility::Collapsed);
  g_shell->tab_cards.Children().Clear();
  g_shell->card_images.clear();
}

// Aperçu du haut de la page et titre ; × ferme l'onglet. Le dernier onglet
// ne se ferme pas : il revient à la page d'accueil.
void AddTabCard(void* context,
                int id,
                int active,
                wchar_t const* title,
                wchar_t const* url) noexcept {
  try {
    ShellState& shell = *g_shell;
    int& index = *static_cast<int*>(context);
    const double width = (shell.root.ActualWidth() - 12) / 2 - 12;

    Grid preview;
    preview.Height(width);
    preview.Background(Brush(kFieldColor));
    TextBlock site = Label(DisplayText(url).c_str(), 13, kDimTextColor);
    site.HorizontalAlignment(HorizontalAlignment::Center);
    site.VerticalAlignment(VerticalAlignment::Center);
    preview.Children().Append(Trimmed(site));
    Image image;
    image.Stretch(Stretch::UniformToFill);
    image.VerticalAlignment(VerticalAlignment::Top);
    if (auto it = shell.thumbnails.find(id); it != shell.thumbnails.end())
      image.Source(it->second);
    shell.card_images.insert_or_assign(id, image);
    preview.Children().Append(image);

    StackPanel content;
    TextBlock name = Trimmed(Label(*title ? title : url, 13));
    name.Margin(Thickness{8, 0, 36, 0});
    name.Height(36);
    name.Padding(Thickness{0, 9, 0, 0});
    content.Children().Append(name);
    content.Children().Append(preview);

    Button open = FlatButton();
    open.HorizontalAlignment(HorizontalAlignment::Stretch);
    open.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    open.Background(Brush(kMenuColor));
    open.BorderThickness(Thickness{2, 2, 2, 2});
    open.BorderBrush(Brush(active ? AccentColor()
                                  : Windows::UI::Colors::Transparent()));
    open.Content(content);
    open.Click([id](auto&&, auto&&) {
      CloseTabs();
      RheniumEngineActivateTab(id);
    });

    Button close = FlatButton();
    close.Width(36);
    close.Height(36);
    close.Margin(Thickness{0, 2, 2, 0});
    close.HorizontalAlignment(HorizontalAlignment::Right);
    close.VerticalAlignment(VerticalAlignment::Top);
    FontIcon cross = Glyph(kGlyphCancel);
    cross.FontSize(12);
    close.Content(cross);
    close.Click([id](auto&&, auto&&) {
      if (!RheniumEngineCloseTab(id)) {
        CloseTabs();
        RheniumEngineNavigate(HomeUrl().c_str());
      }
    });

    Grid card;
    card.Margin(Thickness{6, 6, 6, 6});
    card.Children().Append(open);
    card.Children().Append(close);
    if (index % 2 == 0) {
      RowDefinition row;
      row.Height(GridLength{1, GridUnitType::Auto});
      shell.tab_cards.RowDefinitions().Append(row);
    }
    Grid::SetRow(card, index / 2);
    Grid::SetColumn(card, index % 2);
    shell.tab_cards.Children().Append(card);
    ++index;
  } catch (...) {
  }
}

void RebuildTabs() {
  ShellState& shell = *g_shell;
  shell.tab_cards.Children().Clear();
  shell.tab_cards.RowDefinitions().Clear();
  shell.card_images.clear();
  int index = 0;
  RheniumEngineListTabs(&AddTabCard, &index);
  shell.new_tab.IsEnabled(shell.tab_count_value < kMaxTabs);
}

void OpenTabs() {
  CloseMenu();
  RheniumEngineCaptureTab();  // l'aperçu arrive ensuite
  RebuildTabs();
  g_shell->tabs.Visibility(Visibility::Visible);
}

// Nouvel onglet sur la page d'accueil, adresse prête à la saisie. À la
// limite, le sélecteur s'ouvre pour en fermer un.
void NewTab() {
  CloseTabs();
  if (!RheniumEngineNewTab(HomeUrl().c_str())) {
    OpenTabs();
    return;
  }
  g_shell->address.Focus(FocusState::Programmatic);
}

// Aperçus sur deux colonnes ; en bas, « + » et le bouton des onglets, à la
// place de ceux de la barre.
Grid BuildTabs() {
  ShellState& shell = *g_shell;
  shell.tab_cards = Grid();
  shell.tab_cards.Padding(Thickness{6, 6, 6, 6});
  for (int column = 0; column < 2; ++column)
    shell.tab_cards.ColumnDefinitions().Append(ColumnDefinition());
  ScrollViewer scroll;
  scroll.Content(shell.tab_cards);

  shell.new_tab = FlatButton();
  shell.new_tab.Width(kBarHeight);
  shell.new_tab.Height(kBarHeight);
  shell.new_tab.Content(Glyph(kGlyphAdd));
  shell.new_tab.HorizontalAlignment(HorizontalAlignment::Center);
  shell.new_tab.Click([](auto&&, auto&&) { NewTab(); });
  Button done = TabsButton();
  done.HorizontalAlignment(HorizontalAlignment::Right);
  done.Margin(Thickness{0, 0, kBarHeight, 0});  // à la place de celui de la barre
  done.Click([](auto&&, auto&&) { CloseTabs(); });
  Grid bar;
  bar.Height(kBarHeight);
  bar.Background(Brush(kBarColor));
  bar.Children().Append(shell.new_tab);
  bar.Children().Append(done);

  Grid tabs;
  tabs.Background(Brush(kBarColor));
  RowDefinition cards_row;
  cards_row.Height(GridLength{1, GridUnitType::Star});
  tabs.RowDefinitions().Append(cards_row);
  RowDefinition bar_row;
  bar_row.Height(GridLength{1, GridUnitType::Auto});
  tabs.RowDefinitions().Append(bar_row);
  Grid::SetRow(bar, 1);
  tabs.Children().Append(scroll);
  tabs.Children().Append(bar);
  tabs.Visibility(Visibility::Collapsed);
  return tabs;
}

// Historique

void CloseHistory() {
  g_shell->history.Visibility(Visibility::Collapsed);
  g_shell->history_list.Children().Clear();
}

SYSTEMTIME LocalTime(long long time) {
  const FILETIME utc{static_cast<DWORD>(time), static_cast<DWORD>(time >> 32)};
  SYSTEMTIME system{};
  SYSTEMTIME local{};
  FileTimeToSystemTime(&utc, &system);
  SystemTimeToTzSpecificLocalTime(nullptr, &system, &local);
  return local;
}

int DayKey(SYSTEMTIME const& day) {
  return day.wYear * 10000 + day.wMonth * 100 + day.wDay;
}

// « Aujourd'hui », « Hier », sinon la date.
std::wstring DayLabel(SYSTEMTIME const& day) {
  constexpr long long kDay = 24LL * 3600 * 10000000;  // en 100 ns
  const long long now = Now();
  if (DayKey(day) == DayKey(LocalTime(now)))
    return Tr(L"Aujourd'hui", L"Today");
  if (DayKey(day) == DayKey(LocalTime(now - kDay)))
    return Tr(L"Hier", L"Yesterday");
  wchar_t text[64] = L"";
  GetDateFormatEx(Tr(L"fr-FR", L"en-US"), 0, &day,
                  Tr(L"dddd d MMMM yyyy", L"dddd, MMMM d, yyyy"), text, 64,
                  nullptr);
  return text;
}

void RemoveVisit(long long time, std::wstring const& url) {
  std::vector<HistoryEntry>& visits = g_shell->visits;
  for (auto it = visits.begin(); it != visits.end(); ++it) {
    if (it->time == time && it->url == url) {
      visits.erase(it);
      break;
    }
  }
  SaveHistory();
}

void RebuildHistory();

// Ligne d'une liste (historique, favoris, téléchargements) : titre et
// détail (site, état). Un toucher l'ouvre ; un appui long, ou un clic droit
// à la souris, affiche son menu. Au toucher, le bouton reçoit aussi un Click
// au relâchement de l'appui long : ignoré.
template <typename Open, typename Menu>
Button ListItem(std::wstring const& title,
                std::wstring const& detail,
                Open open,
                Menu build_menu) {
  StackPanel text;
  text.Children().Append(Trimmed(Label(title.c_str(), 15)));
  text.Children().Append(Trimmed(Label(detail.c_str(), 12, kDimTextColor)));
  Button item = FlatButton();
  item.HorizontalAlignment(HorizontalAlignment::Stretch);
  item.HorizontalContentAlignment(HorizontalAlignment::Left);
  item.Padding(Thickness{0, 8, 0, 8});
  item.Content(text);
  auto held = std::make_shared<bool>(false);
  item.Click([held, open](auto&&, auto&&) {
    if (!std::exchange(*held, false))
      open();
  });
  auto show_menu = [held, build_menu](FrameworkElement const& target) {
    MenuFlyout menu = build_menu();
    Style presenter(xaml_typename<MenuFlyoutPresenter>());
    presenter.Setters().Append(Setter(FrameworkElement::RequestedThemeProperty(),
                                      box_value(ShellTheme())));
    menu.MenuFlyoutPresenterStyle(presenter);
    menu.Closed([held](auto&&, auto&&) { *held = false; });
    menu.ShowAt(target);
  };
  item.Holding([held, show_menu](Windows::Foundation::IInspectable const& sender,
                                 HoldingRoutedEventArgs const& args) {
    if (args.HoldingState() != Windows::UI::Input::HoldingState::Started)
      return;
    args.Handled(true);
    *held = true;
    show_menu(sender.as<FrameworkElement>());
  });
  item.RightTapped([show_menu](Windows::Foundation::IInspectable const& sender,
                               RightTappedRoutedEventArgs const& args) {
    if (args.PointerDeviceType() ==
        Windows::Devices::Input::PointerDeviceType::Mouse) {
      show_menu(sender.as<FrameworkElement>());
    }
  });
  return item;
}

// Visite ; son menu propose de la supprimer.
Button VisitItem(HistoryEntry const& entry) {
  return ListItem(
      entry.title.empty() ? entry.url : entry.title, DisplayText(entry.url),
      [url = entry.url] {
        CloseHistory();
        RheniumEngineNavigate(url.c_str());
      },
      [time = entry.time, url = entry.url] {
        MenuFlyoutItem remove;
        remove.Text(Tr(L"Supprimer", L"Delete"));
        remove.Click([time, url](auto&&, auto&&) {
          RemoveVisit(time, url);
          RebuildHistory();
        });
        MenuFlyout menu;
        menu.Items().Append(remove);
        return menu;
      });
}

// Ajoute les kHistoryPage visites suivantes, des plus récentes aux plus
// anciennes, sous le titre de leur jour.
void ShowMoreHistory() {
  ShellState& shell = *g_shell;
  auto children = shell.history_list.Children();
  if (shell.history_shown)
    children.RemoveAtEnd();  // « Plus »
  const size_t total = shell.visits.size();
  const size_t end = std::min(total, shell.history_shown + kHistoryPage);
  for (; shell.history_shown < end; ++shell.history_shown) {
    HistoryEntry const& entry = shell.visits[total - 1 - shell.history_shown];
    const SYSTEMTIME day = LocalTime(entry.time);
    if (DayKey(day) != shell.history_day) {
      shell.history_day = DayKey(day);
      TextBlock title = Label(DayLabel(day).c_str(), 13);
      title.Foreground(Brush(AccentColor()));
      title.Margin(Thickness{0, 16, 0, 4});
      children.Append(title);
    }
    children.Append(VisitItem(entry));
  }
  if (shell.history_shown < total) {
    HyperlinkButton more;
    more.Content(box_value(Tr(L"Plus", L"More")));
    more.Click([](auto&&, auto&&) { ShowMoreHistory(); });
    children.Append(more);
  }
}

void RebuildHistory() {
  ShellState& shell = *g_shell;
  shell.history_list.Children().Clear();
  shell.history_shown = 0;
  shell.history_day = 0;
  if (shell.visits.empty()) {
    TextBlock empty =
        Label(Tr(L"Aucune page visitée.", L"No pages visited."), 15,
              kDimTextColor);
    empty.Margin(Thickness{0, 16, 0, 0});
    shell.history_list.Children().Append(empty);
    return;
  }
  ShowMoreHistory();
}

void OpenHistory() {
  RebuildHistory();
  g_shell->history.Visibility(Visibility::Visible);
}

Grid BuildHistory() {
  ShellState& shell = *g_shell;
  HyperlinkButton clear;
  clear.Content(box_value(Tr(L"Effacer", L"Clear")));
  clear.HorizontalAlignment(HorizontalAlignment::Right);
  clear.Click([](auto&&, auto&&) {
    g_shell->visits.clear();
    SaveHistory();
    RebuildHistory();
  });
  Grid header;
  header.Children().Append(PanelHeader(Tr(L"HISTORIQUE", L"HISTORY")));
  header.Children().Append(clear);

  shell.history_list = StackPanel();
  StackPanel content;
  content.Padding(Thickness{20, 12, 20, 24});
  content.Children().Append(header);
  content.Children().Append(shell.history_list);
  ScrollViewer scroll;
  scroll.Content(content);
  Grid history;
  history.Background(Brush(kBarColor));
  history.Children().Append(scroll);
  history.Visibility(Visibility::Collapsed);
  return history;
}

// Favoris

// Étoile du menu : ajoute la page de l'onglet actif, ou la retire.
void ToggleFavorite() {
  ShellState& shell = *g_shell;
  if (!CanBeFavorite(shell.url))
    return;
  if (auto it = FindFavorite(shell.url); it != shell.favorites.end()) {
    shell.favorites.erase(it);
  } else {
    shell.favorites.push_back(
        {CleanField(shell.url),
         CleanField(shell.title.empty() ? shell.url : shell.title)});
  }
  SaveFavorites();
}

void CloseFavorites() {
  g_shell->favorites_panel.Visibility(Visibility::Collapsed);
  g_shell->favorite_list.Children().Clear();
}

void RebuildFavorites();

void RenameFavorite(std::wstring const& url) {
  auto it = FindFavorite(url);
  if (it == g_shell->favorites.end())
    return;
  try {
    TextBox name;
    name.Text(it->title);
    name.SelectAll();
    ContentDialog dialog;
    dialog.RequestedTheme(ShellTheme());
    dialog.Title(box_value(Tr(L"Renommer le favori", L"Rename favorite")));
    dialog.Content(name);
    dialog.PrimaryButtonText(Tr(L"Renommer", L"Rename"));
    dialog.CloseButtonText(Tr(L"Annuler", L"Cancel"));
    dialog.PrimaryButtonClick([url, name](auto&&, auto&&) {
      auto it = FindFavorite(url);
      const std::wstring title = CleanField(std::wstring(name.Text()));
      if (it == g_shell->favorites.end() || title.empty())
        return;
      it->title = title;
      SaveFavorites();
      RebuildFavorites();
    });
    dialog.ShowAsync();
  } catch (...) {
  }
}

// Favori ; son menu propose de le renommer ou de le supprimer.
Button FavoriteItem(Favorite const& favorite) {
  return ListItem(
      favorite.title, DisplayText(favorite.url),
      [url = favorite.url] {
        CloseFavorites();
        RheniumEngineNavigate(url.c_str());
      },
      [url = favorite.url] {
        MenuFlyoutItem rename;
        rename.Text(Tr(L"Renommer", L"Rename"));
        rename.Click([url](auto&&, auto&&) { RenameFavorite(url); });
        MenuFlyoutItem remove;
        remove.Text(Tr(L"Supprimer", L"Delete"));
        remove.Click([url](auto&&, auto&&) {
          if (auto it = FindFavorite(url); it != g_shell->favorites.end()) {
            g_shell->favorites.erase(it);
            SaveFavorites();
          }
          RebuildFavorites();
        });
        MenuFlyout menu;
        menu.Items().Append(rename);
        menu.Items().Append(remove);
        return menu;
      });
}

// Du plus récent au plus ancien.
void RebuildFavorites() {
  ShellState& shell = *g_shell;
  auto children = shell.favorite_list.Children();
  children.Clear();
  if (shell.favorites.empty()) {
    TextBlock empty = Label(
        Tr(L"Aucun favori. Pour en ajouter un : « … », puis l'étoile.",
           L"No favorites yet. To add one: “…”, then the star."),
        15, kDimTextColor);
    empty.Margin(Thickness{0, 16, 0, 0});
    children.Append(empty);
    return;
  }
  for (auto it = shell.favorites.rbegin(); it != shell.favorites.rend(); ++it)
    children.Append(FavoriteItem(*it));
}

void OpenFavorites() {
  RebuildFavorites();
  g_shell->favorites_panel.Visibility(Visibility::Visible);
}

Grid BuildFavorites() {
  ShellState& shell = *g_shell;
  shell.favorite_list = StackPanel();
  shell.favorite_list.Margin(Thickness{0, 8, 0, 0});
  StackPanel content;
  content.Padding(Thickness{20, 12, 20, 24});
  content.Children().Append(PanelHeader(Tr(L"FAVORIS", L"FAVORITES")));
  content.Children().Append(shell.favorite_list);
  ScrollViewer scroll;
  scroll.Content(content);
  Grid favorites;
  favorites.Background(Brush(kBarColor));
  favorites.Children().Append(scroll);
  favorites.Visibility(Visibility::Collapsed);
  return favorites;
}

// Téléchargements

using Windows::Storage::AccessCache::StorageApplicationPermissions;

// Ceux des sessions précédentes, déjà dans Téléchargements : heure, taille,
// jeton, nom.
void LoadDownloads() {
  for (auto const& line : ReadLines(kDownloadsFile, 4)) {
    DownloadEntry entry;
    entry.time = std::wcstoll(line[0].c_str(), nullptr, 10);
    entry.received = entry.total = std::wcstoll(line[1].c_str(), nullptr, 10);
    entry.token = line[2];
    entry.name = line[3];
    entry.state = RHENIUM_DOWNLOAD_COMPLETE;
    g_shell->downloads.push_back(std::move(entry));
  }
}

void SaveDownloads() {
  static std::atomic<unsigned> latest{0};
  std::string data;
  for (DownloadEntry const& entry : g_shell->downloads) {
    if (entry.token.empty())
      continue;
    data += std::to_string(entry.time) + '\t' + std::to_string(entry.received) +
            '\t' + to_string(entry.token) + '\t' + to_string(entry.name) + '\n';
  }
  SaveLater(kDownloadsFile, std::move(data), latest);
}

DownloadEntry* FindDownload(long long time) {
  for (DownloadEntry& entry : g_shell->downloads) {
    if (entry.time == time)
      return &entry;
  }
  return nullptr;
}

DownloadEntry* FindDownloadById(int id) {
  for (DownloadEntry& entry : g_shell->downloads) {
    if (entry.id == id)
      return &entry;
  }
  return nullptr;
}

// Retire la ligne, pas le fichier.
void ForgetDownload(long long time) {
  std::vector<DownloadEntry>& downloads = g_shell->downloads;
  auto it = std::find_if(
      downloads.begin(), downloads.end(),
      [&](DownloadEntry const& entry) { return entry.time == time; });
  if (it == downloads.end())
    return;
  if (!it->token.empty()) {
    try {
      StorageApplicationPermissions::FutureAccessList().Remove(it->token);
    } catch (...) {
    }
  }
  downloads.erase(it);
  SaveDownloads();
}

std::wstring SizeText(long long bytes) {
  wchar_t text[32];
  if (bytes < 1024 * 1024) {
    swprintf_s(text, Tr(L"%lld Ko", L"%lld KB"), (bytes + 1023) / 1024);
  } else {
    swprintf_s(text, Tr(L"%.1f Mo", L"%.1f MB"),
               static_cast<double>(bytes) / (1024 * 1024));
  }
  std::wstring size(text);
  if (!g_english)
    std::replace(size.begin(), size.end(), L'.', L',');
  return size;
}

std::wstring DownloadName(DownloadEntry const& entry) {
  return entry.name.empty() ? Tr(L"Téléchargement", L"Download") : entry.name;
}

std::wstring DownloadDetail(DownloadEntry const& entry) {
  if (entry.state == RHENIUM_DOWNLOAD_FAILED)
    return Tr(L"Échec", L"Failed");
  if (entry.moving)
    return Tr(L"Enregistrement…", L"Saving…");
  if (entry.state == RHENIUM_DOWNLOAD_COMPLETE)
    return Tr(L"Terminé · ", L"Done · ") + SizeText(entry.received);
  if (entry.total <= 0)
    return SizeText(entry.received);
  wchar_t percent[16];
  swprintf_s(percent, Tr(L"%d %% · ", L"%d%% · "),
             static_cast<int>(entry.received * 100 / entry.total));
  return percent + SizeText(entry.received) + Tr(L" sur ", L" of ") +
         SizeText(entry.total);
}

bool DownloadsOpen() {
  return g_shell->downloads_panel && IsShown(g_shell->downloads_panel);
}

void RebuildDownloads();

fire_and_forget OpenDownloadFile(std::wstring token) {
  bool found = false;
  try {
    auto file =
        co_await StorageApplicationPermissions::FutureAccessList().GetFileAsync(
            token);
    found = true;
    co_await resume_foreground(g_shell->dispatcher);
    co_await Windows::System::Launcher::LaunchFileAsync(file);
  } catch (...) {
  }
  if (!found) {
    co_await resume_foreground(g_shell->dispatcher);
    ShowMessage(Tr(L"Fichier introuvable", L"File not found"),
                Tr(L"Le fichier a été déplacé ou supprimé.",
                   L"The file was moved or deleted."));
  }
}

fire_and_forget DeleteDownloadFile(long long time, std::wstring token) {
  try {
    auto file =
        co_await StorageApplicationPermissions::FutureAccessList().GetFileAsync(
            token);
    co_await file.DeleteAsync();
  } catch (...) {
  }
  co_await resume_foreground(g_shell->dispatcher);
  ForgetDownload(time);
  if (DownloadsOpen())
    RebuildDownloads();
}

// Bandeau : le dernier téléchargement, avec « Annuler » pendant, « Ouvrir »
// après, refermé seul quelques secondes après la fin.
void HideBanner() {
  ShellState& shell = *g_shell;
  shell.banner.Visibility(Visibility::Collapsed);
  shell.banner_timer.Stop();
  shell.banner_time = 0;
}

void RefreshBanner(DownloadEntry const& entry) {
  ShellState& shell = *g_shell;
  shell.banner_name.Text(DownloadName(entry));
  shell.banner_detail.Text(DownloadDetail(entry));
  const bool running = entry.state == RHENIUM_DOWNLOAD_IN_PROGRESS;
  shell.banner_progress.Visibility(running || entry.moving
                                       ? Visibility::Visible
                                       : Visibility::Collapsed);
  shell.banner_progress.IsIndeterminate(entry.moving || entry.total <= 0);
  if (entry.total > 0) {
    shell.banner_progress.Value(static_cast<double>(entry.received) /
                                entry.total);
  }
  const bool can_open = !entry.token.empty();
  shell.banner_action.Visibility(running || can_open ? Visibility::Visible
                                                     : Visibility::Collapsed);
  shell.banner_action.Content(
      box_value(running ? Tr(L"Annuler", L"Cancel") : Tr(L"Ouvrir", L"Open")));
  if (running || entry.moving)
    shell.banner_timer.Stop();
  else
    shell.banner_timer.Start();
}

void ShowBanner(DownloadEntry const& entry) {
  ShellState& shell = *g_shell;
  shell.banner_time = entry.time;
  RefreshBanner(entry);
  shell.banner.Visibility(Visibility::Visible);
}

void DownloadUpdated(DownloadEntry const& entry) {
  ShellState& shell = *g_shell;
  if (shell.banner_time == entry.time)
    RefreshBanner(entry);
  if (auto it = shell.download_details.find(entry.time);
      it != shell.download_details.end()) {
    it->second.Text(DownloadDetail(entry));
  }
}

// Du dossier du profil vers Téléchargements, où le système choisit un nom
// libre ; le fichier se retrouve ensuite par son jeton.
fire_and_forget MoveToDownloads(long long time,
                                std::wstring path,
                                std::wstring name) {
  std::wstring token;
  std::wstring saved_name;
  try {
    auto source =
        co_await Windows::Storage::StorageFile::GetFileFromPathAsync(path);
    auto target = co_await Windows::Storage::DownloadsFolder::CreateFileAsync(
        name, Windows::Storage::CreationCollisionOption::GenerateUniqueName);
    co_await source.MoveAndReplaceAsync(target);
    saved_name = target.Name();
    token = StorageApplicationPermissions::FutureAccessList().Add(target);
  } catch (hresult_error const& e) {
    Note(L"coquille : téléchargement non déplacé",
         static_cast<unsigned>(e.code().value));
  } catch (...) {
  }
  co_await resume_foreground(g_shell->dispatcher);
  DownloadEntry* entry = FindDownload(time);
  if (!entry)
    co_return;
  entry->moving = false;
  if (token.empty()) {
    entry->state = RHENIUM_DOWNLOAD_FAILED;
  } else {
    entry->token = token;
    entry->name = saved_name;
    SaveDownloads();
  }
  DownloadUpdated(*entry);
  if (DownloadsOpen())
    RebuildDownloads();
}

// Au-delà de kMaxDownloads, les plus anciens terminés quittent la liste.
void TrimDownloads() {
  std::vector<DownloadEntry>& downloads = g_shell->downloads;
  for (size_t i = 0; downloads.size() > kMaxDownloads && i < downloads.size();) {
    if (downloads[i].state == RHENIUM_DOWNLOAD_IN_PROGRESS ||
        downloads[i].moving) {
      ++i;
    } else {
      ForgetDownload(downloads[i].time);
    }
  }
}

void OnDownloadChanged(void*,
                       int id,
                       int state,
                       long long received,
                       long long total,
                       wchar_t const* name,
                       wchar_t const* path) noexcept {
  try {
    ShellState& shell = *g_shell;
    DownloadEntry* entry = FindDownloadById(id);
    const bool added = !entry;
    if (added) {
      DownloadEntry download;
      download.id = id;
      download.time = Now();
      shell.downloads.push_back(download);
      TrimDownloads();
      entry = FindDownload(download.time);
      RheniumEngineNote(L"coquille : téléchargement");
    }
    if (entry->state == RHENIUM_DOWNLOAD_COMPLETE)
      return;
    const bool renamed = *name && entry->name != name;
    if (renamed)
      entry->name = CleanField(name);
    entry->received = received;
    entry->total = total;
    entry->state = state;
    if (state == RHENIUM_DOWNLOAD_COMPLETE) {
      entry->moving = true;
      MoveToDownloads(entry->time, path, entry->name);
    }
    if (added)
      ShowBanner(*entry);
    else
      DownloadUpdated(*entry);
    if (DownloadsOpen() && (added || renamed))
      RebuildDownloads();
  } catch (...) {
  }
}

Button DownloadRow(DownloadEntry const& entry) {
  Button row = ListItem(
      DownloadName(entry), DownloadDetail(entry),
      [time = entry.time] {
        DownloadEntry* entry = FindDownload(time);
        if (entry && !entry->token.empty())
          OpenDownloadFile(entry->token);
      },
      [time = entry.time] {
        MenuFlyout menu;
        DownloadEntry* entry = FindDownload(time);
        if (!entry)
          return menu;
        if (entry->state == RHENIUM_DOWNLOAD_IN_PROGRESS) {
          MenuFlyoutItem cancel;
          cancel.Text(Tr(L"Annuler", L"Cancel"));
          cancel.Click([id = entry->id](auto&&, auto&&) {
            RheniumEngineCancelDownload(id);
          });
          menu.Items().Append(cancel);
          return menu;
        }
        if (!entry->token.empty()) {
          MenuFlyoutItem remove_file;
          remove_file.Text(Tr(L"Supprimer le fichier", L"Delete file"));
          remove_file.Click([time, token = entry->token](auto&&, auto&&) {
            DeleteDownloadFile(time, token);
          });
          menu.Items().Append(remove_file);
        }
        if (!entry->moving) {
          MenuFlyoutItem forget;
          forget.Text(Tr(L"Retirer de la liste", L"Remove from list"));
          forget.Click([time](auto&&, auto&&) {
            ForgetDownload(time);
            RebuildDownloads();
          });
          menu.Items().Append(forget);
        }
        return menu;
      });
  g_shell->download_details.insert_or_assign(
      entry.time,
      row.Content().as<StackPanel>().Children().GetAt(1).as<TextBlock>());
  return row;
}

// Du plus récent au plus ancien.
void RebuildDownloads() {
  ShellState& shell = *g_shell;
  auto children = shell.download_list.Children();
  children.Clear();
  shell.download_details.clear();
  if (shell.downloads.empty()) {
    TextBlock empty = Label(Tr(L"Aucun téléchargement.", L"No downloads."), 15,
                            kDimTextColor);
    empty.Margin(Thickness{0, 16, 0, 0});
    children.Append(empty);
    return;
  }
  for (auto it = shell.downloads.rbegin(); it != shell.downloads.rend(); ++it)
    children.Append(DownloadRow(*it));
}

void CloseDownloads() {
  g_shell->downloads_panel.Visibility(Visibility::Collapsed);
  g_shell->download_list.Children().Clear();
  g_shell->download_details.clear();
}

void OpenDownloads() {
  RebuildDownloads();
  g_shell->downloads_panel.Visibility(Visibility::Visible);
}

Grid BuildDownloads() {
  ShellState& shell = *g_shell;
  HyperlinkButton clear;
  clear.Content(box_value(Tr(L"Vider la liste", L"Clear list")));
  clear.HorizontalAlignment(HorizontalAlignment::Right);
  clear.Click([](auto&&, auto&&) {
    std::vector<long long> finished;
    for (DownloadEntry const& entry : g_shell->downloads) {
      if (entry.state != RHENIUM_DOWNLOAD_IN_PROGRESS && !entry.moving)
        finished.push_back(entry.time);
    }
    for (long long time : finished)
      ForgetDownload(time);
    RebuildDownloads();
  });
  Grid header;
  header.Children().Append(PanelHeader(Tr(L"TÉLÉCHARGEMENTS", L"DOWNLOADS")));
  header.Children().Append(clear);

  TextBlock where = Label(Tr(L"Les fichiers sont enregistrés dans le dossier "
                             L"Téléchargements du téléphone.",
                             L"Files are saved to the phone's Downloads "
                             L"folder."),
                          13, kDimTextColor);
  where.Margin(Thickness{0, 4, 0, 8});
  shell.download_list = StackPanel();
  StackPanel content;
  content.Padding(Thickness{20, 12, 20, 24});
  content.Children().Append(header);
  content.Children().Append(where);
  content.Children().Append(shell.download_list);
  ScrollViewer scroll;
  scroll.Content(content);
  Grid downloads;
  downloads.Background(Brush(kBarColor));
  downloads.Children().Append(scroll);
  downloads.Visibility(Visibility::Collapsed);
  return downloads;
}

Grid BuildBanner() {
  ShellState& shell = *g_shell;
  shell.banner_name = Trimmed(Label(L"", 15));
  shell.banner_detail = Trimmed(Label(L"", 12, kDimTextColor));
  StackPanel text;
  text.Background(Brush(Windows::UI::Colors::Transparent()));
  text.VerticalAlignment(VerticalAlignment::Center);
  text.Padding(Thickness{16, 0, 8, 0});
  text.Children().Append(shell.banner_name);
  text.Children().Append(shell.banner_detail);
  text.Tapped([](auto&&, auto&&) {
    HideBanner();
    OpenDownloads();
  });

  shell.banner_action = FlatButton();
  shell.banner_action.Height(kBarHeight);
  shell.banner_action.Padding(Thickness{12, 0, 12, 0});
  shell.banner_action.Foreground(Brush(AccentColor()));
  shell.banner_action.Click([](auto&&, auto&&) {
    DownloadEntry* entry = FindDownload(g_shell->banner_time);
    if (!entry)
      return;
    if (entry->state == RHENIUM_DOWNLOAD_IN_PROGRESS) {
      RheniumEngineCancelDownload(entry->id);
    } else if (!entry->token.empty()) {
      OpenDownloadFile(entry->token);
      HideBanner();
    }
  });
  Button close = FlatButton();
  close.Width(kBarHeight);
  close.Height(kBarHeight);
  FontIcon cross = Glyph(kGlyphCancel);
  cross.FontSize(12);
  close.Content(cross);
  close.Click([](auto&&, auto&&) { HideBanner(); });

  shell.banner_progress = ProgressBar();
  shell.banner_progress.Maximum(1);
  shell.banner_progress.Height(2);
  shell.banner_progress.MinHeight(2);
  shell.banner_progress.Background(Brush(Windows::UI::Colors::Transparent()));
  shell.banner_progress.VerticalAlignment(VerticalAlignment::Top);

  Grid banner;
  banner.Background(Brush(kMenuColor));
  banner.Height(56);
  banner.VerticalAlignment(VerticalAlignment::Bottom);
  ColumnDefinition text_column;
  text_column.Width(GridLength{1, GridUnitType::Star});
  banner.ColumnDefinitions().Append(text_column);
  for (int column = 1; column <= 2; ++column) {
    ColumnDefinition button_column;
    button_column.Width(GridLength{1, GridUnitType::Auto});
    banner.ColumnDefinitions().Append(button_column);
  }
  Grid::SetColumn(shell.banner_action, 1);
  Grid::SetColumn(close, 2);
  Grid::SetColumnSpan(shell.banner_progress, 3);
  banner.Children().Append(text);
  banner.Children().Append(shell.banner_action);
  banner.Children().Append(close);
  banner.Children().Append(shell.banner_progress);
  banner.Visibility(Visibility::Collapsed);

  shell.banner_timer = DispatcherTimer();
  shell.banner_timer.Interval(std::chrono::seconds(8));
  shell.banner_timer.Tick([](auto&&, auto&&) { HideBanner(); });
  return banner;
}

Grid BuildBar() {
  ShellState& shell = *g_shell;
  shell.address = TextBox();
  StyleAddress(shell.address);
  SetUrlInputScope(shell.address);
  shell.address.PlaceholderText(
      Tr(L"Adresse ou recherche", L"Search or enter address"));
  shell.address.VerticalAlignment(VerticalAlignment::Center);
  shell.address.Margin(Thickness{8, 6, 0, 6});
  shell.address.GotFocus([](auto&&, auto&&) {
    CloseMenu();
    RheniumEngineSetWebFocus(0);
    ShowAddress();
    g_shell->address.SelectAll();
  });
  shell.address.LostFocus([](auto&&, auto&&) { ShowAddress(); });
  shell.address.KeyDown([](auto&&, KeyRoutedEventArgs const& args) {
    if (args.Key() != Windows::System::VirtualKey::Enter)
      return;
    args.Handled(true);
    const hstring text = g_shell->address.Text();
    FocusWeb();
    RheniumEngineNavigate(text.c_str());
  });

  Button more;
  more.Content(Glyph(kGlyphMore));
  more.Width(kBarHeight);
  more.Height(kBarHeight);
  more.Padding(Thickness{0, 0, 0, 0});
  more.Background(Brush(Windows::UI::Colors::Transparent()));
  more.BorderThickness(Thickness{0, 0, 0, 0});
  more.Click([](auto&&, auto&&) {
    if (MenuOpen())
      CloseMenu();
    else
      OpenMenu();
  });
  Button tabs = TabsButton();
  tabs.Click([](auto&&, auto&&) { OpenTabs(); });

  // Fil de progression en haut de la barre.
  shell.progress = ProgressBar();
  shell.progress.Maximum(1);
  shell.progress.Height(2);
  shell.progress.MinHeight(2);
  shell.progress.Background(Brush(Windows::UI::Colors::Transparent()));
  shell.progress.VerticalAlignment(VerticalAlignment::Top);
  shell.progress.Visibility(Visibility::Collapsed);

  Grid bar;
  bar.Height(kBarHeight);
  bar.Background(Brush(kBarColor));
  ColumnDefinition address_column;
  address_column.Width(GridLength{1, GridUnitType::Star});
  bar.ColumnDefinitions().Append(address_column);
  for (int column = 1; column <= 2; ++column) {
    ColumnDefinition button_column;
    button_column.Width(GridLength{1, GridUnitType::Auto});
    bar.ColumnDefinitions().Append(button_column);
  }
  Grid::SetColumn(tabs, 1);
  Grid::SetColumn(more, 2);
  Grid::SetColumnSpan(shell.progress, 3);
  bar.Children().Append(shell.address);
  bar.Children().Append(tabs);
  bar.Children().Append(more);
  bar.Children().Append(shell.progress);
  return bar;
}

void BuildContentArea() {
  ShellState& shell = *g_shell;
  shell.panel = SwapChainPanel();
  shell.panel.SizeChanged([](auto&&, auto&&) {
    ShellState& shell = *g_shell;
    if (shell.panel.ActualWidth() <= 0)
      return;
    shell.host.width_dip = static_cast<float>(shell.panel.ActualWidth());
    shell.host.height_dip = static_cast<float>(shell.panel.ActualHeight());
    if (!shell.engine_started)
      StartEngine();
    else
      RheniumEngineResize(shell.host.width_dip, shell.host.height_dip);
  });

  // Le SwapChainPanel refuse un fond, et sans fond une grille ne reçoit pas
  // les pointeurs : une grille transparente de même taille les prend.
  shell.input_surface = Grid();
  shell.input_surface.Background(Brush(Windows::UI::Colors::Transparent()));
  shell.input_surface.PointerPressed([](auto&&, PointerRoutedEventArgs const& args) {
    g_shell->input_surface.CapturePointer(args.Pointer());
    FocusWeb();
    ForwardPointer(RHENIUM_POINTER_PRESSED, args);
  });
  shell.input_surface.PointerMoved([](auto&&, PointerRoutedEventArgs const& args) {
    ForwardPointer(RHENIUM_POINTER_MOVED, args);
  });
  shell.input_surface.PointerReleased([](auto&&, PointerRoutedEventArgs const& args) {
    ForwardPointer(RHENIUM_POINTER_RELEASED, args);
    g_shell->input_surface.ReleasePointerCapture(args.Pointer());
  });
  shell.input_surface.PointerCanceled([](auto&&, PointerRoutedEventArgs const& args) {
    ForwardPointer(RHENIUM_POINTER_CANCELED, args);
  });
  shell.input_surface.PointerCaptureLost(
      [](auto&&, PointerRoutedEventArgs const& args) {
        ForwardPointer(RHENIUM_POINTER_CANCELED, args);
      });

  shell.focus_sink = ContentControl();
  shell.focus_sink.UseSystemFocusVisuals(false);
  shell.focus_sink.IsHitTestVisible(false);
  shell.focus_sink.Width(1);
  shell.focus_sink.Height(1);
  shell.focus_sink.Opacity(0);
  shell.focus_sink.HorizontalAlignment(HorizontalAlignment::Left);
  shell.focus_sink.VerticalAlignment(VerticalAlignment::Top);

  shell.dismiss_layer = Border();
  shell.dismiss_layer.Background(Brush(Windows::UI::Colors::Transparent()));
  shell.dismiss_layer.Visibility(Visibility::Collapsed);
  shell.dismiss_layer.PointerPressed([](auto&&, PointerRoutedEventArgs const& args) {
    args.Handled(true);
    CloseMenu();
  });
}

UIElement BuildUi() {
  ShellState& shell = *g_shell;
  BuildContentArea();
  Grid bar = BuildBar();
  shell.menu = BuildMenu();
  shell.settings = BuildSettings();
  shell.tabs = BuildTabs();
  shell.history = BuildHistory();
  shell.favorites_panel = BuildFavorites();
  shell.downloads_panel = BuildDownloads();
  shell.banner = BuildBanner();

  Grid layout;
  RowDefinition content_row;
  content_row.Height(GridLength{1, GridUnitType::Star});
  layout.RowDefinitions().Append(content_row);
  RowDefinition bar_row;
  bar_row.Height(GridLength{1, GridUnitType::Auto});
  layout.RowDefinitions().Append(bar_row);
  Grid::SetRow(bar, 1);
  // Ordre d'empilement : la page, la barre, le menu, les panneaux.
  for (UIElement const& child :
       {UIElement(shell.focus_sink), UIElement(shell.panel),
        UIElement(shell.input_surface), UIElement(bar),
        UIElement(shell.banner), UIElement(shell.dismiss_layer),
        UIElement(shell.menu), UIElement(shell.tabs), UIElement(shell.history),
        UIElement(shell.favorites_panel), UIElement(shell.downloads_panel),
        UIElement(shell.settings)}) {
    layout.Children().Append(child);
  }
  for (Grid const& panel : {shell.tabs, shell.history, shell.favorites_panel,
                            shell.downloads_panel, shell.settings}) {
    Grid::SetRowSpan(panel, 2);
  }

  shell.root = Border();
  shell.root.Background(Brush(kBarColor));
  shell.root.RequestedTheme(ShellTheme());
  shell.root.Child(layout);
  return shell.root;
}

// Menu, historique et paramètres reconstruits dans la nouvelle langue, à
// leur place dans la grille ; les paramètres restent ouverts.
void ApplyLanguage() {
  ShellState& shell = *g_shell;
  std::wstring home_text(shell.home_url.Text());
  if (home_text == DefaultHomeUrl(!g_english))
    home_text = DefaultHomeUrl();
  auto children = shell.root.Child().as<Grid>().Children();
  auto replace = [&](auto& element, auto const& rebuilt) {
    uint32_t index = 0;
    if (children.IndexOf(element, index))
      children.SetAt(index, rebuilt);
    element = rebuilt;
  };
  CloseMenu();
  replace(shell.menu, BuildMenu());
  replace(shell.history, BuildHistory());
  replace(shell.favorites_panel, BuildFavorites());
  replace(shell.downloads_panel, BuildDownloads());
  replace(shell.settings, BuildSettings());
  for (Grid const& panel : {shell.history, shell.favorites_panel,
                            shell.downloads_panel, shell.settings}) {
    Grid::SetRowSpan(panel, 2);
  }
  shell.address.PlaceholderText(
      Tr(L"Adresse ou recherche", L"Search or enter address"));
  shell.home_url.Text(home_text);
  RebuildExtensions();
  shell.settings.Visibility(Visibility::Visible);
}

// 0 : langue du téléphone, 1 : français, 2 : anglais US.
void SetLanguage(int index) {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  if (index == 1 || index == 2) {
    values.Insert(kLanguageSetting, box_value(index == 1 ? L"fr-FR" : L"en-US"));
  } else if (values.HasKey(kLanguageSetting)) {
    values.Remove(kLanguageSetting);
  }
  const bool english = EnglishWanted();
  if (english == g_english)
    return;
  g_english = english;
  // Hors du gestionnaire : la liste qui l'appelle est recréée.
  g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal,
                               [] { ApplyLanguage(); });
}

void SetTheme(int index) {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  if (index == 1)
    values.Insert(kThemeSetting, box_value(L"light"));
  else if (index == 2)
    values.Insert(kThemeSetting, box_value(L"dark"));
  else if (values.HasKey(kThemeSetting))
    values.Remove(kThemeSetting);
  ApplyTheme();
}

void SetForceDark(bool force_dark) {
  auto values =
      Windows::Storage::ApplicationData::Current().LocalSettings().Values();
  if (force_dark)
    values.Insert(kForceDarkSetting, box_value(true));
  else if (values.HasKey(kForceDarkSetting))
    values.Remove(kForceDarkSetting);
  ApplyTheme();
}

// Bouton retour du téléphone : panneau, menu, puis le moteur (page
// d'extension, précédent, onglet ouvert par un lien) ; sinon le système
// ferme l'app.
bool HandleBack() {
  ShellState& shell = *g_shell;
  if (SettingsOpen())
    CloseSettings();
  else if (IsShown(shell.history))
    CloseHistory();
  else if (IsShown(shell.favorites_panel))
    CloseFavorites();
  else if (DownloadsOpen())
    CloseDownloads();
  else if (TabsOpen())
    CloseTabs();
  else if (MenuOpen())
    CloseMenu();
  else
    return RheniumEngineGoBack() != 0;
  return true;
}

void ShareCurrentPage(DataTransferManager const&,
                      Windows::ApplicationModel::DataTransfer::
                          DataRequestedEventArgs const& args) {
  auto data = args.Request().Data();
  data.Properties().Title(g_shell->title.empty() ? g_shell->url
                                                 : g_shell->title);
  data.SetWebLink(Windows::Foundation::Uri(g_shell->url));
}

struct App : ApplicationT<App> {
  App() {
    // Thème des menus surgissants, qui ne suivent que celui de l'app, fixé
    // au lancement.
    const std::wstring theme = ThemeSetting();
    if (!theme.empty()) {
      RequestedTheme(theme == L"dark" ? ApplicationTheme::Dark
                                      : ApplicationTheme::Light);
    }
    UnhandledException([](auto&&, UnhandledExceptionEventArgs const& args) {
      wchar_t code[16];
      swprintf_s(code, L"%08X", static_cast<unsigned>(args.Exception().value));
      RheniumEngineNote((L"coquille : exception XAML 0x" + std::wstring(code) +
                         L" " + std::wstring(args.Message())).c_str());
    });
  }

  void OnLaunched(
      Windows::ApplicationModel::Activation::LaunchActivatedEventArgs const&) {
    Window window = Window::Current();
    if (!g_shell) {
      g_shell = new ShellState();
      g_shell->dispatcher = window.Dispatcher();
      g_english = EnglishWanted();
      g_shell->engine_language = LanguageSetting();
      g_shell->engine_english = g_english;
      LoadHistory();
      LoadFavorites();
      LoadDownloads();
      ApplyTheme();
      g_shell->ui_settings = Windows::UI::ViewManagement::UISettings();
      g_shell->ui_settings.ColorValuesChanged([](auto&&, auto&&) {
        g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal,
                                     [] { FollowPhoneTheme(); });
      });
      // Fenêtre sur tout l'écran, marges posées par FitVisibleBounds.
      auto view = ApplicationView::GetForCurrentView();
      view.SetDesiredBoundsMode(
          Windows::UI::ViewManagement::ApplicationViewBoundsMode::UseCoreWindow);
      window.Content(BuildUi());
      FitVisibleBounds();
      view.VisibleBoundsChanged([](auto&&, auto&&) { FitVisibleBounds(); });
      auto input_pane = InputPane::GetForCurrentView();
      input_pane.Showing([](auto&&, InputPaneVisibilityEventArgs const& args) {
        OnKeyboardShowing(args);
      });
      input_pane.Hiding([](auto&&, auto&&) {
        if (g_shell->engine_started)
          RheniumEngineSetKeyboardInset(0);
      });
      // Écran d'accueil, verrouillage, suspension : plus de rendu.
      window.VisibilityChanged(
          [](auto&&, Windows::UI::Core::VisibilityChangedEventArgs const& args) {
            if (!g_shell->engine_started)
              return;
            if (!args.Visible())
              SaveSession();
            else
              FollowPhoneTheme();
            RheniumEngineSetVisible(args.Visible() ? 1 : 0);
          });
      DataTransferManager::GetForCurrentView().DataRequested(ShareCurrentPage);
      Windows::UI::Core::SystemNavigationManager::GetForCurrentView()
          .BackRequested([](auto&&, auto const& args) {
            args.Handled(HandleBack());
          });
    }
    window.Activate();
  }
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  init_apartment();  // XAML exige un thread MTA avant Application::Start
  // Après l'apartment : le journal trouve LocalState par l'API WinRT.
  RheniumEngineStartLog();
  Application::Start([](auto&&) { make<App>(); });
  return 0;
}
