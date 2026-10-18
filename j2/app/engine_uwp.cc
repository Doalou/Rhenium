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

#include <cmath>
#include <string>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/escape.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/app/content_main.h"
#include "content/public/browser/web_contents.h"
#include "content/shell/app/shell_main_delegate.h"
#include "content/shell/browser/shell.h"
#include "content/shell/browser/shell_uwp_host.h"
#include "rhenium/log_export_uwp.h"
#include "sandbox/win/src/sandbox_types.h"
#include "ui/events/types/event_type.h"
#include "ui/gfx/win/uwp_core_window.h"
#include "ui/platform_window/uwp/uwp_window.h"
#include "url/gurl.h"

namespace {

// Recherche quand le texte de la barre n'est pas une adresse.
constexpr char kSearchUrl[] = "https://duckduckgo.com/?q=";

content::Shell* GetShell() {
  auto& windows = content::Shell::windows();
  return windows.empty() ? nullptr : windows.front();
}

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
  return GURL(kSearchUrl + base::EscapeQueryParamValue(text, /*use_plus=*/true));
}

void BuildCommandLine(const RheniumHost& host) {
  base::CommandLine::Init(0, nullptr);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  base::FilePath local_state;
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_state);

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
  command_line->AppendSwitchASCII(
      "content-shell-host-window-size",
      base::StringPrintf("%dx%d", static_cast<int>(host.width_dip),
                         static_cast<int>(host.height_dip)));
  command_line->AppendSwitch("disable-gpu");
  command_line->AppendSwitch("disable-software-rasterizer");
  command_line->AppendSwitch("disable-gpu-compositing");
  command_line->AppendSwitch("disable-legacy-window");
  const base::FilePath log_file = local_state.AppendASCII("content_shell.log");
  command_line->AppendSwitchPath("log-file", log_file);
  rhenium::StartLogExport(local_state.AppendASCII("rhenium.log").value(),
                          log_file.value());
  command_line->AppendSwitchPath("data-path", local_state.AppendASCII("data"));

  bool has_url = false;
  std::string extra;
  if (base::ReadFileToString(local_state.AppendASCII("rhenium-args.txt"),
                             &extra)) {
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

base::FilePath LocalState() {
  base::FilePath local_state;
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_state);
  return local_state;
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
  const float scale = host->scale > 0 ? host->scale : 1.0f;
  gfx::uwp::SetHostedContentArea(
      gfx::Size(static_cast<int>(std::lround(host->width_dip * scale)),
                static_cast<int>(std::lround(host->height_dip * scale))),
      scale, host->attach_swap_chain, host->context);
  content::ShellUwpHost shell_host;
  shell_host.context = host->context;
  shell_host.url_changed = host->url_changed;
  shell_host.loading_changed = host->loading_changed;
  shell_host.title_changed = host->title_changed;
  shell_host.progress_changed = host->progress_changed;
  content::SetShellUwpHost(shell_host);

  BuildCommandLine(*host);
  // Pas de broker : en single-process, aucun processus n'est lancé.
  sandbox::SandboxInterfaceInfo sandbox_info = {nullptr};
  content::ShellMainDelegate delegate;
  content::ContentMainParams params(&delegate);
  params.instance = GetModuleHandle(nullptr);
  params.sandbox_info = &sandbox_info;
  rhenium::Note("ContentMain");
  const int exit_code = content::ContentMain(std::move(params));
  rhenium::Note("ContentMain a rendu %d", exit_code);
  return exit_code;
}

extern "C" void RheniumEngineNavigate(const wchar_t* text) {
  content::Shell* shell = GetShell();
  const GURL url = UrlFromText(base::WideToUTF16(text ? text : L""));
  if (shell && url.is_valid())
    shell->LoadURL(url);
}

extern "C" void RheniumEngineGoBack() {
  if (content::Shell* shell = GetShell())
    shell->GoBackOrForward(-1);
}

extern "C" void RheniumEngineGoForward() {
  if (content::Shell* shell = GetShell())
    shell->GoBackOrForward(1);
}

extern "C" void RheniumEngineReload() {
  if (content::Shell* shell = GetShell())
    shell->Reload();
}

extern "C" void RheniumEngineStop() {
  if (content::Shell* shell = GetShell())
    shell->Stop();
}

extern "C" void RheniumEnginePointer(int kind,
                                     unsigned int id,
                                     float x_dip,
                                     float y_dip,
                                     int is_mouse) {
  ui::EventType type = ui::ET_TOUCH_MOVED;
  switch (kind) {
    case RHENIUM_POINTER_PRESSED:
      type = ui::ET_TOUCH_PRESSED;
      break;
    case RHENIUM_POINTER_RELEASED:
      type = ui::ET_TOUCH_RELEASED;
      break;
    case RHENIUM_POINTER_CANCELED:
      type = ui::ET_TOUCH_CANCELLED;
      break;
    default:
      break;
  }
  ui::DispatchHostedPointer(type, id, x_dip, y_dip, is_mouse != 0);
}

extern "C" void RheniumEngineSetWebFocus(int focused) {
  ui::SetHostedWebFocus(focused != 0);
  content::Shell* shell = GetShell();
  if (focused && shell)
    shell->web_contents()->Focus();
}
