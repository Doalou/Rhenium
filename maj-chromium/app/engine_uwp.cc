// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : le moteur (content_shell en single-process) vu par la coquille
// XAML, à travers l'API C de rhenium_engine.h. RheniumEngineRun lance
// ContentMain sur le thread UI ; la boucle du thread UI de content fait
// tourner le CoreDispatcher (base/message_loop/message_pump_uwp.h), donc
// aussi XAML. Le moteur s'affiche dans le SwapChainPanel de la coquille
// (ui/gfx/win/uwp_core_window.h) et en reçoit les pointeurs.
//
// Ligne de commande : celle de l'app est vide. Les options par défaut sont
// complétées par LocalState\rhenium-args.txt (une option ou une URL par ligne),
// pour essayer sans réempaqueter. Journal : LocalState\content_shell.log,
// précédé du journal de l'app (LocalState\rhenium.log), recopiés dans
// Phone\Downloads (log_export_uwp.h).

#include "rhenium/rhenium_engine.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>

#include "base/barrier_closure.h"
#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/callback_helpers.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/strings/escape.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "components/prefs/pref_service.h"
#include "components/user_prefs/user_prefs.h"
#include "components/version_info/version_info.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_uwp_host.h"
#include "extensions/common/constants.h"
#include "extensions/rhenium/rhenium_downloads.h"
#include "extensions/rhenium/rhenium_extension_manager.h"
#include "extensions/rhenium/rhenium_main_delegate.h"
#include "extensions/rhenium/rhenium_pdf.h"
#include "extensions/rhenium/rhenium_tab_model.h"
#include "rhenium/log_export_uwp.h"
#include "sandbox/win/src/sandbox_types.h"
#include "services/network/public/mojom/clear_data_filter.mojom.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/network_context.mojom.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "ui/aura/window.h"
#include "ui/display/display.h"
#include "ui/display/display_list.h"
#include "ui/display/screen_base.h"
#include "ui/events/types/event_type.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/gfx/win/uwp_core_window.h"
#include "ui/platform_window/uwp/uwp_window.h"
#include "url/gurl.h"

namespace {

// Recherche quand le texte de la barre n'est pas une adresse : préfixe auquel
// s'ajoute le texte, choisi dans les paramètres (RheniumEngineSetSearchUrl).
constexpr char kDefaultSearchUrl[] = "https://duckduckgo.com/?q=";

std::string& SearchUrl() {
  static base::NoDestructor<std::string> url(kDefaultSearchUrl);
  return *url;
}

// Échelle de rendu au plus : au-delà, la rastérisation logicielle coûte plus
// qu'elle n'apporte à l'œil sur un écran de téléphone. La swap chain est
// agrandie à l'échelle de l'écran par la composition.
// --rhenium-render-scale=<n> la fixe (4 : définition native du Lumia 950).
constexpr float kMaxRenderScale = 3.0f;

GURL UrlFromText(const std::u16string& raw) {
  const std::string text =
      base::UTF16ToUTF8(base::TrimWhitespace(raw, base::TRIM_ALL));
  if (text.empty())
    return GURL();
  const GURL direct(text);
  if (direct.is_valid() &&
      (direct.SchemeIsHTTPOrHTTPS() || direct.SchemeIs("data") ||
       direct.SchemeIs("about") || direct.SchemeIs("file"))) {
    return direct;
  }
  // Un mot sans espace qui contient un point ressemble à une adresse.
  if (text.find(' ') == std::string::npos &&
      (text.find('.') != std::string::npos ||
       base::StartsWith(text, "localhost"))) {
    const GURL guessed("https://" + text);
    if (guessed.is_valid())
      return guessed;
  }
  return GURL(SearchUrl() +
              base::EscapeQueryParamValue(text, /*use_plus=*/true));
}

base::FilePath LocalState() {
  base::FilePath local_state;
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_state);
  return local_state;
}

