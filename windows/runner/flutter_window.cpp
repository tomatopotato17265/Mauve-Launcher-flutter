#include "flutter_window.h"

#include <flutter/standard_method_codec.h>

#include <optional>
#include <variant>

#include "flutter/generated_plugin_registrant.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project)
    : project_(project) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());

  window_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          flutter_controller_->engine()->messenger(), "mauve_launcher/window",
          &flutter::StandardMethodCodec::GetInstance());
  window_channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<flutter::EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>>
                 result) {
        const std::string& method = call.method_name();
        if (method == "getTitlebarInsets") {
          sent_insets_ = CurrentTitlebarInsets();
          result->Success(Encode(sent_insets_));
        } else if (method == "startDrag") {
          result->Success();
          StartWindowDrag();
        } else if (method == "titlebarDoubleClick") {
          result->Success();
          ToggleMaximize();
        } else if (method == "setTitlebarColor") {
          const flutter::EncodableValue* args = call.arguments();
          int64_t argb = 0;
          if (const auto* arg32 = std::get_if<int32_t>(args)) {
            argb = static_cast<uint32_t>(*arg32);
          } else if (const auto* arg64 = std::get_if<int64_t>(args)) {
            argb = *arg64;
          } else {
            result->Error("bad-arguments", "Expected an ARGB int.");
            return;
          }
          SetCaptionColor(RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF,
                              argb & 0xFF));
          result->Success();
        } else {
          result->NotImplemented();
        }
      });

  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  window_channel_ = nullptr;
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}

void FlutterWindow::OnCaptionButtonsChanged() {
  if (!window_channel_) {
    return;
  }
  const TitlebarInsets insets = CurrentTitlebarInsets();
  if (insets == sent_insets_) {
    return;
  }
  sent_insets_ = insets;
  window_channel_->InvokeMethod(
      "titlebarInsetsChanged",
      std::make_unique<flutter::EncodableValue>(Encode(insets)));
}

FlutterWindow::TitlebarInsets FlutterWindow::CurrentTitlebarInsets() {
  const RECT buttons = GetCaptionButtonBounds();
  const RECT client = GetClientArea();
  const double scale = GetDpiForWindow(GetHandle()) / 96.0;
  if (IsRectEmpty(&buttons) || scale <= 0) {
    return TitlebarInsets{};
  }
  return TitlebarInsets{(client.right - buttons.left) / scale,
                        (buttons.top + buttons.bottom) / 2.0 / scale};
}

flutter::EncodableValue FlutterWindow::Encode(const TitlebarInsets& insets) {
  return flutter::EncodableValue(flutter::EncodableMap{
      {flutter::EncodableValue("leading"), flutter::EncodableValue(0.0)},
      {flutter::EncodableValue("trailing"),
       flutter::EncodableValue(insets.trailing)},
      {flutter::EncodableValue("centerY"),
       flutter::EncodableValue(insets.center_y)},
  });
}
