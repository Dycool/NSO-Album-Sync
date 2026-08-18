#include "nso_album_sync/auth_callback.hpp"

#ifdef __APPLE__

#import <Cocoa/Cocoa.h>
#import <CoreServices/CoreServices.h>

@interface NsoAuthUrlDelegate : NSObject <NSApplicationDelegate>
@end

@implementation NsoAuthUrlDelegate
- (void)application:(NSApplication*)application openURLs:(NSArray<NSURL*>*)urls {
    (void)application;
    for (NSURL* url in urls) {
        NSString* absolute = url.absoluteString;
        if (absolute == nil) continue;
        const char* utf8 = absolute.UTF8String;
        if (utf8 != nullptr) nso::publish_nintendo_auth_callback(utf8);
    }
}
@end

namespace nso {
namespace {

NsoAuthUrlDelegate* g_auth_url_delegate = nil;

void install_auth_delegate() {
    if (g_auth_url_delegate == nil) g_auth_url_delegate = [NsoAuthUrlDelegate new];
    [NSApp setDelegate:g_auth_url_delegate];
}

}  // namespace

bool register_nintendo_auth_protocol() {
    NSString* bundle_id = [[NSBundle mainBundle] bundleIdentifier];
    if (bundle_id == nil) return false;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CFStringRef current = LSCopyDefaultHandlerForURLScheme(CFSTR("npf71b963c1b7b6d119"));
#pragma clang diagnostic pop

    if (current != nullptr) {
        const bool ours = CFStringCompare(current, (CFStringRef)bundle_id,
            kCFCompareCaseInsensitive) == kCFCompareEqualTo;
        CFRelease(current);
        if (!ours) return false;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const OSStatus status = LSSetDefaultHandlerForURLScheme(
            CFSTR("npf71b963c1b7b6d119"), (CFStringRef)bundle_id);
#pragma clang diagnostic pop
        if (status != noErr) return false;
    }

    install_auth_delegate();
    return true;
}

void unregister_nintendo_auth_protocol() {
}

}  // namespace nso

#endif  // __APPLE__
