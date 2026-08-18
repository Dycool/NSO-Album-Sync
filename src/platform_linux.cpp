#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/util.hpp"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#ifdef NSO_HAVE_GTK
#include <gtk/gtk.h>
#ifdef NSO_HAVE_APPINDICATOR
#include <libayatana-appindicator/app-indicator.h>
#endif
#endif

namespace nso {

struct PlatformUi::Impl {
    PlatformCallbacks callbacks;
    MenuState state;
    std::atomic<bool> running{true};
    std::mutex state_mutex;

#ifdef NSO_HAVE_GTK
    GtkWidget* menu = nullptr;
#ifdef NSO_HAVE_APPINDICATOR
    AppIndicator* indicator = nullptr;
#endif
#endif
};

namespace {

constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiSourceUrl[] =
    "https://github.com/samuelthomas2774/nxapi-znca-api";

std::filesystem::path autostart_file() {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return {};
    }

    return std::filesystem::path(home) /
           ".config" /
           "autostart" /
           "nso-album-sync.desktop";
}

std::string executable_path() {
    if (const char* appimage = std::getenv("APPIMAGE");
        appimage != nullptr && *appimage != '\0') {
        return appimage;
    }

    std::array<char, 4096> buffer{};
    const auto length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length > 0) {
        buffer[static_cast<std::size_t>(length)] = '\0';
        return buffer.data();
    }

    return "nso-album-sync";
}

