#include "my_application.h"

#include <flutter_linux/flutter_linux.h>

#include <cstring>

#include "flutter/generated_plugin_registrant.h"

struct _MyApplication {
  GtkApplication parent_instance;
  char** dart_entrypoint_arguments;

  GtkWindow* window;
  FlView* view;
  FlMethodChannel* window_channel;
  GtkWidget* leading_controls;
  GtkWidget* trailing_controls;
  GdkEvent* last_button_press;
  double sent_leading, sent_trailing, sent_center_y;
};

G_DEFINE_TYPE(MyApplication, my_application, GTK_TYPE_APPLICATION)

static const char kWindowControlsCss[] =
    "headerbar.mauve-window-controls,"
    "headerbar.mauve-window-controls:backdrop {"
    "  background: none;"
    "  border: none;"
    "  box-shadow: none;"
    "  border-radius: 0;"
    "  min-height: 0;"
    "  margin: 0;"
    "  padding: 6px;"
    "}";

static gchar* window_buttons_only(const gchar* side) {
  g_auto(GStrv) entries = g_strsplit(side, ",", -1);
  g_autoptr(GPtrArray) kept = g_ptr_array_new();
  for (gchar** entry = entries; *entry != nullptr; entry++) {
    g_strstrip(*entry);
    if (strcmp(*entry, "close") == 0 || strcmp(*entry, "minimize") == 0 ||
        strcmp(*entry, "maximize") == 0) {
      g_ptr_array_add(kept, *entry);
    }
  }
  g_ptr_array_add(kept, nullptr);
  return g_strjoinv(",", reinterpret_cast<gchar**>(kept->pdata));
}

static GtkWidget* window_controls_new(const gchar* layout, GtkAlign halign) {
  GtkWidget* bar = gtk_header_bar_new();
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), TRUE);
  gtk_header_bar_set_decoration_layout(GTK_HEADER_BAR(bar), layout);
  gtk_style_context_add_class(gtk_widget_get_style_context(bar),
                              "mauve-window-controls");
  gtk_widget_set_halign(bar, halign);
  gtk_widget_set_valign(bar, GTK_ALIGN_START);
  gtk_widget_show(bar);
  return bar;
}

static FlValue* titlebar_insets(MyApplication* self) {
  double leading = 0, trailing = 0, center_y = 0;
  if (self->leading_controls != nullptr) {
    leading = gtk_widget_get_allocated_width(self->leading_controls);
    center_y =
        gtk_widget_get_allocated_height(self->leading_controls) / 2.0;
  }
  if (self->trailing_controls != nullptr) {
    trailing = gtk_widget_get_allocated_width(self->trailing_controls);
    center_y = MAX(
        center_y,
        gtk_widget_get_allocated_height(self->trailing_controls) / 2.0);
  }
  FlValue* result = fl_value_new_map();
  fl_value_set_string_take(result, "leading", fl_value_new_float(leading));
  fl_value_set_string_take(result, "trailing", fl_value_new_float(trailing));
  fl_value_set_string_take(result, "centerY", fl_value_new_float(center_y));
  return result;
}

static void controls_size_allocate_cb(GtkWidget* widget,
                                      GdkRectangle* allocation,
                                      MyApplication* self) {
  if (self->window_channel == nullptr) {
    return;
  }
  g_autoptr(FlValue) insets = titlebar_insets(self);
  const double leading =
      fl_value_get_float(fl_value_lookup_string(insets, "leading"));
  const double trailing =
      fl_value_get_float(fl_value_lookup_string(insets, "trailing"));
  const double center_y =
      fl_value_get_float(fl_value_lookup_string(insets, "centerY"));
  if (leading == self->sent_leading && trailing == self->sent_trailing &&
      center_y == self->sent_center_y) {
    return;
  }
  self->sent_leading = leading;
  self->sent_trailing = trailing;
  self->sent_center_y = center_y;
  fl_method_channel_invoke_method(self->window_channel,
                                  "titlebarInsetsChanged", insets, nullptr,
                                  nullptr, nullptr);
}

static gboolean window_captured_event_cb(GtkWidget* widget, GdkEvent* event,
                                         MyApplication* self) {
  if (event->type == GDK_BUTTON_PRESS) {
    g_clear_pointer(&self->last_button_press, gdk_event_free);
    self->last_button_press = gdk_event_copy(event);
  }
  return FALSE;
}

