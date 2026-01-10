// macOS NSApplication subclass implementing CefAppProtocol
// Must be initialized BEFORE SDL_Init for CEF compatibility

#import <Cocoa/Cocoa.h>
#include "include/cef_application_mac.h"

@interface JellyfinApplication : NSApplication <CefAppProtocol> {
    BOOL handlingSendEvent_;
}
@end

@implementation JellyfinApplication

- (BOOL)isHandlingSendEvent {
    return handlingSendEvent_;
}

- (void)setHandlingSendEvent:(BOOL)handlingSendEvent {
    handlingSendEvent_ = handlingSendEvent;
}

- (void)sendEvent:(NSEvent*)event {
    CefScopedSendingEvent sendingEventScoper;
    [super sendEvent:event];
}

@end

void initMacApplication() {
    // Create our CEF-compatible NSApplication subclass
    // This must be done before SDL_Init, which would create a plain NSApplication
    [JellyfinApplication sharedApplication];

    // Make this a foreground app (shows in dock, gets menubar, receives keyboard)
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    // Create basic menu bar with Quit item for Cmd+Q
    NSMenu* menubar = [[NSMenu alloc] init];
    NSMenuItem* appMenuItem = [[NSMenuItem alloc] init];
    [menubar addItem:appMenuItem];
    NSMenu* appMenu = [[NSMenu alloc] init];
    NSMenuItem* quitItem = [[NSMenuItem alloc] initWithTitle:@"Quit"
                                                      action:@selector(terminate:)
                                               keyEquivalent:@"q"];
    [appMenu addItem:quitItem];
    [appMenuItem setSubmenu:appMenu];
    [NSApp setMainMenu:menubar];

    // Activate the app and bring to front
    [NSApp activateIgnoringOtherApps:YES];

    NSLog(@"NSApplication class: %@", NSStringFromClass([NSApp class]));
    NSLog(@"Conforms to CefAppProtocol: %@", [NSApp conformsToProtocol:@protocol(CefAppProtocol)] ? @"YES" : @"NO");
}

// Call this after SDL window is created to ensure it can receive keyboard input
void activateMacWindow() {
    [NSApp activateIgnoringOtherApps:YES];
    // Make the key window (SDL should have set this, but ensure it)
    if ([NSApp mainWindow]) {
        [[NSApp mainWindow] makeKeyAndOrderFront:nil];
    }
}