void BuildCommandLine(const RheniumHost& host) {
  base::CommandLine::Init(0, nullptr);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();
  const base::FilePath local_state = LocalState();

  // Rendu, réseau et GPU en threads, compositing logiciel sans GL, présenté
  // dans le SwapChainPanel de la coquille, qui fixe la taille de la fenêtre
  // de content_shell.
  command_line->AppendSwitch("single-process");
  command_line->AppendSwitch("no-sandbox");
  // Téléphone : meta viewport, agent utilisateur « Mobile », barres de
  // défilement superposées.
  command_line->AppendSwitch("enable-viewport");
  command_line->AppendSwitch("use-mobile-user-agent");
  command_line->AppendSwitchASCII("enable-features", "OverlayScrollbar");
  // Sans cela, la page du Chrome Web Store reçoit chrome.management, dont
  // Rhénium n'a pas le délégué (plantage dans management_api.cc).
  command_line->AppendSwitchASCII("disable-features", "NewWebstoreDomain");
  command_line->AppendSwitchASCII(
      "content-shell-host-window-size",
      base::StringPrintf("%dx%d", static_cast<int>(host.width_dip),
                         static_cast<int>(host.height_dip)));
  command_line->AppendSwitch("disable-gpu");
  command_line->AppendSwitch("disable-gpu-compositing");
  command_line->AppendSwitchPath("log-file",
                                 local_state.AppendASCII("content_shell.log"));
  command_line->AppendSwitchPath("data-path", local_state.AppendASCII("data"));
  // Pas --lang, qui changerait aussi les .pak chargés.
  if (host.language && *host.language) {
    command_line->AppendSwitchASCII("rhenium-language",
                                    base::WideToASCII(host.language));
  }

  bool has_url = false;
  std::string extra;
  if (base::ReadFileToString(local_state.AppendASCII("rhenium-args.txt"),
                             &extra)) {
    if (base::StartsWith(extra, "\xEF\xBB\xBF"))  // BOM du Bloc-notes
      extra.erase(0, 3);
    for (const auto& line :
         base::SplitString(extra, "\r\n", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_NONEMPTY)) {
      if (base::StartsWith(line, "--")) {
        std::string name = line.substr(2);
        std::string value;
        size_t equals = name.find('=');
        if (equals != std::string::npos) {
          value = name.substr(equals + 1);
          name.resize(equals);
        }
        command_line->AppendSwitchASCII(name, value);
      } else {
        command_line->AppendArg(line);
        has_url = true;
      }
    }
  }
  if (!has_url && host.start_url)
    command_line->AppendArg(base::WideToUTF8(host.start_url));
  rhenium::Note("ligne de commande : %s",
                base::WideToUTF8(command_line->GetCommandLineString()).c_str());
}

float RenderScale(float screen_scale) {
  double forced = 0;
  if (base::StringToDouble(
          base::CommandLine::ForCurrentProcess()->GetSwitchValueASCII(
              "rhenium-render-scale"),
          &forced) &&
      forced >= 1) {
    return static_cast<float>(forced);
  }
  return std::min(screen_scale, kMaxRenderScale);
}

gfx::Size ToPixels(float width_dip, float height_dip, float scale) {
  return gfx::Size(static_cast<int>(std::lround(width_dip * scale)),
                   static_cast<int>(std::lround(height_dip * scale)));
}

// Rappel de la coquille pour la fin d'une installation d'extension.
void (*g_extension_installed)(void* context, int ok, const wchar_t* text) =
    nullptr;

// La page d'extension, si elle est ouverte, sinon l'onglet actif.
content::WebContents* GetTopWebContents() {
  if (content::WebContents* overlay = rhenium::GetOverlayWebContents())
    return overlay;
  return rhenium::GetActiveWebContents();
}

}  // namespace

extern "C" void RheniumEngineStartLog() {
  const base::FilePath local_state = LocalState();
  if (local_state.empty())
    return;  // ApplicationData demande un apartment COM
  rhenium::StartLogExport(local_state.AppendASCII("rhenium.log").value(),
                          local_state.AppendASCII("content_shell.log").value());
}

extern "C" void RheniumEngineNote(const wchar_t* text) {
  rhenium::Note("%s", base::WideToUTF8(text ? text : L"").c_str());
  rhenium::FlushNow();
}

