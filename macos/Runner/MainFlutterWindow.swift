import Cocoa
import FlutterMacOS

class MainFlutterWindow: NSWindow {
  private var lastMouseDown: NSEvent?
  private var mouseDownMonitor: Any?

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)

    configureTitlebar()

    RegisterGeneratedPlugins(registry: flutterViewController)
    registerWindowChannel(messenger: flutterViewController.engine.binaryMessenger)

    super.awakeFromNib()
  }

  /// Hides the OS title bar and lets Flutter draw under it, while keeping AppKit's own traffic lights.
  private func configureTitlebar() {
    styleMask.insert(.fullSizeContentView)
    titlebarAppearsTransparent = true
    titleVisibility = .hidden

    self.toolbar = NSToolbar(identifier: "MainToolbar")
    toolbarStyle = .unified

    mouseDownMonitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseDown) {
      [weak self] event in
      if event.window === self { self?.lastMouseDown = event }
      return event
    }
  }

  /// Lets Dart ask where the native controls sit so it can keep content clear of them, instead of hard-coding AppKit's metrics.
  private func registerWindowChannel(messenger: FlutterBinaryMessenger) {
    let channel = FlutterMethodChannel(name: "mauve_launcher/window", binaryMessenger: messenger)
    channel.setMethodCallHandler { [weak self] call, result in
      guard let self = self else {
        result(nil)
        return
      }
      switch call.method {
      case "getTitlebarInsets":
        result(self.titlebarInsets())
      case "startDrag":
        if let event = self.lastMouseDown { self.performDrag(with: event) }
        result(nil)
      case "titlebarDoubleClick":
        self.titlebarDoubleClick()
        result(nil)
      case "setTitlebarColor":
        result(nil)
      default:
        result(FlutterMethodNotImplemented)
      }
    }
  }

  private func titlebarInsets() -> [String: Double] {
    let buttons: [NSWindow.ButtonType] = [.closeButton, .miniaturizeButton, .zoomButton]
    let rects = buttons
      .compactMap { standardWindowButton($0) }
      .map { $0.convert($0.bounds, to: nil) }
    guard let first = rects.first else { return [:] }
    let trailing = rects.map(\.maxX).max() ?? 0
    // Window coordinates are bottom-up; Flutter's are top-down.
    let centerY = frame.height - first.midY
    return ["leading": Double(trailing), "trailing": 0, "centerY": Double(centerY)]
  }

  private func titlebarDoubleClick() {
    switch UserDefaults.standard.string(forKey: "AppleActionOnDoubleClick") {
    case "Minimize": performMiniaturize(nil)
    case "None": break
    default: performZoom(nil)
    }
  }
}
