#include "win32_window.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <flutter_windows.h>
#include <windowsx.h>

#include "resource.h"

namespace {

/// Window attribute that enables dark mode window decorations.
///
/// Redefined in case the developer's machine has a Windows SDK older than
/// version 10.0.22000.0.
/// See: https://docs.microsoft.com/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

/// Window attribute for the caption color (Windows 11 and later).
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif

/// Posted after a resize to re-read the caption button bounds once DWM has
/// laid the buttons out for the new size.
constexpr UINT kUpdateCaptionButtonsMessage = WM_APP + 1;

constexpr const wchar_t kWindowClassName[] = L"FLUTTER_RUNNER_WIN32_WINDOW";

/// Registry key for app theme preference.
///
/// A value of 0 indicates apps should use dark mode. A non-zero or missing
/// value indicates apps should use light mode.
constexpr const wchar_t kGetPreferredBrightnessRegKey[] =
  L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
constexpr const wchar_t kGetPreferredBrightnessRegValue[] = L"AppsUseLightTheme";

// The number of Win32Window objects that currently exist.
static int g_active_window_count = 0;

using EnableNonClientDpiScaling = BOOL __stdcall(HWND hwnd);

// Scale helper to convert logical scaler values to physical using passed in
// scale factor
int Scale(int source, double scale_factor) {
  return static_cast<int>(source * scale_factor);
}

// Dynamically loads the |EnableNonClientDpiScaling| from the User32 module.
// This API is only needed for PerMonitor V1 awareness mode.
void EnableFullDpiSupportIfAvailable(HWND hwnd) {
  HMODULE user32_module = LoadLibraryA("User32.dll");
  if (!user32_module) {
    return;
  }
  auto enable_non_client_dpi_scaling =
      reinterpret_cast<EnableNonClientDpiScaling*>(
          GetProcAddress(user32_module, "EnableNonClientDpiScaling"));
  if (enable_non_client_dpi_scaling != nullptr) {
    enable_non_client_dpi_scaling(hwnd);
  }
  FreeLibrary(user32_module);
}

}  // namespace

// Manages the Win32Window's window class registration.
class WindowClassRegistrar {
 public:
  ~WindowClassRegistrar() = default;

  // Returns the singleton registrar instance.
  static WindowClassRegistrar* GetInstance() {
    if (!instance_) {
      instance_ = new WindowClassRegistrar();
    }
    return instance_;
  }

  // Returns the name of the window class, registering the class if it hasn't
  // previously been registered.
  const wchar_t* GetWindowClass();

  // Unregisters the window class. Should only be called if there are no
  // instances of the window.
  void UnregisterWindowClass();

 private:
  WindowClassRegistrar() = default;

  static WindowClassRegistrar* instance_;

  bool class_registered_ = false;
};

WindowClassRegistrar* WindowClassRegistrar::instance_ = nullptr;

const wchar_t* WindowClassRegistrar::GetWindowClass() {
  if (!class_registered_) {
    WNDCLASS window_class{};
    window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
    window_class.lpszClassName = kWindowClassName;
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.cbClsExtra = 0;
    window_class.cbWndExtra = 0;
    window_class.hInstance = GetModuleHandle(nullptr);
    window_class.hIcon =
        LoadIcon(window_class.hInstance, MAKEINTRESOURCE(IDI_APP_ICON));
    window_class.hbrBackground = 0;
    window_class.lpszMenuName = nullptr;
    window_class.lpfnWndProc = Win32Window::WndProc;
    RegisterClass(&window_class);
    class_registered_ = true;
  }
  return kWindowClassName;
}

void WindowClassRegistrar::UnregisterWindowClass() {
  UnregisterClass(kWindowClassName, nullptr);
  class_registered_ = false;
}

Win32Window::Win32Window() {
  ++g_active_window_count;
}

Win32Window::~Win32Window() {
  --g_active_window_count;
  Destroy();
}

bool Win32Window::Create(const std::wstring& title,
                         const Point& origin,
                         const Size& size) {
  Destroy();

  const wchar_t* window_class =
      WindowClassRegistrar::GetInstance()->GetWindowClass();

  const POINT target_point = {static_cast<LONG>(origin.x),
                              static_cast<LONG>(origin.y)};
  HMONITOR monitor = MonitorFromPoint(target_point, MONITOR_DEFAULTTONEAREST);
  UINT dpi = FlutterDesktopGetDpiForMonitor(monitor);
  double scale_factor = dpi / 96.0;

  HWND window = CreateWindow(
      window_class, title.c_str(), WS_OVERLAPPEDWINDOW,
      Scale(origin.x, scale_factor), Scale(origin.y, scale_factor),
      Scale(size.width, scale_factor), Scale(size.height, scale_factor),
      nullptr, nullptr, GetModuleHandle(nullptr), this);

  if (!window) {
    return false;
  }

  UpdateTheme(window);
  ExtendFrameIntoClientArea();
  SetWindowPos(window, nullptr, 0, 0, 0, 0,
               SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                   SWP_NOACTIVATE);

  return OnCreate();
}