static void start_window_drag(MyApplication* self) {
  GdkEvent* press = self->last_button_press;
  if (press == nullptr) {
    return;
  }
  gtk_window_begin_move_drag(self->window, press->button.button,
                             static_cast<gint>(press->button.x_root),
                             static_cast<gint>(press->button.y_root),
                             press->button.time);

  GdkEvent* release = gdk_event_copy(press);
  release->type = GDK_BUTTON_RELEASE;
  gtk_main_do_event(release);
  gdk_event_free(release);
}

static void titlebar_double_click(MyApplication* self) {
  g_autofree gchar* action = nullptr;
  g_object_get(gtk_widget_get_settings(GTK_WIDGET(self->window)),
               "gtk-titlebar-double-click", &action, nullptr);
  if (action == nullptr || g_str_has_prefix(action, "toggle-maximize")) {
    if (gtk_window_is_maximized(self->window)) {
      gtk_window_unmaximize(self->window);
    } else {
      gtk_window_maximize(self->window);
    }
  } else if (strcmp(action, "minimize") == 0) {
    gtk_window_iconify(self->window);
  }
}

static void window_method_call_cb(FlMethodChannel* channel,
                                  FlMethodCall* method_call,
                                  gpointer user_data) {
  MyApplication* self = MY_APPLICATION(user_data);
  const gchar* method = fl_method_call_get_name(method_call);
  g_autoptr(FlMethodResponse) response = nullptr;
  if (strcmp(method, "getTitlebarInsets") == 0) {
    g_autoptr(FlValue) insets = titlebar_insets(self);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(insets));
  } else if (strcmp(method, "startDrag") == 0) {
    start_window_drag(self);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (strcmp(method, "titlebarDoubleClick") == 0) {
    titlebar_double_click(self);
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else if (strcmp(method, "setTitlebarColor") == 0) {
    response = FL_METHOD_RESPONSE(fl_method_success_response_new(nullptr));
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }
  fl_method_call_respond(method_call, response, nullptr);
}

static void setup_window_chrome(MyApplication* self, FlView* view) {
  gtk_window_set_titlebar(self->window, gtk_event_box_new());

  g_autoptr(GtkCssProvider) css = gtk_css_provider_new();
  gtk_css_provider_load_from_data(css, kWindowControlsCss, -1, nullptr);
  gtk_style_context_add_provider_for_screen(
      gtk_window_get_screen(self->window), GTK_STYLE_PROVIDER(css),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

  GtkWidget* overlay = gtk_overlay_new();
  gtk_widget_show(overlay);
  gtk_container_add(GTK_CONTAINER(overlay), GTK_WIDGET(view));
  gtk_container_add(GTK_CONTAINER(self->window), overlay);

  g_autofree gchar* layout = nullptr;
  g_object_get(gtk_widget_get_settings(GTK_WIDGET(self->window)),
               "gtk-decoration-layout", &layout, nullptr);
  g_auto(GStrv) sides = g_strsplit(layout != nullptr ? layout : ":close", ":", 2);
  g_autofree gchar* left = window_buttons_only(sides[0] != nullptr ? sides[0] : "");
  g_autofree gchar* right = window_buttons_only(
      sides[0] != nullptr && sides[1] != nullptr ? sides[1] : "");

  if (left[0] != '\0') {
    g_autofree gchar* bar_layout = g_strconcat(left, ":", nullptr);
    self->leading_controls = window_controls_new(bar_layout, GTK_ALIGN_START);
  }
  if (right[0] != '\0') {
    g_autofree gchar* bar_layout = g_strconcat(":", right, nullptr);
    self->trailing_controls = window_controls_new(bar_layout, GTK_ALIGN_END);
  }
  GtkWidget* bars[] = {self->leading_controls, self->trailing_controls};
  for (GtkWidget* bar : bars) {
    if (bar == nullptr) {
      continue;
    }
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), bar);
    g_signal_connect(bar, "size-allocate",
                     G_CALLBACK(controls_size_allocate_cb), self);
  }

  g_signal_connect(self->window, "captured-event",
                   G_CALLBACK(window_captured_event_cb), self);
}

// Called when first Flutter frame received.
static void first_frame_cb(MyApplication* self, FlView* view) {
  gtk_widget_show(gtk_widget_get_toplevel(GTK_WIDGET(view)));
}

