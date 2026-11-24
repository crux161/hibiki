#import <Cocoa/Cocoa.h>
#include <SDL3/SDL.h>
#include "hibiki/app_menu.h"

@interface HibikiMenuTarget : NSObject
@property uint32_t eventType;
- (void)activate:(NSMenuItem*)sender;
@end

@implementation HibikiMenuTarget
- (void)activate:(NSMenuItem*)sender {
    SDL_Event event{};
    event.type = self.eventType;
    event.user.code = static_cast<int>(sender.tag);
    SDL_PushEvent(&event);
}
@end

void hibiki::install_app_menu(uint32_t event_type) {
    static HibikiMenuTarget* target;
    target = [HibikiMenuTarget new];
    target.eventType = event_type;
    NSMenu* main = NSApp.mainMenu;
    if (!main.numberOfItems) return;
    NSMenu* app = [main itemAtIndex:0].submenu;
    NSMenuItem* settings = nil;
    for (NSMenuItem* item in app.itemArray) {
        if (item.action == @selector(orderFrontStandardAboutPanel:)) {
            item.title = @"About Hibiki";
            item.target = target;
            item.action = @selector(activate:);
            item.tag = static_cast<int>(AppAction::about);
        }
        if ([item.keyEquivalent isEqualToString:@","]) settings = item;
    }
    NSMenuItem* firmware = [[NSMenuItem alloc] initWithTitle:@"Firmware Setup…"
        action:@selector(activate:) keyEquivalent:@","];
    firmware.target = target;
    firmware.tag = static_cast<int>(AppAction::firmware);
    if (settings) {
        NSInteger index = [app indexOfItem:settings];
        [app removeItem:settings];
        [app insertItem:firmware atIndex:index];
    } else [app insertItem:firmware atIndex:MIN(2, app.numberOfItems)];

    NSMenu* help = [[NSMenu alloc] initWithTitle:@"Help"];
    NSMenuItem* shortcuts = [[NSMenuItem alloc] initWithTitle:@"Keyboard Shortcuts"
        action:@selector(activate:) keyEquivalent:@"?"];
    shortcuts.target = target;
    shortcuts.tag = static_cast<int>(AppAction::shortcuts);
    [help addItem:shortcuts];
    NSMenuItem* scan = [[NSMenuItem alloc] initWithTitle:@"Scan Channels"
        action:@selector(activate:) keyEquivalent:@""];
    scan.target = target;
    scan.tag = static_cast<int>(AppAction::rescan);
    [help addItem:scan];
    NSMenuItem* root = [[NSMenuItem alloc] initWithTitle:@"Help" action:nil keyEquivalent:@""];
    root.submenu = help;
    [main addItem:root];
    NSApp.helpMenu = help;
}
