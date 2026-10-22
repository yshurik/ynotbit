#include "mac_integration.h"
#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <objc/runtime.h>

namespace {
std::function<void()> &reopenHandler() {
    static std::function<void()> handler;
    return handler;
}
} // namespace

@interface YnotbitReopen : NSObject
- (void)reopen:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply;
@end
@implementation YnotbitReopen
- (void)reopen:(NSAppleEventDescriptor *)event withReplyEvent:(NSAppleEventDescriptor *)reply {
    if (auto &handler = reopenHandler())
        handler();
}
@end

namespace bm::mac {
void activateApp() {
    if (!NSApp) // no Cocoa application: the tests run on the offscreen platform
        return;
    if (@available(macOS 14.0, *)) {
        [NSApp activate];
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        [NSApp activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
    }
}
void onReopen(std::function<void()> handler) {
    reopenHandler() = std::move(handler);
    // The event manager does not keep its target: this one lives as long as the app.
    static YnotbitReopen *target = [[YnotbitReopen alloc] init];
    [[NSAppleEventManager sharedAppleEventManager] setEventHandler:target
                                                      andSelector:@selector(reopen:withReplyEvent:)
                                                    forEventClass:kCoreEventClass
                                                       andEventID:kAEReopenApplication];
}
void silenceStatusItemClicks() {
    // The delegate of the NSStatusItem Qt makes (qcocoasystemtrayicon.mm). Not API: the tray-click
    // test clicks the icon, and fails if these are no longer what Qt calls. Without the Cocoa
    // platform (the tests' offscreen one) there is no such class, and nothing is swapped.
    Class delegate = NSClassFromString(@"QStatusItemDelegate");
    // Made once, so that calling this again swaps the same functions in and loses none.
    static IMP nothingOnClick = imp_implementationWithBlock(^(id) {});
    static IMP nothingOnMenu = imp_implementationWithBlock(^(id, NSNotification *) {});
    if (Method method = class_getInstanceMethod(delegate, @selector(statusItemClicked)))
        method_setImplementation(method, nothingOnClick);
    if (Method method = class_getInstanceMethod(delegate, @selector(statusItemMenuBeganTracking:)))
        method_setImplementation(method, nothingOnMenu);
}
} // namespace bm::mac