bool Win32Window::Show() {
  return ShowWindow(window_handle_, SW_SHOWNORMAL);
}

// static
LRESULT CALLBACK Win32Window::WndProc(HWND const window,
                                      UINT const message,
                                      WPARAM const wparam,
                                      LPARAM const lparam) noexcept {
  if (message == WM_NCCREATE) {
    auto window_struct = reinterpret_cast<CREATESTRUCT*>(lparam);
    SetWindowLongPtr(window, GWLP_USERDATA,
                     reinterpret_cast<LONG_PTR>(window_struct->lpCreateParams));

    auto that = static_cast<Win32Window*>(window_struct->lpCreateParams);
    EnableFullDpiSupportIfAvailable(window);
    that->window_handle_ = window;
  } else if (Win32Window* that = GetThisFromHandle(window)) {
    return that->MessageHandler(window, message, wparam, lparam);
  }

  return DefWindowProc(window, message, wparam, lparam);
}

LRESULT
Win32Window::MessageHandler(HWND hwnd,
                            UINT const message,
                            WPARAM const wparam,
                            LPARAM const lparam) noexcept {

  LRESULT dwm_result = 0;
  if (DwmDefWindowProc(hwnd, message, wparam, lparam, &dwm_result)) {
    return dwm_result;
  }

  switch (message) {
    case WM_NCCALCSIZE: {
      if (!wparam) {
        break;
      }

      auto params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam);
      const LONG original_top = params->rgrc[0].top;
      const LRESULT result = DefWindowProc(hwnd, message, wparam, lparam);
      if (result != 0) {
        return result;
      }
      params->rgrc[0].top = original_top;

      if (IsZoomed(hwnd)) {
        params->rgrc[0].top += ResizeBorderThickness();
      }
      return 0;
    }

    case WM_NCHITTEST: {

      const LRESULT hit = DefWindowProc(hwnd, message, wparam, lparam);
      if (hit != HTCLIENT) {
        return hit;
      }
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd, &point);
      if (IsOnTopResizeBorder(point)) {
        const RECT client = GetClientArea();
        const int corner = ResizeBorderThickness();
        if (point.x < corner) {
          return HTTOPLEFT;
        }
        if (point.x >= client.right - corner) {
          return HTTOPRIGHT;
        }
        return HTTOP;
      }
      return HTCLIENT;
    }

    case WM_PAINT: {

      PAINTSTRUCT paint;
      HDC dc = BeginPaint(hwnd, &paint);
      FillRect(dc, &paint.rcPaint,
               static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      EndPaint(hwnd, &paint);
      return 0;
    }

    case kUpdateCaptionButtonsMessage:
      UpdateCaptionButtons();
      return 0;

    case WM_DESTROY:
      window_handle_ = nullptr;
      Destroy();
      if (quit_on_close_) {
        PostQuitMessage(0);
      }
      return 0;

    case WM_DPICHANGED: {
      auto newRectSize = reinterpret_cast<RECT*>(lparam);
      LONG newWidth = newRectSize->right - newRectSize->left;
      LONG newHeight = newRectSize->bottom - newRectSize->top;

      ExtendFrameIntoClientArea();
      SetWindowPos(hwnd, nullptr, newRectSize->left, newRectSize->top, newWidth,
                   newHeight, SWP_NOZORDER | SWP_NOACTIVATE);

      return 0;
    }
    case WM_SIZE: {
      RECT rect = GetClientArea();
      if (child_content_ != nullptr) {
        // Size and position the child window.
        MoveWindow(child_content_, rect.left, rect.top, rect.right - rect.left,
                   rect.bottom - rect.top, TRUE);
        UpdateCaptionButtons();
        PostMessage(hwnd, kUpdateCaptionButtonsMessage, 0, 0);
      }
      return 0;
    }

    case WM_ACTIVATE:
      if (child_content_ != nullptr) {
        SetFocus(child_content_);
      }
      return 0;

    case WM_DWMCOLORIZATIONCOLORCHANGED:
      UpdateTheme(hwnd);
      return 0;
  }

  return DefWindowProc(window_handle_, message, wparam, lparam);
}

void Win32Window::Destroy() {
  OnDestroy();

  if (window_handle_) {
    DestroyWindow(window_handle_);
    window_handle_ = nullptr;
  }
  if (g_active_window_count == 0) {
    WindowClassRegistrar::GetInstance()->UnregisterWindowClass();
  }
}

Win32Window* Win32Window::GetThisFromHandle(HWND const window) noexcept {
  return reinterpret_cast<Win32Window*>(
      GetWindowLongPtr(window, GWLP_USERDATA));
}

