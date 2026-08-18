#pragma once

#include <functional>
#include <string>

namespace nso {

struct MenuState {
    std::string nickname;
    std::string last_sync;
    std::string status;

    bool auto_sync = false;
    bool notifications = false;
    bool discord = false;
    bool start_on_boot = false;
    bool signed_in = false;
    int sync_interval_minutes = 60;
};

struct PlatformCallbacks {
    std::function<void()> ready;
    std::function<void()> sync_now;
    std::function<void()> toggle_auto;
    std::function<void()> toggle_notifications;
    std::function<void()> toggle_discord;
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
