// Copyright 2026 Doalo
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Rhénium : coquille XAML de l'app (C++/WinRT, compilée par MSVC avec
// exceptions). Le moteur (Chromium, clang, sans exceptions) s'affiche dans un
// SwapChainPanel et se commande par l'API C de rhenium_engine.h. Aucune
// exception ne doit remonter vers le moteur : tout rappel venu de lui est
// enveloppé.
//
// Interface construite en code, sans fichier .xaml : la page, et en bas une
// barre comme celle d'Edge sur mobile (précédent, adresse, recharger ou
// arrêter, fil de progression). Le tout tient dans la zone visible, entre la
// barre d'état et les boutons à l'écran du téléphone.

#include <windows.h>

#include <dxgi.h>
#include <stdio.h>
#include <unknwn.h>

#include <string>
#include <windows.ui.xaml.media.dxinterop.h>

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.Devices.Input.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Input.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

#include "rhenium_engine.h"

using namespace winrt;
using namespace Windows::UI::Xaml;
using namespace Windows::UI::Xaml::Controls;
using namespace Windows::UI::Xaml::Input;
using namespace Windows::UI::Xaml::Media;
using Windows::UI::Color;
using Windows::UI::ColorHelper;
using Windows::UI::Core::CoreDispatcher;
using Windows::UI::Core::CoreDispatcherPriority;
using Windows::UI::ViewManagement::ApplicationView;
using Windows::UI::ViewManagement::InputPane;
using Windows::UI::ViewManagement::InputPaneVisibilityEventArgs;