void Win32Window::SetChildContent(HWND content) {
  child_content_ = content;
  SetParent(content, window_handle_);
  RECT frame = GetClientArea();

  MoveWindow(content, frame.left, frame.top, frame.right - frame.left,
             frame.bottom - frame.top, true);
  SetWindowSubclass(content, ChildContentProc, 1,
                    reinterpret_cast<DWORD_PTR>(this));
  UpdateCaptionButtons();

  SetFocus(child_content_);
}

RECT Win32Window::GetClientArea() {
  RECT frame;
  GetClientRect(window_handle_, &frame);
  return frame;
}

HWND Win32Window::GetHandle() {
  return window_handle_;
}

void Win32Window::SetQuitOnClose(bool quit_on_close) {
  quit_on_close_ = quit_on_close;
}

bool Win32Window::OnCreate() {
  // No-op; provided for subclasses.
  return true;
}

void Win32Window::OnDestroy() {
  // No-op; provided for subclasses.
}

void Win32Window::OnCaptionButtonsChanged() {}

RECT Win32Window::GetCaptionButtonBounds() {
  RECT bounds{};
  if (FAILED(DwmGetWindowAttribute(window_handle_, DWMWA_CAPTION_BUTTON_BOUNDS,
                                   &bounds, sizeof(bounds)))) {
    return RECT{};
  }

  RECT window_rect;
  GetWindowRect(window_handle_, &window_rect);
  POINT client_origin{0, 0};
  ClientToScreen(window_handle_, &client_origin);
  OffsetRect(&bounds, window_rect.left - client_origin.x,
             window_rect.top - client_origin.y);
  return bounds;
}

void Win32Window::StartWindowDrag() {

  ReleaseCapture();
  SendMessage(window_handle_, WM_SYSCOMMAND, SC_MOVE | HTCAPTION, 0);
}

void Win32Window::ToggleMaximize() {
  SendMessage(window_handle_, WM_SYSCOMMAND,
              IsZoomed(window_handle_) ? SC_RESTORE : SC_MAXIMIZE, 0);
}

void Win32Window::SetCaptionColor(COLORREF color) {
  DwmSetWindowAttribute(window_handle_, DWMWA_CAPTION_COLOR, &color,
                        sizeof(color));
}

int Win32Window::ResizeBorderThickness() {
  const UINT dpi = GetDpiForWindow(window_handle_);
  return GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

void Win32Window::ExtendFrameIntoClientArea() {

  const UINT dpi = GetDpiForWindow(window_handle_);
  const MARGINS margins{
      0, 0, GetSystemMetricsForDpi(SM_CYCAPTION, dpi) + ResizeBorderThickness(),
      0};
  DwmExtendFrameIntoClientArea(window_handle_, &margins);
}

void Win32Window::UpdateCaptionButtons() {
  if (child_content_ == nullptr) {
    return;
  }
  const RECT client = GetClientArea();
  HRGN region = CreateRectRgn(0, 0, client.right, client.bottom);
  const RECT buttons = GetCaptionButtonBounds();
  if (!IsRectEmpty(&buttons)) {
    HRGN hole = CreateRectRgnIndirect(&buttons);
    CombineRgn(region, region, hole, RGN_DIFF);
    DeleteObject(hole);
  }
  SetWindowRgn(child_content_, region, TRUE);
  OnCaptionButtonsChanged();
}

bool Win32Window::IsOnTopResizeBorder(POINT client_point) {
  return !IsZoomed(window_handle_) && client_point.y >= 0 &&
         client_point.y < ResizeBorderThickness();
}

LRESULT CALLBACK Win32Window::ChildContentProc(HWND window, UINT message,
                                               WPARAM wparam, LPARAM lparam,
                                               UINT_PTR subclass_id,
                                               DWORD_PTR ref_data) {
  auto that = reinterpret_cast<Win32Window*>(ref_data);
  switch (message) {
    case WM_NCHITTEST: {
      POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(that->window_handle_, &point);
      if (that->IsOnTopResizeBorder(point)) {
        return HTTRANSPARENT;
      }
      break;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(window, ChildContentProc, subclass_id);
      break;
  }
  return DefSubclassProc(window, message, wparam, lparam);
}

void Win32Window::UpdateTheme(HWND const window) {
  DWORD light_mode;
  DWORD light_mode_size = sizeof(light_mode);
  LSTATUS result = RegGetValue(HKEY_CURRENT_USER, kGetPreferredBrightnessRegKey,
                               kGetPreferredBrightnessRegValue,
                               RRF_RT_REG_DWORD, nullptr, &light_mode,
                               &light_mode_size);

  if (result == ERROR_SUCCESS) {
    BOOL enable_dark_mode = light_mode == 0;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE,
                          &enable_dark_mode, sizeof(enable_dark_mode));
  }
}