std::string desktop_escape(std::string value) {
    std::string result;
    result.reserve(value.size() + 8);
    for (char character : value) {
        if (character == '\\' || character == '"' || character == '`' ||
            character == '$') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    return result;
}

#ifdef NSO_HAVE_GTK

void install_nso_css() {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;

    static constexpr char kCss[] =
        ".nso-title { color: #e60012; font-weight: 700; font-size: 16px; }"
        ".nso-subtle { color: #666666; }"
        ".nso-dialog { background: #f7f7f7; }"
        ".nso-dialog button.suggested-action {"
        "  background-image: none; background-color: #e60012; color: white;"
        "  border-radius: 4px; min-height: 30px; min-width: 92px;"
        "}"
        ".nso-dialog entry { min-height: 30px; }";

    auto* provider = gtk_css_provider_new();
    GError* error = nullptr;
    if (!gtk_css_provider_load_from_data(provider, kCss, -1, &error)) {
        if (error != nullptr) {
            std::cerr << "GTK CSS load failed: " << error->message << '\n';
            g_error_free(error);
        }
        g_object_unref(provider);
        return;
    }

    if (auto* screen = gdk_screen_get_default(); screen != nullptr) {
        gtk_style_context_add_provider_for_screen(
            screen,
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }
    g_object_unref(provider);
}

void invoke_callback(std::function<void()>* callback) {
    if (callback != nullptr && *callback) {
        (*callback)();
    }
}

GtkWidget* append_menu_item(
    GtkWidget* menu,
    const std::string& label,
    std::function<void()>* callback,
    bool enabled = true) {
    auto* item = gtk_menu_item_new_with_label(label.c_str());
    gtk_widget_set_sensitive(item, enabled ? TRUE : FALSE);
    if (callback != nullptr) {
        g_signal_connect_swapped(
            item,
            "activate",
            G_CALLBACK(invoke_callback),
            callback);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

void append_check_item(
    GtkWidget* menu,
    const std::string& label,
    bool checked,
    std::function<void()>* callback) {
    auto* item = gtk_check_menu_item_new_with_label(label.c_str());
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
    g_signal_connect_swapped(
        item,
        "activate",
        G_CALLBACK(invoke_callback),
        callback);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

void append_separator(GtkWidget* menu) {
    gtk_menu_shell_append(
        GTK_MENU_SHELL(menu),
        gtk_separator_menu_item_new());
}

void append_status_item(
    GtkWidget* menu,
    const std::string& label,
    bool emphasized = false) {
    auto* item = append_menu_item(menu, label, nullptr, false);
    if (!emphasized) {
        return;
    }

    if (auto* child = gtk_bin_get_child(GTK_BIN(item));
        child != nullptr && GTK_IS_LABEL(child)) {
        const auto escaped = std::string("<b>") + label + "</b>";
        gtk_label_set_markup(GTK_LABEL(child), escaped.c_str());
    }
}

std::string auto_sync_label(int minutes) {
    const int safe_minutes = std::max(1, minutes);
    if (safe_minutes == 60) {
        return "Auto-Sync (Hourly)";
    }
    return "Auto-Sync (Every " + std::to_string(safe_minutes) + " min)";
}

void rebuild_menu(PlatformUi::Impl* impl) {
    if (impl == nullptr || impl->menu == nullptr) {
        return;
    }

    auto* children = gtk_container_get_children(GTK_CONTAINER(impl->menu));
    for (auto* node = children; node != nullptr; node = node->next) {
        gtk_widget_destroy(GTK_WIDGET(node->data));
    }
    g_list_free(children);

    MenuState state;
    {
        std::lock_guard lock(impl->state_mutex);
        state = impl->state;
    }

    append_status_item(
        impl->menu,
        state.signed_in
            ? "Connected as " + state.nickname
            : "Not signed in");
    append_status_item(impl->menu, "Last sync: " + state.last_sync);
    append_separator(impl->menu);

    append_menu_item(
        impl->menu,
        "Sync Now",
        &impl->callbacks.sync_now,
        state.signed_in);
    append_check_item(
        impl->menu,
        auto_sync_label(state.sync_interval_minutes),
        state.auto_sync,
        &impl->callbacks.toggle_auto);
    append_check_item(
        impl->menu,
        "Notifications",
        state.notifications,
        &impl->callbacks.toggle_notifications);
    append_check_item(
        impl->menu,
        "Discord Rich Presence",
        state.discord,
        &impl->callbacks.toggle_discord);

    append_separator(impl->menu);
    append_menu_item(
        impl->menu,
        "Choose Album Folder…",
        &impl->callbacks.select_folder);
    append_menu_item(
        impl->menu,
        "Open Album Folder",
        &impl->callbacks.open_folder);

    append_separator(impl->menu);
    append_check_item(
        impl->menu,
        "Start on Boot",
        state.start_on_boot,
        &impl->callbacks.toggle_start);
    append_menu_item(impl->menu, "HTTP Proxy…", &impl->callbacks.proxy);
    append_menu_item(
        impl->menu,
        state.signed_in ? "Sign Out" : "Sign In to Nintendo Account…",
        &impl->callbacks.sign_in_out);

    append_separator(impl->menu);
    append_menu_item(impl->menu, "Exit", &impl->callbacks.exit);
    gtk_widget_show_all(impl->menu);
}

GtkWidget* create_dialog(
    const std::string& title,
    const std::string& message,
    bool with_entry,
    const std::string& initial,
    GtkWidget** entry_out) {
    const bool is_proxy = title == "HTTP Proxy";
    auto* dialog = gtk_dialog_new_with_buttons(
        title.c_str(),
        nullptr,
        static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
        "Cancel",
        GTK_RESPONSE_CANCEL,
        is_proxy ? "Save" : "Continue",
        GTK_RESPONSE_OK,
        nullptr);

    gtk_window_set_default_size(
        GTK_WINDOW(dialog),
        580,
        with_entry ? (is_proxy ? 205 : 240) : 210);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(dialog),
        "nso-dialog");

    auto* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 20);
    gtk_box_set_spacing(GTK_BOX(content), 10);

    auto* brand = gtk_label_new("Nintendo Switch Online · Album Sync");
    gtk_label_set_xalign(GTK_LABEL(brand), 0.0f);
    gtk_style_context_add_class(
        gtk_widget_get_style_context(brand),
        "nso-title");
    gtk_box_pack_start(GTK_BOX(content), brand, FALSE, FALSE, 0);

    auto* label = gtk_label_new(message.c_str());
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 72);
    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 0);

    GtkWidget* entry = nullptr;
    if (with_entry) {
        entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), initial.c_str());
        gtk_entry_set_activates_default(GTK_ENTRY(entry), TRUE);
        gtk_entry_set_placeholder_text(
            GTK_ENTRY(entry),
            is_proxy ? "http://127.0.0.1:8080" : "Optional value");
        gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 0);
    }

    if (auto* ok = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_OK);
        ok != nullptr) {
        gtk_style_context_add_class(
            gtk_widget_get_style_context(ok),
            "suggested-action");
    }

    if (entry_out != nullptr) {
        *entry_out = entry;
    }

    gtk_widget_show_all(dialog);
    if (entry != nullptr) {
        gtk_widget_grab_focus(entry);
        gtk_editable_select_region(GTK_EDITABLE(entry), 0, -1);
    }
    return dialog;
}

