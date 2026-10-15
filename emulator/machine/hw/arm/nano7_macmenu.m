/*
 * nano7_macmenu.m -- native macOS menu bar for the desktop front end
 * (NanoEmu.app): File / Emulation / View / Device menus added to the menu
 * bar SDL creates (it keeps its own application menu: About, Hide, Quit).
 *
 *   File       Save State (Cmd-S), Load State (Cmd-L), Reset (Cmd-R),
 *              Screenshot (Cmd-E)
 *   Emulation  Pause (Cmd-P), Mute (Shift-Cmd-M), Speed 1x / 2x / 4x, Clock Profile (Stock /
 *              Safe / Max; none checked = the guest's own clocks), Gamepad
 *              Keys (Cmd-G)
 *   View       Size 100 % / 150 % / 200 % / 300 % (Cmd-1 / 2 / 3 / 4),
 *              Rotate (Cmd-T), Device Frame (Cmd-D), Usage HUD (Cmd-U),
 *              Full Screen (also Window > Toggle Full Screen, Ctrl-Cmd-F)
 *   Device     Home, Sleep/Wake, Play/Pause, Volume Up / Down (taps)
 *
 * Sizes are in points: the SDL window draws at the display's backing scale
 * (machine/patches/sdl-hidpi.patch), so 100 % on a Retina display is pixel
 * zoom 2 and 150 % is zoom 3 - whole device pixels either way. Items whose
 * size would not be a whole pixel zoom are left out.
 *
 * Every action goes through nano7_ui_command() (queued, run in QEMU's main
 * loop); check marks come from nano7_ui_state(). The menu is installed by
 * a QEMU timer once SDL has created the application and its menu bar
 * (never with -display none). NANO7_MENU_SELFTEST=1 fires the harmless
 * items once after installing and logs them (tests over ssh).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#import <Cocoa/Cocoa.h>
#include "nano7_macmenu.h"
#include "nano7_ui.h"
#include "nano7_pmu.h"
#include "nano7_power.h"

#define TAG(cmd, arg)   ((cmd) * 100 + (arg) + 10)
#define TAG_CMD(t)      ((int)(t) / 100)
#define TAG_ARG(t)      ((int)(t) % 100 - 10)

@interface N7MenuTarget : NSObject <NSMenuItemValidation>
@end

@implementation N7MenuTarget
- (void)run:(NSMenuItem *)item
{
    nano7_ui_command(TAG_CMD(item.tag), TAG_ARG(item.tag));
}

- (BOOL)validateMenuItem:(NSMenuItem *)item
{
    int cmd = TAG_CMD(item.tag), arg = TAG_ARG(item.tag);
    BOOL on = NO;

    switch (cmd) {
    case NANO7_UI_PAUSE:
    case NANO7_UI_HUD:
    case NANO7_UI_FRAME:
    case NANO7_UI_PADKEYS:
    case NANO7_UI_MUTE:
        on = nano7_ui_state(cmd) != 0;
        break;
    case NANO7_UI_ROTATE:
        on = arg && nano7_ui_state(cmd) / 90 + 1 == arg;
        break;
    case NANO7_UI_ZOOM:
    case NANO7_UI_SPEED:
    case NANO7_UI_PROFILE:
        on = nano7_ui_state(cmd) == arg;
        break;
    default:
        break;
    }
    item.state = on ? NSControlStateValueOn : NSControlStateValueOff;
    return YES;
}
@end

static N7MenuTarget *n7_target;
static QEMUTimer *n7_install_timer;
static int n7_tries;

static NSMenuItem *n7_item(NSMenu *m, NSString *title, int cmd, int arg,
                           NSString *key, NSEventModifierFlags mods)
{
    NSMenuItem *it = [[NSMenuItem alloc] initWithTitle:title
                                                action:@selector(run:)
                                         keyEquivalent:key ? key : @""];

    it.target = n7_target;
    it.tag = TAG(cmd, arg);
    if (key) {
        it.keyEquivalentModifierMask = mods;
    }
    [m addItem:it];
    return it;
}

static NSMenu *n7_submenu(NSMenu *parent, NSString *title)
{
    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:title action:nil
                                             keyEquivalent:@""];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];

    m.autoenablesItems = YES;
    holder.submenu = m;
    [parent addItem:holder];
    return m;
}

/* a top-level menu, placed before SDL's Window menu (macOS order: app,
 * File, ..., View, ..., Window, Help) */
