#include "nso_album_sync/platform.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mach-o/dyld.h>
#include <mutex>
#include <string>

namespace nso {

struct PlatformUi::Impl {
    PlatformCallbacks callbacks;
    MenuState state;
    std::mutex state_mutex;
    NSStatusItem* status_item = nil;
    NSMenu* menu = nil;
};

namespace {

enum MenuCommand : NSInteger {
    SyncNow = 1,
    ToggleAutoSync,
    ToggleNotifications,
    ToggleDiscord,
    SelectFolder,
    OpenFolder,
    ToggleStartOnBoot,
    ConfigureProxy,
    SignInOrOut,
    Exit,
};

NSString* ns_string(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()];
}

}  // namespace

// Objective-C declarations must live at global scope, so the Objective-C menu
// target forwards into this small C++ dispatcher instead of containing the
// application logic itself.
void dispatch_menu_action(PlatformUi::Impl* impl, NSInteger command) {
    if (impl == nullptr) {
        return;
    }

    const auto& callbacks = impl->callbacks;

    switch (static_cast<MenuCommand>(command)) {
        case SyncNow:
            callbacks.sync_now();
            break;
        case ToggleAutoSync:
            callbacks.toggle_auto();
            break;
        case ToggleNotifications:
            callbacks.toggle_notifications();
            break;
        case ToggleDiscord:
            callbacks.toggle_discord();
            break;
        case SelectFolder:
            callbacks.select_folder();
            break;
        case OpenFolder:
            callbacks.open_folder();
            break;
        case ToggleStartOnBoot:
            callbacks.toggle_start();
            break;
        case ConfigureProxy:
            callbacks.proxy();
            break;
        case SignInOrOut:
            callbacks.sign_in_out();
            break;
        case Exit:
            callbacks.exit();
            break;
    }
}

}  // namespace nso

@interface NsoMenuTarget : NSObject {
@public
    nso::PlatformUi::Impl* impl;
}
- (void)action:(id)sender;
@end

@implementation NsoMenuTarget

- (void)action:(id)sender {
    nso::dispatch_menu_action(impl, [sender tag]);
}

@end

namespace nso {

namespace {

NsoMenuTarget* g_menu_target = nil;

NSMenuItem* add_menu_item(
    NSMenu* menu,
    NSString* title,
    NSInteger command,
    bool checked = false) {
    auto* item = [[NSMenuItem alloc]
        initWithTitle:title
               action:@selector(action:)
        keyEquivalent:@""];

    item.tag = command;
    item.target = g_menu_target;
    item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:item];
    return item;
}

void add_disabled_item(NSMenu* menu, const std::string& title) {
    auto* item = [[NSMenuItem alloc]
        initWithTitle:ns_string(title)
               action:nil
        keyEquivalent:@""];
    item.enabled = NO;
    [menu addItem:item];
}

void rebuild_menu(PlatformUi::Impl* impl) {
    if (impl == nullptr || impl->menu == nil) {
        return;
    }

    MenuState state;
    {
        std::lock_guard lock(impl->state_mutex);
        state = impl->state;
    }

    [impl->menu removeAllItems];

    const auto account_label = state.signed_in
        ? "Connected (" + state.nickname + ")"
        : "Not signed in";

    add_disabled_item(impl->menu, account_label);
    add_disabled_item(impl->menu, "Last sync: " + state.last_sync);
    [impl->menu addItem:[NSMenuItem separatorItem]];

    add_menu_item(impl->menu, @"Sync Now", SyncNow);
    add_menu_item(
        impl->menu,
        @"Auto-Sync (Hourly)",
        ToggleAutoSync,
        state.auto_sync);
    add_menu_item(
        impl->menu,
        @"Notifications",
        ToggleNotifications,
        state.notifications);
    add_menu_item(
        impl->menu,
        @"Discord Rich Presence",
        ToggleDiscord,
        state.discord);

    [impl->menu addItem:[NSMenuItem separatorItem]];
    add_menu_item(impl->menu, @"Select Folder…", SelectFolder);
    add_menu_item(impl->menu, @"Open Album Folder", OpenFolder);

    [impl->menu addItem:[NSMenuItem separatorItem]];
    add_menu_item(
        impl->menu,
        @"Start on Boot",
        ToggleStartOnBoot,
        state.start_on_boot);
    add_menu_item(impl->menu, @"HTTP Proxy…", ConfigureProxy);
    add_menu_item(
        impl->menu,
        state.signed_in ? @"Sign Out" : @"Sign In",
        SignInOrOut);

    [impl->menu addItem:[NSMenuItem separatorItem]];
    add_menu_item(impl->menu, @"Quit NSO Album Sync", Exit);
}

std::filesystem::path launch_agent_file() {
    const char* home = std::getenv("HOME");
    return std::filesystem::path(home ? home : ".") /
           "Library" /
           "LaunchAgents" /
           "org.dycool.nso-album-sync.plist";
}

std::string executable_path() {
    std::uint32_t required = 0;
    _NSGetExecutablePath(nullptr, &required);

    std::string path(required, '\0');
    _NSGetExecutablePath(path.data(), &required);
    path.resize(std::strlen(path.c_str()));
    return path;
}

void enqueue_notification(NSString* title, NSString* message) {
    auto* content = [UNMutableNotificationContent new];
    content.title = title;
    content.body = message;
    content.sound = [UNNotificationSound defaultSound];

    auto* request = [UNNotificationRequest
        requestWithIdentifier:[NSUUID UUID].UUIDString
                      content:content
                      trigger:nil];

    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:request
        withCompletionHandler:^(NSError* error) {
            if (error != nil) {
                NSLog(@"NSO Album Sync notification failed: %@", error);
            }
        }];
}