extern "C" int RheniumEngineRun(const RheniumHost* host) {
  RheniumEngineStartLog();  // sans effet si la coquille l'a déjà fait
  BuildCommandLine(*host);
  const float scale = RenderScale(host->scale > 0 ? host->scale : 1.0f);
  rhenium::Note("échelle de rendu %.2f (écran %.2f)", scale, host->scale);
  // Ce que V8 lit pour choisir ses instructions (v8/src/base/cpu.cc).
  rhenium::Note(
      "processeur : division %d, NEON %d, VFP 32 registres %d, ARMv8 %d",
      IsProcessorFeaturePresent(PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE),
      IsProcessorFeaturePresent(PF_ARM_NEON_INSTRUCTIONS_AVAILABLE),
      IsProcessorFeaturePresent(PF_ARM_VFP_32_REGISTERS_AVAILABLE),
      IsProcessorFeaturePresent(PF_ARM_V8_INSTRUCTIONS_AVAILABLE));
  gfx::uwp::SetHostedContentArea(
      ToPixels(host->width_dip, host->height_dip, scale), scale,
      host->attach_swap_chain, host->context);
  content::ShellUwpHost shell_host;
  shell_host.context = host->context;
  shell_host.url_changed = host->url_changed;
  shell_host.loading_changed = host->loading_changed;
  shell_host.title_changed = host->title_changed;
  shell_host.progress_changed = host->progress_changed;
  shell_host.displayed_url = &rhenium::PdfSourceUrl;
  content::SetShellUwpHost(shell_host);

  // Pas de broker : en single-process, aucun processus n'est lancé.
  sandbox::SandboxInterfaceInfo sandbox_info = {nullptr};
  rhenium::TabObserver tabs;
  tabs.context = host->context;
  tabs.tabs_changed = host->tabs_changed;
  tabs.page_visited = host->page_visited;
  tabs.tab_thumbnail = host->tab_thumbnail;
  rhenium::InitTabs(tabs);
  rhenium::ExtensionObserver extensions;
  extensions.context = host->context;
  extensions.install_done = [](void* context, bool ok, const wchar_t* text) {
    if (g_extension_installed)
      g_extension_installed(context, ok ? 1 : 0, text);
  };
  g_extension_installed = host->extension_installed;
  rhenium::InitExtensions(extensions);
  rhenium::DownloadObserver downloads;
  downloads.context = host->context;
  downloads.download_changed = host->download_changed;
  rhenium::InitDownloads(downloads);
  rhenium::RheniumMainDelegate delegate;
  content::ContentMainParams params(&delegate);
  params.instance = GetModuleHandle(nullptr);
  params.sandbox_info = &sandbox_info;
  rhenium::Note("ContentMain");
  const int exit_code = content::ContentMain(std::move(params));
  rhenium::Note("ContentMain a rendu %d", exit_code);
  return exit_code;
}

extern "C" void RheniumEngineNavigate(const wchar_t* text) {
  content::Shell* shell = rhenium::GetActiveShell();
  const GURL url = UrlFromText(base::WideToUTF16(text ? text : L""));
  if (shell && url.is_valid()) {
    rhenium::CloseOverlay();
    shell->LoadURL(url);
  }
}

extern "C" int RheniumEngineGoBack() {
  return rhenium::GoBack() ? 1 : 0;
}

extern "C" void RheniumEngineGoForward() {
  if (content::Shell* shell = rhenium::GetActiveShell())
    shell->GoBackOrForward(1);
}

extern "C" void RheniumEngineReload() {
  if (content::Shell* shell = rhenium::GetActiveShell())
    shell->Reload();
}

extern "C" void RheniumEngineStop() {
  if (content::Shell* shell = rhenium::GetActiveShell())
    shell->Stop();
}

extern "C" int RheniumEngineNewTab(const wchar_t* text) {
  return rhenium::OpenTab(UrlFromText(base::WideToUTF16(text ? text : L"")))
             ? 1
             : 0;
}

extern "C" int RheniumEngineRestoreTab(const wchar_t* url,
                                       const wchar_t* title) {
  return rhenium::RestoreTab(GURL(base::WideToUTF8(url ? url : L"")),
                             base::WideToUTF16(title ? title : L""))
             ? 1
             : 0;
}

extern "C" void RheniumEngineActivateTab(int id) {
  rhenium::ActivateTab(id);
}

extern "C" int RheniumEngineCloseTab(int id) {
  return rhenium::CloseTab(id) ? 1 : 0;
}

extern "C" void RheniumEngineListTabs(void (*visit)(void* context,
                                                    int id,
                                                    int active,
                                                    const wchar_t* title,
                                                    const wchar_t* url),
                                      void* context) {
  rhenium::ForEachTab(visit, context);
}

extern "C" void RheniumEngineCaptureTab() {
  rhenium::CaptureActiveTab();
}

