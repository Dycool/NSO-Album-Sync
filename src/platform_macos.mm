#include "nso_album_sync/platform.hpp"
#include "nso_album_sync/auth_callback.hpp"
#include "nso_album_sync/util.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <UserNotifications/UserNotifications.h>

#include <algorithm>
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

NSString* auto_sync_title(int minutes) {
    const int safe_minutes = std::max(1, minutes);
    if (safe_minutes == 60) {
        return @"Auto-Sync (Hourly)";
    }
    return [NSString stringWithFormat:@"Auto-Sync (Every %d min)", safe_minutes];
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

std::string xml_escape(std::string value) {
    const auto replace_all = [&](const std::string& from, const std::string& to) {
        std::size_t position = 0;
        while ((position = value.find(from, position)) != std::string::npos) {
            value.replace(position, from.size(), to);
            position += to.size();
        }
    };

    replace_all("&", "&amp;");
    replace_all("<", "&lt;");
    replace_all(">", "&gt;");
    replace_all("\"", "&quot;");
    replace_all("'", "&apos;");
    return value;
}

}  // namespace

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

@interface NsoNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation NsoNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    (void)center;
    (void)notification;
    completionHandler(
        UNNotificationPresentationOptionList |
        UNNotificationPresentationOptionBanner |
        UNNotificationPresentationOptionSound);
}
@end