static NSMenu *n7_top(NSMenu *bar, NSString *title)
{
    NSInteger at = [bar indexOfItemWithTitle:@"Window"];
    NSMenuItem *holder = [[NSMenuItem alloc] initWithTitle:title action:nil
                                             keyEquivalent:@""];
    NSMenu *m = [[NSMenu alloc] initWithTitle:title];

    holder.submenu = m;
    if (at < 0) {
        [bar addItem:holder];
    } else {
        [bar insertItem:holder atIndex:at];
    }
    return m;
}

static void n7_build(NSMenu *bar, int scale)
{
    const NSEventModifierFlags cmd = NSEventModifierFlagCommand;
    NSMenu *m, *sub;
    NSMenuItem *fs;
    static const struct { int pct; NSString *key; } sizes[] = {
        { 100, @"1" }, { 150, @"2" }, { 200, @"3" }, { 300, @"4" },
    };
    unsigned i;

    m = n7_top(bar, @"File");
    n7_item(m, @"Save State", NANO7_UI_SAVE, 0, @"s", cmd);
    n7_item(m, @"Load State", NANO7_UI_LOAD, 0, @"l", cmd);
    [m addItem:[NSMenuItem separatorItem]];
    n7_item(m, @"Reset", NANO7_UI_RESET, 0, @"r", cmd);
    [m addItem:[NSMenuItem separatorItem]];
    n7_item(m, @"Save Screenshot", NANO7_UI_SHOT, 0, @"e", cmd);

    m = n7_top(bar, @"Emulation");
    n7_item(m, @"Pause", NANO7_UI_PAUSE, 0, @"p", cmd);
    n7_item(m, @"Mute", NANO7_UI_MUTE, 0, @"m", cmd | NSEventModifierFlagShift);
    [m addItem:[NSMenuItem separatorItem]];
    sub = n7_submenu(m, @"Speed");
    n7_item(sub, @"Normal (1x)", NANO7_UI_SPEED, 1, nil, 0);
    n7_item(sub, @"Fast (2x)", NANO7_UI_SPEED, 2, nil, 0);
    n7_item(sub, @"Fastest (4x)", NANO7_UI_SPEED, 4, nil, 0);
    sub = n7_submenu(m, @"Clock Profile");
    n7_item(sub, @"Stock (342 / 133 MHz)", NANO7_UI_PROFILE,
            NANO7_PROFILE_STOCK, nil, 0);
    n7_item(sub, @"Safe (684 / 171 MHz)", NANO7_UI_PROFILE,
            NANO7_PROFILE_SAFE, nil, 0);
    n7_item(sub, @"Max (684 / 266 MHz)", NANO7_UI_PROFILE,
            NANO7_PROFILE_MAX, nil, 0);
    [m addItem:[NSMenuItem separatorItem]];
    n7_item(m, @"Gamepad Keys", NANO7_UI_PADKEYS, 0, @"g", cmd);

    m = n7_top(bar, @"View");
    for (i = 0; i < ARRAY_SIZE(sizes); i++) {
        int z2 = sizes[i].pct * scale;  /* pixel zoom x 100 */

        if (z2 % 100 == 0) {
            n7_item(m, [NSString stringWithFormat:@"Size %d %%",
                        sizes[i].pct], NANO7_UI_ZOOM, z2 / 100,
                    sizes[i].key, cmd);
        }
    }
    [m addItem:[NSMenuItem separatorItem]];
    n7_item(m, @"Rotate", NANO7_UI_ROTATE, 0, @"t", cmd);
    sub = n7_submenu(m, @"Rotation");
    n7_item(sub, @"0\u00b0 (Portrait)", NANO7_UI_ROTATE, 1, nil, 0);
    n7_item(sub, @"90\u00b0", NANO7_UI_ROTATE, 2, nil, 0);
    n7_item(sub, @"180\u00b0", NANO7_UI_ROTATE, 3, nil, 0);
    n7_item(sub, @"270\u00b0", NANO7_UI_ROTATE, 4, nil, 0);
    n7_item(m, @"Device Frame", NANO7_UI_FRAME, 0, @"d", cmd);
    n7_item(m, @"Side Panel (HUD)", NANO7_UI_HUD, 0, @"u", cmd);
    [m addItem:[NSMenuItem separatorItem]];
    /* the window's own full screen (target nil: the key window) */
    fs = [[NSMenuItem alloc] initWithTitle:@"Full Screen"
                                    action:@selector(toggleFullScreen:)
                             keyEquivalent:@""];
    [m addItem:fs];

    m = n7_top(bar, @"Device");
    n7_item(m, @"Home", NANO7_UI_BUTTON, NANO7_BTN_HOME, nil, 0);
    n7_item(m, @"Sleep / Wake", NANO7_UI_BUTTON, NANO7_BTN_POWER, nil, 0);
    n7_item(m, @"Play / Pause", NANO7_UI_BUTTON, NANO7_BTN_PLAY, nil, 0);
    [m addItem:[NSMenuItem separatorItem]];
    n7_item(m, @"Volume Up", NANO7_UI_BUTTON, NANO7_BTN_VOLUP, nil, 0);
    n7_item(m, @"Volume Down", NANO7_UI_BUTTON, NANO7_BTN_VOLDOWN, nil, 0);
}

