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
// (icônes puis commandes). Onglets, historique et paramètres s'ouvrent en
// plein écran. Le précédent passe par le bouton retour du téléphone.

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
#include <mutex>
#include <string>
#include <vector>
#include <windows.ui.xaml.media.dxinterop.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.DataTransfer.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
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

constexpr Color kBarColor{0xFF, 0x1F, 0x1F, 0x1F};
constexpr Color kMenuColor{0xFF, 0x2B, 0x2B, 0x2B};
constexpr Color kFieldColor{0xFF, 0x33, 0x33, 0x33};
constexpr Color kFieldFocusedColor{0xFF, 0xF2, 0xF2, 0xF2};
constexpr Color kTextColor{0xFF, 0xE6, 0xE6, 0xE6};
constexpr Color kDimTextColor{0xFF, 0x99, 0x99, 0x99};
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
  bool installing = false;
  Grid tabs{nullptr};
  Grid tab_cards{nullptr};
  Button new_tab{nullptr};
  Grid history{nullptr};
  StackPanel history_list{nullptr};
  CoreDispatcher dispatcher{nullptr};
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

FontIcon Glyph(wchar_t const* glyph) {
  FontIcon icon;
  icon.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
  icon.FontSize(18);
  icon.Glyph(glyph);
  return icon;
}

TextBlock Label(wchar_t const* text, double size, Color color = kTextColor) {
  TextBlock block;
  block.Text(text);
  block.FontSize(size);
  block.Foreground(Brush(color));
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

// Hors du thread UI ; seule la dernière demande compte.
void SaveHistory() {
  static std::atomic<unsigned> latest{0};
  const unsigned request = ++latest;
  Windows::System::Threading::ThreadPool::RunAsync(
      [data = HistoryText(), path = LocalPath(kHistoryFile),
       request](auto&&) {
        std::lock_guard<std::mutex> lock(g_writing);
        if (request != latest || WriteLocalFile(path, data))
          return;
        g_shell->dispatcher.RunAsync(CoreDispatcherPriority::Normal, [] {
          RheniumEngineNote(L"coquille : historique non écrit");
        });
      });
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
                                          to_string(url) + '\t' +
                                          to_string(title) + '\n';
    }
  } catch (...) {
  }
}

// Avant que le système ne suspende puis, souvent, ne tue l'app : historique
// et onglets, tout de suite.
void SaveSession() {
  std::string tabs;
  RheniumEngineListTabs(&AddSavedTab, &tabs);
  std::lock_guard<std::mutex> lock(g_writing);
  if (!WriteLocalFile(LocalPath(kHistoryFile), HistoryText()) ||
      !WriteLocalFile(LocalPath(kTabsFile), tabs)) {
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
  char id[33];
  shell.add_extension.Visibility(
      !shell.installing && RheniumEngineWebStoreId(shell.url.c_str(), id)
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
  status_bar.BackgroundColor(kBarColor);
  status_bar.BackgroundOpacity(1);
  status_bar.ForegroundColor(kTextColor);
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
    std::wstring clean(title);
    for (wchar_t& c : clean) {
      if (c == L'\t' || c == L'\r' || c == L'\n')
        c = L' ';
    }
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
  } catch (...) {
  }
}

// Lance le moteur une fois la taille du panneau connue. RheniumEngineRun ne
// rend la main qu'à la fermeture : la boucle de Chromium fait tourner le
// CoreDispatcher, donc XAML continue de vivre.
void StartEngine() {
  ShellState& shell = *g_shell;
  shell.engine_started = true;
  LoadTabs();
  RheniumEngineSetSearchUrl(kSearchEngines[SearchEngineIndex()].prefix);
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
  auto set = [&](wchar_t const* key, Color color) {
    resources.Insert(box_value(key), Brush(color));
  };
  set(L"TextControlBackground", kFieldColor);
  set(L"TextControlBackgroundPointerOver", kFieldColor);
  set(L"TextControlBackgroundFocused", kFieldFocusedColor);
  set(L"TextControlForeground", kTextColor);
  set(L"TextControlForegroundPointerOver", kTextColor);
  set(L"TextControlForegroundFocused", kBarColor);
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
void NewTab();
void RebuildExtensions();
void SetLanguage(int index);

// Boîte de message ; une seule à la fois (XAML refuse d'en ouvrir deux).
void ShowMessage(std::wstring const& title, std::wstring const& text) {
  try {
    ContentDialog dialog;
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
  items.Children().Append(MenuItem(Tr(L"Historique", L"History"), OpenHistory));
  items.Children().Append(MenuItem(Tr(L"Accueil", L"Home"), [] {
    RheniumEngineNavigate(HomeUrl().c_str());
  }));
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

// Visite : titre et site ; un appui long propose de la supprimer.
Button VisitItem(HistoryEntry const& entry) {
  StackPanel text;
  text.Children().Append(Trimmed(Label(
      entry.title.empty() ? entry.url.c_str() : entry.title.c_str(), 15)));
  text.Children().Append(
      Trimmed(Label(DisplayText(entry.url).c_str(), 12, kDimTextColor)));
  Button item = FlatButton();
  item.HorizontalAlignment(HorizontalAlignment::Stretch);
  item.HorizontalContentAlignment(HorizontalAlignment::Left);
  item.Padding(Thickness{0, 8, 0, 8});
  item.Content(text);
  item.Click([url = entry.url](auto&&, auto&&) {
    CloseHistory();
    RheniumEngineNavigate(url.c_str());
  });
  item.RightTapped([time = entry.time, url = entry.url](
                       Windows::Foundation::IInspectable const& sender, auto&&) {
    MenuFlyoutItem remove;
    remove.Text(Tr(L"Supprimer", L"Delete"));
    remove.Click([time, url](auto&&, auto&&) {
      RemoveVisit(time, url);
      RebuildHistory();
    });
    MenuFlyout menu;
    menu.Items().Append(remove);
    menu.ShowAt(sender.as<FrameworkElement>());
  });
  return item;
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
      TextBlock title = Label(DayLabel(day).c_str(), 13, AccentColor());
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
        UIElement(shell.dismiss_layer), UIElement(shell.menu),
        UIElement(shell.tabs), UIElement(shell.history),
        UIElement(shell.settings)}) {
    layout.Children().Append(child);
  }
  for (Grid const& panel : {shell.tabs, shell.history, shell.settings})
    Grid::SetRowSpan(panel, 2);

  shell.root = Border();
  shell.root.Background(Brush(kBarColor));
  shell.root.RequestedTheme(ElementTheme::Dark);
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
  replace(shell.settings, BuildSettings());
  for (Grid const& panel : {shell.history, shell.settings})
    Grid::SetRowSpan(panel, 2);
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

// Bouton retour du téléphone : panneau, menu, puis le moteur (page
// d'extension, précédent, onglet ouvert par un lien) ; sinon le système
// ferme l'app.
bool HandleBack() {
  ShellState& shell = *g_shell;
  if (SettingsOpen())
    CloseSettings();
  else if (IsShown(shell.history))
    CloseHistory();
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
            RheniumEngineSetVisible(args.Visible() ? 1 : 0);
          });
      DataTransferManager::GetForCurrentView().DataRequested(ShareCurrentPage);
      try {
        StyleStatusBar();
      } catch (...) {
      }
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