namespace nso {
namespace {

constexpr char kNxapiDisclosureTitle[] = "Third-Party Service Disclosure";
constexpr char kNxapiSourceUrl[] =
    "https://github.com/samuelthomas2774/nxapi-znca-api";

NsoMenuTarget* g_menu_target = nil;
NsoNotificationDelegate* g_notification_delegate = nil;

void decorate_menu_item(NSMenuItem* item, NSString* symbol) {
    if (item == nil || symbol == nil) {
        return;
    }

    if (@available(macOS 11.0, *)) {
        item.image = [NSImage imageWithSystemSymbolName:symbol
                               accessibilityDescription:item.title];
    }
}

NSMenuItem* add_menu_item(
    NSMenu* menu,
    NSString* title,
    NSInteger command,
    bool checked = false,
    bool enabled = true,
    NSString* symbol = nil) {
    auto* item = [[NSMenuItem alloc]
        initWithTitle:title
               action:@selector(action:)
        keyEquivalent:@""];

    item.tag = command;
    item.target = g_menu_target;
    item.state = checked ? NSControlStateValueOn : NSControlStateValueOff;
    item.enabled = enabled;
    decorate_menu_item(item, symbol);
    [menu addItem:item];
    return item;
}

void add_status_item(
    NSMenu* menu,
    const std::string& title,
    bool emphasized = false) {
    auto* item = [[NSMenuItem alloc]
        initWithTitle:ns_string(title)
               action:nil
        keyEquivalent:@""];
    item.enabled = NO;

    if (emphasized) {
        NSDictionary* attributes = @{
            NSFontAttributeName: [NSFont boldSystemFontOfSize:[NSFont systemFontSize]]
        };
        item.attributedTitle = [[NSAttributedString alloc]
            initWithString:ns_string(title)
                attributes:attributes];
    }

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

    add_status_item(
        impl->menu,
        state.signed_in
            ? "Connected as " + state.nickname
            : "Not signed in");
    if (!state.status.empty() && state.status != "Ready") {
        add_status_item(impl->menu, state.status);
    }
    add_status_item(impl->menu, "Last sync: " + state.last_sync);
    [impl->menu addItem:[NSMenuItem separatorItem]];

    add_menu_item(
        impl->menu,
        @"Sync Now",
        SyncNow,
        false,
        state.signed_in,
        @"arrow.triangle.2.circlepath");
    add_menu_item(
        impl->menu,
        auto_sync_title(state.sync_interval_minutes),
        ToggleAutoSync,
        state.auto_sync,
        true,
        @"clock.arrow.circlepath");
    add_menu_item(
        impl->menu,
        @"Notifications",
        ToggleNotifications,
        state.notifications,
        true,
        @"bell");
    add_menu_item(
        impl->menu,
        @"Discord Rich Presence",
        ToggleDiscord,
        state.discord,
        true,
        @"person.2.wave.2");

    [impl->menu addItem:[NSMenuItem separatorItem]];
    add_menu_item(
        impl->menu,
        @"Choose Album Folder…",
        SelectFolder,
        false,
        true,
        @"folder.badge.plus");
    add_menu_item(
        impl->menu,
        @"Open Album Folder",
        OpenFolder,
        false,
        true,
        @"folder");

    [impl->menu addItem:[NSMenuItem separatorItem]];
    add_menu_item(
        impl->menu,
        @"Start on Boot",
        ToggleStartOnBoot,
        state.start_on_boot,
        true,
        @"power");
    add_menu_item(
        impl->menu,
        @"HTTP Proxy…",
        ConfigureProxy,
        false,
        true,
        @"network");
    add_menu_item(
        impl->menu,
        state.signed_in ? @"Sign Out" : @"Sign In to Nintendo Account…",
        SignInOrOut,
        false,
        true,
        state.signed_in ? @"rectangle.portrait.and.arrow.right" : @"person.crop.circle.badge.plus");

    [impl->menu addItem:[NSMenuItem separatorItem]];
    add_menu_item(
        impl->menu,
        @"Exit",
        Exit,
        false,
        true,
        @"xmark.circle");
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

        register_nintendo_auth_protocol();

        g_notification_delegate = [NsoNotificationDelegate new];
        [UNUserNotificationCenter currentNotificationCenter].delegate =
            g_notification_delegate;

        g_menu_target = [NsoMenuTarget new];
        g_menu_target->impl = impl_;

        impl_->status_item = [[NSStatusBar systemStatusBar]
            statusItemWithLength:NSSquareStatusItemLength];

        NSString* icon_path = [[NSBundle mainBundle]
            pathForResource:@"app"
                     ofType:@"icns"];
        NSImage* status_icon = icon_path != nil
            ? [[NSImage alloc] initWithContentsOfFile:icon_path]
            : nil;

        if (status_icon != nil) {
            [status_icon setTemplate:YES];
            [status_icon setSize:NSMakeSize(18.0, 18.0)];
            impl_->status_item.button.image = status_icon;
            impl_->status_item.button.imagePosition = NSImageOnly;
        } else {
            impl_->status_item.button.title = @"NSO";
        }
        impl_->status_item.button.toolTip = @"NSO Album Sync";

        impl_->menu = [NSMenu new];
        impl_->menu.autoenablesItems = NO;
        rebuild_menu(impl_);
        impl_->status_item.menu = impl_->menu;

        if (impl_->callbacks.ready) {
            impl_->callbacks.ready();
        }

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
    deliver_notification(ns_string(title), ns_string(message));
}

std::string PlatformUi::prompt(
    const std::string& title,
    const std::string& message,
    const std::string& initial) {
    __block std::string result = initial;

    void (^show_prompt)(void) = ^{
        const bool is_proxy = title == "HTTP Proxy";
        auto* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleInformational;
        alert.messageText = ns_string(title);
        alert.informativeText = ns_string(message);
        alert.icon = [NSApp applicationIconImage];
        [alert addButtonWithTitle:is_proxy ? @"Save" : @"Continue"];
        [alert addButtonWithTitle:@"Cancel"];

        auto* field = [[NSTextField alloc]
            initWithFrame:NSMakeRect(0, 0, 500, 26)];
        field.stringValue = ns_string(initial);
        field.placeholderString = is_proxy
            ? @"http://127.0.0.1:8080"
            : @"Optional value";
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
        const bool is_disclosure = title == kNxapiDisclosureTitle;
        for (;;) {
            auto* alert = [NSAlert new];
            alert.alertStyle = is_disclosure
                ? NSAlertStyleInformational
                : NSAlertStyleWarning;
            alert.messageText = ns_string(title);
            alert.informativeText = ns_string(message);
            alert.icon = [NSApp applicationIconImage];
            [alert addButtonWithTitle:is_disclosure ? @"Continue" : @"Confirm"];
            [alert addButtonWithTitle:@"Cancel"];
            if (is_disclosure) {
                [alert addButtonWithTitle:@"View Source"];
            }

            const auto response = [alert runModal];
            if (is_disclosure && response == NSAlertThirdButtonReturn) {
                open_url(kNxapiSourceUrl);
                continue;
            }
            confirmed = response == NSAlertFirstButtonReturn;
            break;
        }
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
        panel.title = @"Choose Album Folder";
        panel.prompt = @"Choose Folder";
        panel.message = @"Select where NSO Album Sync should save your album captures.";
        panel.canChooseDirectories = YES;
        panel.canChooseFiles = NO;
        panel.canCreateDirectories = YES;
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

    const auto escaped_path = xml_escape(executable_path());
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
        << "  <array><string>" << escaped_path << "</string></array>\n"
        << "  <key>RunAtLoad</key>\n"
        << "  <true/>\n"
        << "</dict>\n"
        << "</plist>\n";
}

}  // namespace nso

#endif  // __APPLE__
