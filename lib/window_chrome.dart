import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

@immutable
class TitlebarInsets {
  const TitlebarInsets({this.leading = 0, this.trailing = 0, this.centerY = 0});

  factory TitlebarInsets.fromMap(Map<Object?, Object?> map) {
    double read(String key) => (map[key] as num?)?.toDouble() ?? 0;
    return TitlebarInsets(
      leading: read('leading'),
      trailing: read('trailing'),
      centerY: read('centerY'),
    );
  }

  final double leading;

  final double trailing;

  final double centerY;

  static const none = TitlebarInsets();

  @override
  bool operator ==(Object other) =>
      other is TitlebarInsets &&
      other.leading == leading &&
      other.trailing == trailing &&
      other.centerY == centerY;

  @override
  int get hashCode => Object.hash(leading, trailing, centerY);
}

abstract final class WindowChrome {
  static const _channel = MethodChannel('mauve_launcher/window');

  static final insets = ValueNotifier<TitlebarInsets>(TitlebarInsets.none);

  static bool get _supported => switch (defaultTargetPlatform) {
    TargetPlatform.macOS ||
    TargetPlatform.windows ||
    TargetPlatform.linux => !kIsWeb,
    _ => false,
  };

  static Future<void> init() async {
    if (!_supported) return;
    _channel.setMethodCallHandler((call) async {
      if (call.method == 'titlebarInsetsChanged') {
        insets.value = TitlebarInsets.fromMap(call.arguments as Map);
      }
    });
    try {
      final map = await _channel.invokeMapMethod<Object?, Object?>(
        'getTitlebarInsets',
      );
      if (map != null) insets.value = TitlebarInsets.fromMap(map);
    } on MissingPluginException {
    }
  }

  static Future<void> startDrag() => _invoke('startDrag');

  static Future<void> titlebarDoubleClick() => _invoke('titlebarDoubleClick');

  static Future<void> setTitlebarColor(Color color) =>
      _invoke('setTitlebarColor', color.toARGB32());

  static Future<void> _invoke(String method, [Object? arguments]) async {
    if (!_supported) return;
    try {
      await _channel.invokeMethod<void>(method, arguments);
    } on MissingPluginException {
    }
  }
}

class WindowDragArea extends StatefulWidget {
  const WindowDragArea({super.key, required this.color});

  final Color color;

  @override
  State<WindowDragArea> createState() => _WindowDragAreaState();
}

class _WindowDragAreaState extends State<WindowDragArea> {
  @override
  void initState() {
    super.initState();
    WindowChrome.setTitlebarColor(widget.color);
  }

  @override
  void didUpdateWidget(WindowDragArea oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.color != widget.color) {
      WindowChrome.setTitlebarColor(widget.color);
    }
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      behavior: HitTestBehavior.opaque,
      onPanStart: (_) => WindowChrome.startDrag(),
      onDoubleTap: WindowChrome.titlebarDoubleClick,
      child: ColoredBox(color: widget.color),
    );
  }
}