void deliver_notification(NSString* title, NSString* message) {
    auto* center = [UNUserNotificationCenter currentNotificationCenter];

    [center getNotificationSettingsWithCompletionHandler:^(
        UNNotificationSettings* settings) {
        const auto status = settings.authorizationStatus;
        if (status == UNAuthorizationStatusAuthorized ||
            status == UNAuthorizationStatusProvisional) {
            enqueue_notification(title, message);
            return;
        }

        if (status != UNAuthorizationStatusNotDetermined) {
            return;
        }

        [center
            requestAuthorizationWithOptions:(UNAuthorizationOptionAlert |
                                             UNAuthorizationOptionSound)
            completionHandler:^(BOOL granted, NSError* error) {
                if (error != nil) {
                    NSLog(@"NSO Album Sync notification permission failed: %@", error);
                    return;
                }

                if (granted) {
                    enqueue_notification(title, message);
                }
            }];
    }];
}

}  // namespace

PlatformUi::PlatformUi() : impl_(new Impl) {}

PlatformUi::~PlatformUi() {
    delete impl_;
}

void PlatformUi::run(const PlatformCallbacks& callbacks) {
    @autoreleasepool {
        impl_->callbacks = callbacks;

        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        g_menu_target = [NsoMenuTarget new];
        g_menu_target->impl = impl_;

        impl_->status_item = [[NSStatusBar systemStatusBar]
            statusItemWithLength:NSSquareStatusItemLength];
        impl_->status_item.button.title = @"NSO";

        impl_->menu = [NSMenu new];
        rebuild_menu(impl_);
        impl_->status_item.menu = impl_->menu;

        [NSApp run];
    }
}

void PlatformUi::stop() {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSApp terminate:nil];
    });
}

void PlatformUi::update(const MenuState& state) {
    {
        std::lock_guard lock(impl_->state_mutex);
        impl_->state = state;
    }

    if (impl_->menu != nil) {
        dispatch_async(dispatch_get_main_queue(), ^{
            rebuild_menu(impl_);
        });
    }
}

void PlatformUi::notify(
    const std::string& title,
    const std::string& message) {
    NSString* notification_title = ns_string(title);
    NSString* notification_message = ns_string(message);

    deliver_notification(notification_title, notification_message);
}

std::string PlatformUi::prompt(
    const std::string& title,
    const std::string& message,
    const std::string& initial) {
    __block std::string result;

    void (^show_prompt)(void) = ^{
        auto* alert = [NSAlert new];
        alert.messageText = ns_string(title);
        alert.informativeText = ns_string(message);
        [alert addButtonWithTitle:@"OK"];
        [alert addButtonWithTitle:@"Cancel"];

        auto* field = [[NSTextField alloc]
            initWithFrame:NSMakeRect(0, 0, 440, 24)];
        field.stringValue = ns_string(initial);
        alert.accessoryView = field;

        if ([alert runModal] == NSAlertFirstButtonReturn) {
            result = field.stringValue.UTF8String;
        }
    };

    if ([NSThread isMainThread]) {
        show_prompt();
    } else {
        dispatch_sync(dispatch_get_main_queue(), show_prompt);
    }

    return result;
}

bool PlatformUi::confirm(
    const std::string& title,
    const std::string& message) {
    __block bool confirmed = false;

    void (^show_confirmation)(void) = ^{
        auto* alert = [NSAlert new];
        alert.messageText = ns_string(title);
        alert.informativeText = ns_string(message);
        [alert addButtonWithTitle:@"Yes"];
        [alert addButtonWithTitle:@"No"];
        confirmed = [alert runModal] == NSAlertFirstButtonReturn;
    };

    if ([NSThread isMainThread]) {
        show_confirmation();
    } else {
        dispatch_sync(dispatch_get_main_queue(), show_confirmation);
    }

    return confirmed;
}

std::string PlatformUi::choose_folder(const std::string& initial) {
    __block std::string result;

    void (^show_picker)(void) = ^{
        auto* panel = [NSOpenPanel openPanel];
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.allowsMultipleSelection = NO;

        if (!initial.empty()) {
            panel.directoryURL = [NSURL fileURLWithPath:ns_string(initial)];
        }

        if ([panel runModal] == NSModalResponseOK) {
            result = panel.URL.path.UTF8String;
        }
    };

    if ([NSThread isMainThread]) {
        show_picker();
    } else {
        dispatch_sync(dispatch_get_main_queue(), show_picker);
    }

    return result;
}

bool start_on_boot_enabled() {
    return std::filesystem::exists(launch_agent_file());
}

void set_start_on_boot(bool enabled) {
    const auto file = launch_agent_file();

    if (!enabled) {
        std::error_code error;
        std::filesystem::remove(file, error);
        return;
    }

    std::filesystem::create_directories(file.parent_path());

    std::ofstream output(file);
    output
        << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\">\n"
        << "<dict>\n"
        << "  <key>Label</key>\n"
        << "  <string>org.dycool.nso-album-sync</string>\n"
        << "  <key>ProgramArguments</key>\n"
        << "  <array><string>" << executable_path() << "</string></array>\n"
        << "  <key>RunAtLoad</key>\n"
        << "  <true/>\n"
        << "</dict>\n"
        << "</plist>\n";
}

}  // namespace nso

#endif  // __APPLE__