// Implements GApplication::activate.
static void my_application_activate(GApplication* application) {
  MyApplication* self = MY_APPLICATION(application);
  GtkWindow* window =
      GTK_WINDOW(gtk_application_window_new(GTK_APPLICATION(application)));
  self->window = window;

  gtk_window_set_title(window, "mauve_launcher");

  gtk_window_set_default_size(window, 1280, 720);

  g_autoptr(FlDartProject) project = fl_dart_project_new();
  fl_dart_project_set_dart_entrypoint_arguments(
      project, self->dart_entrypoint_arguments);

  FlView* view = fl_view_new(project);
  GdkRGBA background_color;
  // Background defaults to black, override it here if necessary, e.g. #00000000
  // for transparent.
  gdk_rgba_parse(&background_color, "#000000");
  fl_view_set_background_color(view, &background_color);
  gtk_widget_show(GTK_WIDGET(view));
  self->view = view;
  setup_window_chrome(self, view);

  // Show the window when Flutter renders.
  // Requires the view to be realized so we can start rendering.
  g_signal_connect_swapped(view, "first-frame", G_CALLBACK(first_frame_cb),
                           self);
  gtk_widget_realize(GTK_WIDGET(view));

  fl_register_plugins(FL_PLUGIN_REGISTRY(view));

  g_autoptr(FlStandardMethodCodec) codec = fl_standard_method_codec_new();
  self->window_channel = fl_method_channel_new(
      fl_engine_get_binary_messenger(fl_view_get_engine(view)),
      "mauve_launcher/window", FL_METHOD_CODEC(codec));
  fl_method_channel_set_method_call_handler(
      self->window_channel, window_method_call_cb, self, nullptr);

  gtk_widget_grab_focus(GTK_WIDGET(view));
}

// Implements GApplication::local_command_line.
static gboolean my_application_local_command_line(GApplication* application,
                                                  gchar*** arguments,
                                                  int* exit_status) {
  MyApplication* self = MY_APPLICATION(application);
  // Strip out the first argument as it is the binary name.
  self->dart_entrypoint_arguments = g_strdupv(*arguments + 1);

  g_autoptr(GError) error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
    g_warning("Failed to register: %s", error->message);
    *exit_status = 1;
    return TRUE;
  }

  g_application_activate(application);
  *exit_status = 0;

  return TRUE;
}

// Implements GApplication::startup.
static void my_application_startup(GApplication* application) {
  // MyApplication* self = MY_APPLICATION(object);

  // Perform any actions required at application startup.

  G_APPLICATION_CLASS(my_application_parent_class)->startup(application);
}

// Implements GApplication::shutdown.
static void my_application_shutdown(GApplication* application) {
  // MyApplication* self = MY_APPLICATION(object);

  // Perform any actions required at application shutdown.

  G_APPLICATION_CLASS(my_application_parent_class)->shutdown(application);
}

// Implements GObject::dispose.
static void my_application_dispose(GObject* object) {
  MyApplication* self = MY_APPLICATION(object);
  g_clear_pointer(&self->dart_entrypoint_arguments, g_strfreev);
  g_clear_object(&self->window_channel);
  g_clear_pointer(&self->last_button_press, gdk_event_free);
  G_OBJECT_CLASS(my_application_parent_class)->dispose(object);
}

static void my_application_class_init(MyApplicationClass* klass) {
  G_APPLICATION_CLASS(klass)->activate = my_application_activate;
  G_APPLICATION_CLASS(klass)->local_command_line =
      my_application_local_command_line;
  G_APPLICATION_CLASS(klass)->startup = my_application_startup;
  G_APPLICATION_CLASS(klass)->shutdown = my_application_shutdown;
  G_OBJECT_CLASS(klass)->dispose = my_application_dispose;
}

static void my_application_init(MyApplication* self) {}

MyApplication* my_application_new() {
  // Set the program name to the application ID, which helps various systems
  // like GTK and desktop environments map this running application to its
  // corresponding .desktop file. This ensures better integration by allowing
  // the application to be recognized beyond its binary name.
  g_set_prgname(APPLICATION_ID);

  return MY_APPLICATION(g_object_new(my_application_get_type(),
                                     "application-id", APPLICATION_ID, "flags",
                                     G_APPLICATION_NON_UNIQUE, nullptr));
}