namespace {

constexpr wchar_t kStartUrl[] = L"https://fr.m.wikipedia.org/";
constexpr wchar_t kGlyphBack[] = L"";
constexpr wchar_t kGlyphReload[] = L"";
constexpr wchar_t kGlyphStop[] = L"";

constexpr Color kBarColor{0xFF, 0x1F, 0x1F, 0x1F};
constexpr Color kFieldColor{0xFF, 0x33, 0x33, 0x33};
constexpr Color kFieldFocusedColor{0xFF, 0xF2, 0xF2, 0xF2};
constexpr Color kTextColor{0xFF, 0xE6, 0xE6, 0xE6};
constexpr Color kDimTextColor{0xFF, 0x99, 0x99, 0x99};
constexpr double kBarHeight = 48;

struct ShellState {
  Border root{nullptr};
  SwapChainPanel panel{nullptr};
  Grid input_surface{nullptr};  // reçoit les pointeurs au-dessus du panneau
  TextBox address{nullptr};
  Button back{nullptr};
  Button reload{nullptr};
  FontIcon reload_icon{nullptr};
  ContentControl focus_sink{nullptr};  // garde le focus XAML pendant la page
  ProgressBar progress{nullptr};
  CoreDispatcher dispatcher{nullptr};
  RheniumHost host{};
  std::wstring url;
  bool engine_started = false;
  bool loading = false;
  bool can_go_back = false;
};

ShellState* g_shell = nullptr;  // une seule fenêtre, pour toute la vie de l'app

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

Button BarButton(FontIcon const& icon) {
  Button button;
  button.Content(icon);
  button.Width(kBarHeight);
  button.Height(kBarHeight);
  button.Padding(Thickness{0, 0, 0, 0});
  button.Background(Brush(Windows::UI::Colors::Transparent()));
  button.BorderThickness(Thickness{0, 0, 0, 0});
  return button;
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

// La page et la barre tiennent dans la zone visible ; autour, la couleur de la
// barre passe sous la barre d'état et les boutons du téléphone.
void FitVisibleBounds() {
  auto bounds = Window::Current().Bounds();
  auto visible = ApplicationView::GetForCurrentView().VisibleBounds();
  const Thickness insets{
      visible.X - bounds.X, visible.Y - bounds.Y,
      (bounds.X + bounds.Width) - (visible.X + visible.Width),
      (bounds.Y + bounds.Height) - (visible.Y + visible.Height)};
  g_shell->root.Padding(insets);
  wchar_t line[128];
  swprintf_s(line, L"coquille : zone visible %.0fx%.0f, marges %.0f %.0f",
             visible.Width, visible.Height, insets.Top, insets.Bottom);
  RheniumEngineNote(line);
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

// Rappels du moteur. Aucune exception ne doit en sortir.

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

void OnLoadingChanged(void*, int loading, int can_go_back, int) noexcept {
  try {
    ShellState& shell = *g_shell;
    if (loading && !shell.loading)
      shell.progress.Value(0.05);
    shell.loading = loading != 0;
    shell.can_go_back = can_go_back != 0;
    shell.back.IsEnabled(shell.can_go_back);
    shell.reload_icon.Glyph(shell.loading ? kGlyphStop : kGlyphReload);
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

void OnTitleChanged(void*, wchar_t const*) noexcept {}

// Lance le moteur une fois la taille du panneau connue. RheniumEngineRun ne
// rend la main qu'à la fermeture : la boucle de Chromium fait tourner le
// CoreDispatcher, donc XAML continue de vivre.
void StartEngine() {
  ShellState& shell = *g_shell;
  shell.engine_started = true;
  shell.host.context = &shell;
  shell.host.width_dip = static_cast<float>(shell.panel.ActualWidth());
  shell.host.height_dip = static_cast<float>(shell.panel.ActualHeight());
  shell.host.scale = shell.panel.CompositionScaleX();
  shell.host.start_url = kStartUrl;
  shell.host.attach_swap_chain = &OnAttachSwapChain;
  shell.host.url_changed = &OnUrlChanged;
  shell.host.loading_changed = &OnLoadingChanged;
  shell.host.title_changed = &OnTitleChanged;
  shell.host.progress_changed = &OnProgressChanged;
  shell.url = kStartUrl;
  ShowAddress();
  Note(L"coquille : lancement du moteur, hauteur en DIP",
       static_cast<unsigned>(shell.host.height_dip));
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

UIElement BuildUi() {
  ShellState& shell = *g_shell;

  shell.panel = SwapChainPanel();
  // Le SwapChainPanel refuse un fond, et sans fond une grille ne reçoit pas
  // les pointeurs : une grille transparente de même taille les prend.
  shell.input_surface = Grid();
  shell.input_surface.Background(Brush(Windows::UI::Colors::Transparent()));
  shell.panel.SizeChanged([](auto&&, auto&&) {
    ShellState& shell = *g_shell;
    if (shell.panel.ActualWidth() <= 0)
      return;
    if (!shell.engine_started) {
      StartEngine();
    } else {
      // Lue par RheniumEngineRun si le moteur n'a pas encore démarré.
      shell.host.width_dip = static_cast<float>(shell.panel.ActualWidth());
      shell.host.height_dip = static_cast<float>(shell.panel.ActualHeight());
      RheniumEngineResize(shell.host.width_dip, shell.host.height_dip);
    }
  });
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

  shell.back = BarButton(Glyph(kGlyphBack));
  shell.back.IsEnabled(false);
  shell.back.Click([](auto&&, auto&&) { RheniumEngineGoBack(); });

  shell.reload_icon = Glyph(kGlyphReload);
  shell.reload = BarButton(shell.reload_icon);
  shell.reload.Click([](auto&&, auto&&) {
    if (g_shell->loading)
      RheniumEngineStop();
    else
      RheniumEngineReload();
  });

  shell.address = TextBox();
  StyleAddress(shell.address);
  shell.address.PlaceholderText(L"Adresse ou recherche");
  shell.address.IsSpellCheckEnabled(false);
  shell.address.IsTextPredictionEnabled(false);
  shell.address.VerticalAlignment(VerticalAlignment::Center);
  shell.address.Margin(Thickness{0, 6, 0, 6});
  InputScope scope;
  InputScopeName url_scope;
  url_scope.NameValue(InputScopeNameValue::Url);
  scope.Names().Append(url_scope);
  shell.address.InputScope(scope);
  shell.address.GotFocus([](auto&&, auto&&) {
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

  Grid bar;
  bar.Height(kBarHeight);
  for (GridLength width : {GridLength{1, GridUnitType::Auto},
                           GridLength{1, GridUnitType::Star},
                           GridLength{1, GridUnitType::Auto}}) {
    ColumnDefinition column;
    column.Width(width);
    bar.ColumnDefinitions().Append(column);
  }
  Grid::SetColumn(shell.back, 0);
  Grid::SetColumn(shell.address, 1);
  Grid::SetColumn(shell.reload, 2);
  bar.Children().Append(shell.back);
  bar.Children().Append(shell.address);
  bar.Children().Append(shell.reload);

  // Fil de progression en haut de la barre.
  shell.progress = ProgressBar();
  shell.progress.Minimum(0);
  shell.progress.Maximum(1);
  shell.progress.Height(2);
  shell.progress.MinHeight(2);
  shell.progress.Background(Brush(Windows::UI::Colors::Transparent()));
  shell.progress.VerticalAlignment(VerticalAlignment::Top);
  shell.progress.Visibility(Visibility::Collapsed);
  Grid::SetColumnSpan(shell.progress, 3);
  bar.Children().Append(shell.progress);

  Grid layout;
  RowDefinition content_row;
  content_row.Height(GridLength{1, GridUnitType::Star});
  RowDefinition bar_row;
  bar_row.Height(GridLength{1, GridUnitType::Auto});
  layout.RowDefinitions().Append(content_row);
  layout.RowDefinitions().Append(bar_row);
  shell.focus_sink = ContentControl();
  shell.focus_sink.IsTabStop(true);
  shell.focus_sink.UseSystemFocusVisuals(false);
  shell.focus_sink.IsHitTestVisible(false);
  shell.focus_sink.Width(1);
  shell.focus_sink.Height(1);
  shell.focus_sink.Opacity(0);
  shell.focus_sink.HorizontalAlignment(HorizontalAlignment::Left);
  shell.focus_sink.VerticalAlignment(VerticalAlignment::Top);

  Grid::SetRow(shell.focus_sink, 0);
  Grid::SetRow(shell.panel, 0);
  Grid::SetRow(shell.input_surface, 0);
  Grid::SetRow(bar, 1);
  layout.Children().Append(shell.focus_sink);
  layout.Children().Append(shell.panel);
  layout.Children().Append(shell.input_surface);
  layout.Children().Append(bar);

  shell.root = Border();
  shell.root.Background(Brush(kBarColor));
  shell.root.RequestedTheme(ElementTheme::Dark);
  shell.root.Child(layout);
  return shell.root;
}

struct App : ApplicationT<App> {
  App() {
    RheniumEngineNote(L"coquille : App construite");
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
            if (g_shell->engine_started)
              RheniumEngineSetVisible(args.Visible() ? 1 : 0);
          });
      try {
        StyleStatusBar();
      } catch (...) {
      }
      // Bouton retour du téléphone : précédent dans l'historique, sinon le
      // système ferme l'app.
      Windows::UI::Core::SystemNavigationManager::GetForCurrentView()
          .BackRequested([](auto&&, auto const& args) {
            if (g_shell->can_go_back) {
              args.Handled(true);
              RheniumEngineGoBack();
            }
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