extern "C" int RheniumEngineDiscardTab() {
  base::MemoryPressureListener::NotifyMemoryPressure(
      base::MEMORY_PRESSURE_LEVEL_CRITICAL);
  return rhenium::DiscardTab() ? 1 : 0;
}

extern "C" void RheniumEngineSetDesktopSites(int desktop) {
  rhenium::SetDesktopSites(desktop != 0);
}

extern "C" int RheniumEngineIsTabDesktop() {
  return rhenium::IsActiveTabDesktop() ? 1 : 0;
}

extern "C" void RheniumEngineSetTabDesktop(int desktop) {
  rhenium::SetActiveTabDesktop(desktop != 0);
}

extern "C" void RheniumEngineSetDarkMode(int dark, int force_dark) {
  rhenium::SetDarkMode(dark != 0, force_dark != 0);
}

extern "C" void RheniumEnginePointer(int kind,
                                     unsigned int id,
                                     float x_dip,
                                     float y_dip,
                                     int is_mouse) {
  ui::EventType type = ui::EventType::kTouchMoved;
  switch (kind) {
    case RHENIUM_POINTER_PRESSED:
      type = ui::EventType::kTouchPressed;
      break;
    case RHENIUM_POINTER_RELEASED:
      type = ui::EventType::kTouchReleased;
      break;
    case RHENIUM_POINTER_CANCELED:
      type = ui::EventType::kTouchCancelled;
      break;
    default:
      break;
  }
  ui::DispatchHostedPointer(type, id, x_dip, y_dip, is_mouse != 0);
}

extern "C" void RheniumEngineSetWebFocus(int focused) {
  ui::SetHostedWebFocus(focused != 0);
  content::WebContents* web_contents = GetTopWebContents();
  if (focused && web_contents)
    web_contents->Focus();
}

extern "C" int RheniumEngineOpenExtensionPopup() {
  return rhenium::OpenActionPopup() ? 1 : 0;
}

extern "C" int RheniumEngineOpenExtensionOptions() {
  return rhenium::OpenOptionsPage() ? 1 : 0;
}

extern "C" void RheniumEngineListExtensions(
    void (*visit)(void* context,
                  const char* id,
                  const wchar_t* name,
                  const wchar_t* version,
                  int enabled,
                  int removable,
                  int has_popup,
                  int has_options),
    void* context) {
  rhenium::ForEachExtension(visit, context);
}

extern "C" int RheniumEngineSetExtensionEnabled(const char* id, int enabled) {
  return rhenium::SetExtensionEnabled(id ? id : "", enabled != 0) ? 1 : 0;
}

extern "C" int RheniumEngineUninstallExtension(const char* id) {
  return rhenium::UninstallExtension(id ? id : "") ? 1 : 0;
}

extern "C" int RheniumEngineOpenExtensionPopupFor(const char* id) {
  return rhenium::OpenExtensionPopup(id ? id : "") ? 1 : 0;
}

extern "C" int RheniumEngineOpenExtensionOptionsFor(const char* id) {
  return rhenium::OpenExtensionOptions(id ? id : "") ? 1 : 0;
}

extern "C" int RheniumEngineWebStoreId(const wchar_t* url, char id[33]) {
  const std::string found =
      rhenium::WebStoreExtensionId(GURL(base::WideToUTF16(url ? url : L"")));
  if (found.size() != 32)
    return 0;
  std::copy(found.begin(), found.end(), id);
  id[32] = 0;
  return 1;
}

extern "C" void RheniumEngineCancelDownload(int id) {
  rhenium::CancelDownload(id);
}

extern "C" int RheniumEngineInstallExtension(const char* id) {
  return rhenium::InstallFromWebStore(id ? id : "") ? 1 : 0;
}

// Appelée avant ContentMain : pas de GURL ici, sa première utilisation fige
// les schémas que content enregistre ensuite (CHECK dans url_util.cc).
extern "C" void RheniumEngineSetSearchUrl(const wchar_t* prefix) {
  const std::string url = base::WideToUTF8(prefix ? prefix : L"");
  SearchUrl() = base::StartsWith(url, "https://") ||
                        base::StartsWith(url, "http://")
                    ? url
                    : kDefaultSearchUrl;
}

extern "C" const wchar_t* RheniumEngineChromiumVersion() {
  static const base::NoDestructor<std::wstring> version(
      base::ASCIIToWide(version_info::GetVersionNumber()));
  return version->c_str();
}

