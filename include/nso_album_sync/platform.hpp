#pragma once

#include "nso_album_sync/rpc_settings.hpp"

#include <filesystem>
#include <functional>
#include <string>

#if defined(__linux__) && defined(NSO_HAVE_GTK)
#include <gtk/gtk.h>

namespace nso::detail {

struct GtkClipboardUriData {
    std::string uri;
};

inline void gtk_clipboard_uri_get(
    GtkClipboard*,
    GtkSelectionData* selection_data,
    guint info,
    gpointer user_data) {
    auto* data = static_cast<GtkClipboardUriData*>(user_data);
    if (data == nullptr) {
        return;
    }

    if (info == 0) {
        gchar* uris[] = {data->uri.data(), nullptr};
        gtk_selection_data_set_uris(selection_data, uris);
        return;
    }

    const std::string payload = "copy\n" + data->uri;
    gtk_selection_data_set(
        selection_data,
        gtk_selection_data_get_target(selection_data),
        8,
        reinterpret_cast<const guchar*>(payload.data()),
        static_cast<gint>(payload.size()));
}

inline void gtk_clipboard_uri_clear(GtkClipboard*, gpointer user_data) {
    delete static_cast<GtkClipboardUriData*>(user_data);
}

inline gboolean gtk_clipboard_set_uris_compat(
    GtkClipboard* clipboard,
    gchar** uris) {
    if (clipboard == nullptr || uris == nullptr || uris[0] == nullptr ||
        uris[0][0] == '\0') {
        return FALSE;
    }

    GtkTargetEntry targets[] = {
        {const_cast<gchar*>("text/uri-list"), 0, 0},
        {const_cast<gchar*>("x-special/gnome-copied-files"), 0, 1},
    };

    auto* data = new GtkClipboardUriData{uris[0]};
    if (!gtk_clipboard_set_with_data(
            clipboard,
            targets,
            G_N_ELEMENTS(targets),
            gtk_clipboard_uri_get,
            gtk_clipboard_uri_clear,
            data)) {
        delete data;
        return FALSE;
    }

    return TRUE;
}

}  // namespace nso::detail

#define gtk_clipboard_set_uris(clipboard, uris) \
    ::nso::detail::gtk_clipboard_set_uris_compat((clipboard), (uris))
#endif

namespace nso {

struct MenuState {
    std::string nickname;
    std::string last_sync;
    std::string status;

    bool auto_sync = false;
    bool notifications = false;
    bool discord = false;
    RpcSettings rpc;
    bool start_on_boot = false;
    bool signed_in = false;
    bool sync_busy = false;
    int sync_interval_minutes = 60;
};

struct PlatformCallbacks {
    std::function<void()> ready;
    std::function<void()> sync_now;
    std::function<void()> copy_last_capture;
    std::function<void()> toggle_auto;
    std::function<void()> toggle_notifications;
    std::function<void()> toggle_discord;
    std::function<void()> toggle_rpc_zelda;
    std::function<void()> toggle_rpc_animal_crossing;
    std::function<void()> toggle_rpc_splatoon3;
    std::function<void()> toggle_rpc_splatoon2;
    std::function<void()> toggle_rpc_username;
    std::function<void()> toggle_rpc_profile_picture;
    std::function<void()> toggle_rpc_play_time;
    std::function<void()> refresh_rpc;
    std::function<void()> select_folder;
    std::function<void()> open_folder;
    std::function<void()> toggle_start;
    std::function<void()> proxy;
    std::function<void()> sign_in_out;
    std::function<void()> exit;
};

class PlatformUi {
public:
    PlatformUi();
    ~PlatformUi();

    void run(const PlatformCallbacks& callbacks);
    void stop();
    void update(const MenuState& state);
    void notify(const std::string& title, const std::string& message);
    bool copy_file_to_clipboard(const std::filesystem::path& path);

    std::string prompt(
        const std::string& title,
        const std::string& message,
        const std::string& initial = "");

    bool confirm(const std::string& title, const std::string& message);
    std::string choose_folder(const std::string& initial);

    struct Impl;

private:
    Impl* impl_;
};

void set_start_on_boot(bool enabled);
bool start_on_boot_enabled();

}  // namespace nso
