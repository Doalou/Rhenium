// Copyright 2026 The Rhénium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : point d'entrée UWP de content_shell. CoreApplication crée la vue ;
// sa méthode Run lance ContentMain en single-process sur le thread de la
// CoreWindow, dont le CoreDispatcher sert de boucle au thread UI de content
// (base/message_loop/message_pump_uwp.h).
//
// Ligne de commande : celle de l'app est vide. Les options par défaut sont
// complétées par LocalState\rhenium-args.txt (une option ou une URL par ligne),
// pour essayer sans réempaqueter. Journal : LocalState\content_shell.log,
// précédé du journal de l'app (LocalState\rhenium.log), recopiés dans
// Phone\Downloads (log_export_uwp.h).

#include <windows.h>

#include <roapi.h>
#include <windows.applicationmodel.core.h>
#include <windows.ui.core.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <string>
#include <vector>

#include "base/base_paths.h"
#include "base/command_line.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/path_service.h"
#include "base/strings/string_split.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/app/content_main.h"
#include "content/shell/app/shell_main_delegate.h"
#include "rhenium/log_export_uwp.h"
#include "sandbox/win/src/sandbox_types.h"
#include "ui/gfx/win/uwp_core_window.h"

namespace {

using ABI::Windows::ApplicationModel::Activation::IActivatedEventArgs;
using ABI::Windows::ApplicationModel::Core::CoreApplicationView;
using ABI::Windows::ApplicationModel::Core::ICoreApplication;
using ABI::Windows::ApplicationModel::Core::ICoreApplicationView;
using ABI::Windows::ApplicationModel::Core::IFrameworkView;
using ABI::Windows::ApplicationModel::Core::IFrameworkViewSource;
using ABI::Windows::Foundation::ITypedEventHandler;
using ABI::Windows::UI::Core::CoreProcessEventsOption_ProcessAllIfPresent;
using ABI::Windows::UI::Core::ICoreDispatcher;
using ABI::Windows::UI::Core::ICoreWindow;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;
using Microsoft::WRL::WinRt;
using Microsoft::WRL::Wrappers::HStringReference;

// Page de test : la console de la page est recopiée dans le journal. Étape 3,
// JavaScript ; étape 4, requête HTTPS vers l'API de Wikipédia (lisible depuis
// toute origine avec origin=*), puis ouverture de la page d'accueil.
constexpr char kDefaultUrl[] =
    "data:text/html,<title>Rhenium</title><script>"
    "console.log('Rhenium: JS ' + (6 * 7));"
    "fetch('https://fr.wikipedia.org/w/api.php?action=query%26meta=siteinfo"
    "%26format=json%26origin=*')"
    ".then(r => { console.log('Rhenium: HTTP ' + r.status); return r.json(); })"
    ".then(j => { console.log('Rhenium: site ' + j.query.general.sitename);"
    " location.href = 'https://fr.wikipedia.org/'; })"
    ".catch(e => console.log('Rhenium: erreur ' + e));"
    "</script>";

void BuildCommandLine() {
  base::CommandLine::Init(0, nullptr);
  base::CommandLine* command_line = base::CommandLine::ForCurrentProcess();

  base::FilePath local_state;
  base::PathService::Get(base::DIR_LOCAL_APP_DATA, &local_state);

  // Rendu, réseau et GPU en threads, compositing logiciel sans GL, présenté
  // dans la CoreWindow (software_output_device_uwp.h), qui fixe la taille de
  // la fenêtre de content_shell.
  command_line->AppendSwitch("single-process");
  command_line->AppendSwitch("no-sandbox");
  // Téléphone : meta viewport, agent utilisateur « Mobile », barres de
  // défilement superposées.
  command_line->AppendSwitch("enable-viewport");
  command_line->AppendSwitch("use-mobile-user-agent");
  command_line->AppendSwitchASCII("enable-features", "OverlayScrollbar");
  gfx::Size pixel_size;
  float scale = 1.0f;
  if (gfx::uwp::GetCoreWindowMetrics(&pixel_size, &scale)) {
    command_line->AppendSwitchASCII(
        "content-shell-host-window-size",
        base::StringPrintf("%dx%d",
                           static_cast<int>(pixel_size.width() / scale),
                           static_cast<int>(pixel_size.height() / scale)));
  }
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
  if (!has_url)
    command_line->AppendArg(kDefaultUrl);
  rhenium::Note("ligne de commande : %s",
                base::WideToUTF8(command_line->GetCommandLineString()).c_str());
}

int RunContentShell() {
  BuildCommandLine();
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

class ShellView
    : public RuntimeClass<RuntimeClassFlags<WinRt>, IFrameworkView> {
 public:
  // IFrameworkView :
  IFACEMETHODIMP Initialize(ICoreApplicationView* view) override {
    EventRegistrationToken token;
    return view->add_Activated(
        Callback<ITypedEventHandler<CoreApplicationView*,
                                    IActivatedEventArgs*>>(
            [this](ICoreApplicationView*, IActivatedEventArgs*) {
              return window_ ? window_->Activate() : S_OK;
            })
            .Get(),
        &token);
  }
  IFACEMETHODIMP SetWindow(ICoreWindow* window) override {
    window_ = window;
    return S_OK;
  }
  IFACEMETHODIMP Load(HSTRING) override { return S_OK; }
  IFACEMETHODIMP Run() override {
    // Activation de la fenêtre (fin de l'écran de démarrage) avant le
    // démarrage de content, qui prend plusieurs secondes sur le téléphone.
    ComPtr<ICoreDispatcher> dispatcher;
    if (SUCCEEDED(window_->get_Dispatcher(&dispatcher)))
      dispatcher->ProcessEvents(CoreProcessEventsOption_ProcessAllIfPresent);
    RunContentShell();
    return S_OK;
  }
  IFACEMETHODIMP Uninitialize() override { return S_OK; }

 private:
  ComPtr<ICoreWindow> window_;
};

class ShellViewSource
    : public RuntimeClass<RuntimeClassFlags<WinRt>, IFrameworkViewSource> {
 public:
  IFACEMETHODIMP CreateView(IFrameworkView** view) override {
    return Make<ShellView>().CopyTo(view);
  }
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
  if (FAILED(RoInitialize(RO_INIT_MULTITHREADED)))
    return 1;
  ComPtr<ICoreApplication> application;
  HRESULT hr = RoGetActivationFactory(
      HStringReference(
          RuntimeClass_Windows_ApplicationModel_Core_CoreApplication)
          .Get(),
      IID_PPV_ARGS(&application));
  if (SUCCEEDED(hr))
    hr = application->Run(Make<ShellViewSource>().Get());
  RoUninitialize();
  return SUCCEEDED(hr) ? 0 : 1;
}