std::string indicator_icon_path() {
    if (const char* appdir = std::getenv("APPDIR");
        appdir != nullptr && *appdir != '\0') {
        const std::filesystem::path root(appdir);
        const std::array candidates{
            root / "nso-album-sync.svg",
            root / "usr/share/icons/hicolor/scalable/apps/nso-album-sync.svg",
        };
        for (const auto& candidate : candidates) {
            if (std::filesystem::exists(candidate)) {
                return candidate.string();
            }
        }
    }

    const auto local = std::filesystem::absolute("icon.svg");
    if (std::filesystem::exists(local)) {
        return local.string();
    }
    return "applications-games";
}

#endif  // NSO_HAVE_GTK

}  // namespace

PlatformUi::PlatformUi() : impl_(new Impl) {}

PlatformUi::~PlatformUi() {
    delete impl_;
}

void PlatformUi::run(const PlatformCallbacks& callbacks) {
    impl_->callbacks = callbacks;

#ifdef NSO_HAVE_GTK
    int argc = 0;
    char** argv = nullptr;
    gtk_init(&argc, &argv);
    install_nso_css();

    impl_->menu = gtk_menu_new();
    rebuild_menu(impl_);

#ifdef NSO_HAVE_APPINDICATOR
    impl_->indicator = app_indicator_new(
        "nso-album-sync",
        "applications-games",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
    const auto icon = indicator_icon_path();
    app_indicator_set_icon_full(
        impl_->indicator,
        icon.c_str(),
        "NSO Album Sync");
    app_indicator_set_title(impl_->indicator, "NSO Album Sync");
    app_indicator_set_status(
        impl_->indicator,
        APP_INDICATOR_STATUS_ACTIVE);
    app_indicator_set_menu(
        impl_->indicator,
        GTK_MENU(impl_->menu));
#else
    std::cerr
        << "GTK build has no AppIndicator support; menu is unavailable.\n";
#endif

    if (impl_->callbacks.ready) {
        impl_->callbacks.ready();
    }

    gtk_main();
#else
    std::cout
        << "NSO Album Sync running. This build has no GTK/AppIndicator; "
           "use Ctrl+C to stop.\n";

    if (impl_->callbacks.ready) {
        impl_->callbacks.ready();
    }

    while (impl_->running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
#endif
}

void PlatformUi::stop() {
    impl_->running = false;

#ifdef NSO_HAVE_GTK
    if (gtk_main_level()) {
        g_idle_add(
            +[](gpointer) -> gboolean {
                gtk_main_quit();
                return G_SOURCE_REMOVE;
            },
            nullptr);
    }
#endif
}

void PlatformUi::update(const MenuState& state) {
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->state = state;
    }

#ifdef NSO_HAVE_GTK
    if (impl_->menu != nullptr) {
        g_idle_add(
            +[](gpointer data) -> gboolean {
                rebuild_menu(static_cast<Impl*>(data));
                return G_SOURCE_REMOVE;
            },
            impl_);
    }
#endif
}

void PlatformUi::notify(
    const std::string& title,
    const std::string& message) {
    std::cerr << title << ": " << message << '\n';

#ifdef NSO_HAVE_GTK
    char* arguments[] = {
        const_cast<char*>("notify-send"),
        const_cast<char*>("--app-name=NSO Album Sync"),
        const_cast<char*>("--icon=applications-games"),
        const_cast<char*>(title.c_str()),
        const_cast<char*>(message.c_str()),
        nullptr,
    };

    GError* error = nullptr;
    const gboolean launched = g_spawn_async(
        nullptr,
        arguments,
        nullptr,
        static_cast<GSpawnFlags>(G_SPAWN_SEARCH_PATH |
                                 G_SPAWN_STDOUT_TO_DEV_NULL |
                                 G_SPAWN_STDERR_TO_DEV_NULL),
        nullptr,
        nullptr,
        nullptr,
        &error);

    if (!launched && error != nullptr) {
        std::cerr << "Notification launch failed: " << error->message << '\n';
        g_error_free(error);
    }
#endif
}

std::string PlatformUi::prompt(
    const std::string& title,
    const std::string& message,
    const std::string& initial) {
#ifdef NSO_HAVE_GTK
    GtkWidget* entry = nullptr;
    auto* dialog = create_dialog(title, message, true, initial, &entry);

    std::string result = initial;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK && entry != nullptr) {
        result = gtk_entry_get_text(GTK_ENTRY(entry));
    }

    gtk_widget_destroy(dialog);
    return result;
#else
    std::cout << '\n' << title << '\n' << message << "\n> ";
    std::string value;
    std::getline(std::cin, value);
    if (title == "HTTP Proxy") {
        return value;
    }
    return value.empty() ? initial : value;
#endif
}

