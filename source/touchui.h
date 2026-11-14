#ifndef TOUCHUI_H
#define TOUCHUI_H

#include <3ds.h>
#include <citro2d.h>
#include "protocol.h"

#define BOTTOM_W 320.0f
#define BOTTOM_H 240.0f
#define BAR_H    24.0f        // height of the three mode buttons along the top

#define MAX_KEYS   100
#define IP_MAX_LEN 31         // longest host IP the "Change IP?" box accepts

typedef enum {
    UI_MODE_CONTROLLER = PKT_MODE_CONTROLLER,
    UI_MODE_MOUSE      = PKT_MODE_MOUSE,
    UI_MODE_KEYBOARD   = PKT_MODE_KEYBOARD,
} UiMode;

typedef struct {
    const char *label;       // what the key shows normally ("q", "1", "-")
    const char *shiftLabel;  // what it shows (and types) with Shift on ("Q", "!", "_"); NULL if unchanged
    u8    hid;               // USB HID usage ID (0xE0-0xE7 for the modifier keys)
    u8    modBit;            // nonzero: a toggle key (Ctrl, Alt, Shift, Win) using this MOD_* bit
    bool  isLetter;          // letters follow Caps Lock as well as Shift
    float x, y, w, h;
    C2D_Text text, shiftText;
    float textW, textH, shiftW, shiftH, textScale;
} UiKey;

typedef struct {
    UiMode mode;
    bool   isNew3DS;

    // Circle Pad / C-Stick behaviour in the mouse and keyboard modes
    bool   circleScroll;     // false: moves the cursor, true: scrolls
    bool   cstickScroll;

    // The touch currently on the screen
    int    target;           // what the touch started on (see TARGET_* in touchui.c)
    int    activeKey;        // index of the key being pressed, or -1
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

    // Letter case
    bool   capsLock;         // follows the PC's Caps Lock (flipped by every tap on the Caps key)
    bool   shiftHeld;        // L or R held down (they are Shift in keyboard mode)

    // "Change IP?" button and the text box on the top screen
    bool   changeIpVisible;  // the "Change IP?" button is showing under the Keyboard button
    bool   ipEntry;          // the box is open: the keyboard types into it instead of into the PC
    bool   ipCaps;           // Caps Lock for the box only (the PC never hears about it)
    bool   ipError;          // Enter was pressed on something that isn't an IPv4 address
    bool   ipSubmitted;      // a new IP is waiting for main.c to pick it up
    char   ipText[IP_MAX_LEN + 1];
    int    ipLen, ipCursor;
    char   ipSubmitText[IP_MAX_LEN + 1];
    char   currentIp[IP_MAX_LEN + 1];   // the IP in use now (the box starts with this)

    // Layout and text
    UiKey  keys[MAX_KEYS];
    int    numKeys;
    C2D_TextBuf textBuf;
    C2D_Text barText[3];
    float  barTextW[3], barTextH[3];
    C2D_Text chipText[4];    // Circle Pad: Move / Scroll, C-Stick: Move / Scroll
    float  chipTextW[4], chipTextH[4];
    C2D_Text changeIpText;
    float  changeIpW, changeIpH;
} TouchUI;

void touchui_init(TouchUI *ui, bool isNew3DS);
void touchui_exit(TouchUI *ui);

// Tell the UI which IP is in use (call at startup and after every change).
void touchui_set_current_ip(TouchUI *ui, const char *ip);

// Call once per frame. `down`, `held`, `up` come from hidKeysDown/Held/Up().
void touchui_update(TouchUI *ui, u32 down, u32 held, u32 up, touchPosition touch, u64 nowMs);

// Fills in the v2 part of the packet (mode, flags, mouse, keyboard).
void touchui_fill_packet(TouchUI *ui, ControllerPacket *pkt);

// True once, after a valid new IP was entered. Copies it to `out`.
bool touchui_take_submitted_ip(TouchUI *ui, char *out, size_t size);

// Color to clear the bottom screen with (depends on the mode).
u32  touchui_background(const TouchUI *ui);

// Draws the whole bottom screen. Call after C2D_SceneBegin(bottom).
void touchui_draw(const TouchUI *ui);

// Shared with main.c: a filled rectangle with rounded corners.
// `z` is the layer: higher draws in front (text sits at 0.5, plain shapes at 0).
void ui_draw_rounded_rect(float x, float y, float w, float h, float r, u32 clr);
void ui_draw_rounded_rect_z(float x, float y, float w, float h, float r, u32 clr, float z);

#endif
