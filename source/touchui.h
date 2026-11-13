#ifndef TOUCHUI_H
#define TOUCHUI_H

#include <3ds.h>
#include <citro2d.h>
#include "protocol.h"

#define BOTTOM_W 320.0f
#define BOTTOM_H 240.0f
#define BAR_H    24.0f        // height of the three mode buttons along the top

#define MAX_KEYS 100

typedef enum {
    UI_MODE_CONTROLLER = PKT_MODE_CONTROLLER,
    UI_MODE_MOUSE      = PKT_MODE_MOUSE,
    UI_MODE_KEYBOARD   = PKT_MODE_KEYBOARD,
} UiMode;

typedef struct {
    const char *label;
    u8    hid;       // USB HID usage ID (0xE0-0xE7 for the modifier keys)
    u8    modBit;    // nonzero: a toggle key (Ctrl, Alt, Shift, Win) using this MOD_* bit
    float x, y, w, h;
    C2D_Text text;
    float textW, textH, textScale;
} UiKey;

typedef struct {
    UiMode mode;
    bool   isNew3DS;

    // Circle Pad / C-Stick behaviour in the mouse and keyboard modes
    bool   circleScroll;     // false: moves the cursor, true: scrolls
    bool   cstickScroll;

    // The touch currently on the screen
    int    target;           // what the touch started on (see TARGET_* in touchui.c)
    int    activeKey;        // index of the key being held, or -1
    int    startX, startY, lastX, lastY;
    u64    touchStartMs;
    bool   padMoved;
    float  accX, accY;       // cursor movement not yet sent (fractions of a pixel)

    // What goes into the next packet
    int    tapFrames;        // frames left to hold the left button after a tap
    u8     latchedMods;      // toggle keys that are lit
    u8     appliedMods;      // modifiers applied to the key being held
    u8     heldKey;          // HID code of the key being held (0 = none)
    u8     heldFrames;
    bool   releasePending;
    u8     winPulseFrames;   // frames left for a lone Windows-key tap

    // Layout and text
    UiKey  keys[MAX_KEYS];
    int    numKeys;
    C2D_TextBuf textBuf;
    C2D_Text barText[3];
    float  barTextW[3], barTextH[3];
    C2D_Text chipText[4];    // Circle Pad: Move / Scroll, C-Stick: Move / Scroll
    float  chipTextW[4], chipTextH[4];
} TouchUI;

void touchui_init(TouchUI *ui, bool isNew3DS);
void touchui_exit(TouchUI *ui);

// Call once per frame. `down`, `held`, `up` come from hidKeysDown/Held/Up().
void touchui_update(TouchUI *ui, u32 down, u32 held, u32 up, touchPosition touch, u64 nowMs);

// Fills in the v2.0.0 part of the packet (mode, flags, mouse, keyboard).
void touchui_fill_packet(TouchUI *ui, ControllerPacket *pkt);

// Color to clear the bottom screen with (depends on the mode).
u32  touchui_background(const TouchUI *ui);

// Draws the whole bottom screen. Call after C2D_SceneBegin(bottom).
void touchui_draw(const TouchUI *ui);

// Shared with main.c: a filled rectangle with rounded corners.
void ui_draw_rounded_rect(float x, float y, float w, float h, float r, u32 clr);

#endif