/* fire the harmless items (no reset / load / pause) and log the menus */
static void n7_selftest(NSMenu *bar)
{
    for (NSMenuItem *top in bar.itemArray) {
        NSMutableString *line = [NSMutableString stringWithFormat:@"%@:",
                                 top.title];

        for (NSMenuItem *it in top.submenu.itemArray) {
            if (!it.separatorItem) {
                [line appendFormat:@" [%@]", it.title];
            }
        }
        fprintf(stderr, "[nano7-menu] %s\n", line.UTF8String);
    }
    nano7_ui_command(NANO7_UI_HUD, 0);
    nano7_ui_command(NANO7_UI_HUD, 0);
    nano7_ui_command(NANO7_UI_PROFILE, NANO7_PROFILE_SAFE);
    nano7_ui_command(NANO7_UI_BUTTON, NANO7_BTN_VOLUP);
    nano7_ui_command(NANO7_UI_SHOT, 0);
}

static void n7_try_install(void *opaque)
{
    NSMenu *bar;
    NSWindow *win;
    int scale;

    if (!NSApp || !(bar = NSApp.mainMenu) || !NSApp.windows.count) {
        if (++n7_tries < 120) {         /* 30 s, then: no window (headless) */
            timer_mod(n7_install_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 250);
        }
        return;
    }
    win = NSApp.windows.firstObject;
    scale = MAX(1, (int)lround(win.backingScaleFactor));
    n7_target = [[N7MenuTarget alloc] init];
    n7_build(bar, scale);
    fprintf(stderr, "[nano7-menu] menu bar installed (backing scale %d, %s "
            "thread)\n", scale, [NSThread isMainThread] ? "main" : "OTHER");
    if (getenv("NANO7_MENU_SELFTEST")) {
        n7_selftest(bar);
    }
}

void nano7_macmenu_init(void)
{
    n7_install_timer = timer_new_ms(QEMU_CLOCK_REALTIME, n7_try_install,
                                    NULL);
    timer_mod(n7_install_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 250);
}