extern "C" void RheniumEngineResize(float width_dip, float height_dip) {
  if (!gfx::uwp::IsHosted())  // moteur pas encore lancé : il lira la taille
    return;
  const float scale = gfx::uwp::GetHostedScale();
  const gfx::Size pixels = ToPixels(width_dip, height_dip, scale);
  rhenium::Note("zone de contenu %.0fx%.0f DIP", width_dip, height_dip);
  gfx::uwp::UpdateHostedSize(pixels);
  if (auto* screen = static_cast<display::ScreenBase*>(display::Screen::Get())) {
    display::Display display = screen->GetPrimaryDisplay();
    display.SetScaleAndBounds(scale, gfx::Rect(pixels));
    screen->display_list().UpdateDisplay(display);
  }
  ui::ResizeHostedWindow();
  // La fenêtre de la page suit celle de l'hôte (FillLayout ne la règle
  // qu'une fois).
  const gfx::Rect bounds(gfx::Size(static_cast<int>(std::lround(width_dip)),
                                   static_cast<int>(std::lround(height_dip))));
  for (content::Shell* shell : content::Shell::windows())
    shell->web_contents()->GetNativeView()->SetBounds(bounds);
}

extern "C" void RheniumEngineSetKeyboardInset(float bottom_dip) {
  content::WebContents* web_contents = GetTopWebContents();
  if (!web_contents)
    return;
  if (content::RenderWidgetHostView* view =
          web_contents->GetRenderWidgetHostView()) {
    view->SetInsets(gfx::Insets::TLBR(
        0, 0, static_cast<int>(std::lround(std::max(0.0f, bottom_dip))), 0));
  }
}

extern "C" void RheniumEngineSetVisible(int visible) {
  rhenium::Note(visible ? "app visible" : "app masquée");
  rhenium::SetAppVisible(visible != 0);
  // Une app masquée peut être tuée sans arrêt propre : les préférences
  // (règles et permissions des extensions) sont écrites tout de suite.
  content::WebContents* web_contents = rhenium::GetActiveWebContents();
  if (web_contents && !visible) {
    content::BrowserContext* context = web_contents->GetBrowserContext();
    user_prefs::UserPrefs::Get(context)->CommitPendingWrite();
    // De même pour les cookies (écrits par lots toutes les 30 s) et le
    // stockage des sites (localStorage).
    content::StoragePartition* partition =
        context->GetDefaultStoragePartition();
    partition->GetCookieManagerForBrowserProcess()->FlushCookieStore(
        base::DoNothing());
    partition->Flush();
  }
}

extern "C" void RheniumEngineClearBrowsingData(void (*done)(void* context),
                                               void* context) {
  content::WebContents* web_contents = rhenium::GetActiveWebContents();
  if (!web_contents) {
    if (done)
      done(context);
    return;
  }
  rhenium::Note("effacement des données de navigation");
  base::RepeatingClosure cleared = base::BarrierClosure(
      2, base::BindOnce(
             [](void (*done)(void*), void* context) {
               rhenium::Note("données de navigation effacées");
               if (done)
                 done(context);
             },
             done, context));
  content::StoragePartition* partition =
      web_contents->GetBrowserContext()->GetDefaultStoragePartition();
  // Tout ce que les sites ont stocké, sauf ce qui appartient aux extensions
  // (service worker et stockage de uBO Lite).
  partition->ClearData(
      content::StoragePartition::REMOVE_DATA_MASK_COOKIES |
          content::StoragePartition::REMOVE_DATA_MASK_FILE_SYSTEMS |
          content::StoragePartition::REMOVE_DATA_MASK_INDEXEDDB |
          content::StoragePartition::REMOVE_DATA_MASK_LOCAL_STORAGE |
          content::StoragePartition::REMOVE_DATA_MASK_SERVICE_WORKERS |
          content::StoragePartition::REMOVE_DATA_MASK_CACHE_STORAGE,
      /*filter_builder=*/nullptr,
      base::BindRepeating(
          [](const blink::StorageKey& key, storage::SpecialStoragePolicy*) {
            return key.origin().scheme() != extensions::kExtensionScheme;
          }),
      network::mojom::CookieDeletionFilter::New(),
      /*perform_storage_cleanup=*/true, base::Time(), base::Time::Max(),
      cleared);
  partition->GetNetworkContext()->ClearHttpCache(base::Time(),
                                                 base::Time::Max(),
                                                 /*filter=*/nullptr, cleared);
}