bool PlatformUi::confirm(
    const std::string& title,
    const std::string& message) {
    const bool is_disclosure = title == kNxapiDisclosureTitle;
#ifdef NSO_HAVE_GTK
    auto* dialog = create_dialog(title, message, false, {}, nullptr);
    if (is_disclosure) {
        gtk_dialog_add_button(
            GTK_DIALOG(dialog),
            "View Source",
            GTK_RESPONSE_APPLY);
        gtk_widget_show_all(dialog);
    }

    bool confirmed = false;
    for (;;) {
        const int response = gtk_dialog_run(GTK_DIALOG(dialog));
        if (response == GTK_RESPONSE_APPLY && is_disclosure) {
            open_url(kNxapiSourceUrl);
            continue;
        }
        confirmed = response == GTK_RESPONSE_OK;
        break;
    }
    gtk_widget_destroy(dialog);
    return confirmed;
#else
    for (;;) {
        std::cout << '\n' << title << '\n' << message;
        if (is_disclosure) {
            std::cout << "\n[c] Continue  [s] View Source  [Enter] Cancel\n> ";
        } else {
            std::cout << " [y/N]\n> ";
        }
        std::string answer;
        std::getline(std::cin, answer);
        if (is_disclosure && (answer == "s" || answer == "S")) {
            open_url(kNxapiSourceUrl);
            continue;
        }
        if (is_disclosure) {
            return answer == "c" || answer == "C" ||
                   answer == "continue" || answer == "CONTINUE";
        }
        return answer == "y" || answer == "Y" ||
               answer == "yes" || answer == "YES";
    }
#endif
}

std::string PlatformUi::choose_folder(const std::string& initial) {
#ifdef NSO_HAVE_GTK
    auto* dialog = gtk_file_chooser_dialog_new(
        "Choose Album Folder",
        nullptr,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "Cancel",
        GTK_RESPONSE_CANCEL,
        "Choose Folder",
        GTK_RESPONSE_ACCEPT,
        nullptr);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 720, 520);
    gtk_file_chooser_set_create_folders(GTK_FILE_CHOOSER(dialog), TRUE);

    if (!initial.empty()) {
        gtk_file_chooser_set_filename(
            GTK_FILE_CHOOSER(dialog),
            initial.c_str());
    }

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
        char* selected =
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (selected != nullptr) {
            result = selected;
            g_free(selected);
        }
    }

    gtk_widget_destroy(dialog);
    return result;
#else
    return prompt("Choose Album Folder", "Path:", initial);
#endif
}

bool start_on_boot_enabled() {
    const auto file = autostart_file();
    return !file.empty() && std::filesystem::exists(file);
}

void set_start_on_boot(bool enabled) {
    const auto file = autostart_file();
    if (file.empty()) {
        return;
    }

    if (!enabled) {
        std::error_code error;
        std::filesystem::remove(file, error);
        return;
    }

    std::filesystem::create_directories(file.parent_path());

    const auto executable = desktop_escape(executable_path());
    std::ofstream output(file);
    output
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=NSO Album Sync\n"
        << "Comment=Sync Nintendo Switch Online album captures\n"
        << "Exec=\"" << executable << "\"\n"
        << "Icon=applications-games\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n";
}

}  // namespace nso

#endif  // __linux__
