#include "nso_album_sync/platform.hpp"

#if defined(__linux__)

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

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

#ifdef NSO_HAVE_GTK

void invoke_callback(std::function<void()>* callback) {
    if (callback != nullptr && *callback) {
        (*callback)();
    }
}

void append_menu_item(
    GtkWidget* menu,
    const char* label,
    std::function<void()>* callback) {
    auto* item = gtk_menu_item_new_with_label(label);
    g_signal_connect_swapped(
        item,
        "activate",
        G_CALLBACK(invoke_callback),
        callback);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

void append_check_item(
    GtkWidget* menu,
    const char* label,
    bool checked,
    std::function<void()>* callback) {
    auto* item = gtk_check_menu_item_new_with_label(label);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item), checked);
    g_signal_connect_swapped(
        item,
        "activate",
        G_CALLBACK(invoke_callback),
        callback);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

void append_disabled_item(GtkWidget* menu, const std::string& label) {
    auto* item = gtk_menu_item_new_with_label(label.c_str());
    gtk_widget_set_sensitive(item, FALSE);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
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

    const auto account_label = state.signed_in
        ? "Connected (" + state.nickname + ")"
        : "Not signed in";

    append_disabled_item(impl->menu, account_label);
    append_disabled_item(impl->menu, "Last sync: " + state.last_sync);

    append_menu_item(impl->menu, "Sync Now", &impl->callbacks.sync_now);
    append_check_item(
        impl->menu,
        "Auto-Sync (Hourly)",
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

    append_menu_item(
        impl->menu,
        "Select Folder…",
        &impl->callbacks.select_folder);
    append_menu_item(
        impl->menu,
        "Open Album Folder",
        &impl->callbacks.open_folder);

    append_check_item(
        impl->menu,
        "Start on Boot",
        state.start_on_boot,
        &impl->callbacks.toggle_start);
    append_menu_item(impl->menu, "HTTP Proxy…", &impl->callbacks.proxy);
    append_menu_item(
        impl->menu,
        state.signed_in ? "Sign Out" : "Sign In",
        &impl->callbacks.sign_in_out);
    append_menu_item(impl->menu, "Exit", &impl->callbacks.exit);

    gtk_widget_show_all(impl->menu);
}

#endif  // NSO_HAVE_GTK

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

    impl_->menu = gtk_menu_new();
    rebuild_menu(impl_);

#ifdef NSO_HAVE_APPINDICATOR
    impl_->indicator = app_indicator_new(
        "nso-album-sync",
        "applications-games",
        APP_INDICATOR_CATEGORY_APPLICATION_STATUS);
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

    gtk_main();
#else
    std::cout
        << "NSO Album Sync running. This build has no GTK/AppIndicator; "
           "use Ctrl+C to stop.\n";

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
    auto* dialog = gtk_dialog_new_with_buttons(
        title.c_str(),
        nullptr,
        GTK_DIALOG_MODAL,
        "Cancel",
        GTK_RESPONSE_CANCEL,
        "OK",
        GTK_RESPONSE_OK,
        nullptr);

    auto* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    auto* label = gtk_label_new(message.c_str());
    auto* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), initial.c_str());

    gtk_box_pack_start(GTK_BOX(content), label, FALSE, FALSE, 8);
    gtk_box_pack_start(GTK_BOX(content), entry, FALSE, FALSE, 8);
    gtk_widget_show_all(dialog);

    std::string result;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        result = gtk_entry_get_text(GTK_ENTRY(entry));
    }

    gtk_widget_destroy(dialog);
    return result;
#else
    std::cout << '\n' << title << '\n' << message << "\n> ";

    std::string value;
    std::getline(std::cin, value);
    return value.empty() ? initial : value;
#endif
}

bool PlatformUi::confirm(
    const std::string& title,
    const std::string& message) {
    const auto answer = prompt(title, message + " [y/N]");
    return answer == "y" || answer == "Y" || answer == "yes";
}

std::string PlatformUi::choose_folder(const std::string& initial) {
#ifdef NSO_HAVE_GTK
    auto* dialog = gtk_file_chooser_dialog_new(
        "Select Album Folder",
        nullptr,
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "Cancel",
        GTK_RESPONSE_CANCEL,
        "Select",
        GTK_RESPONSE_ACCEPT,
        nullptr);

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
    return prompt("Select Folder", "Path:", initial);
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

    std::ofstream output(file);
    output
        << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=NSO Album Sync\n"
        << "Exec=nso-album-sync\n"
        << "X-GNOME-Autostart-enabled=true\n";
}

}  // namespace nso

#endif  // __linux__
