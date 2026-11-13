#ifndef PROTOCOL_H
#define PROTOCOL_H

// The packet the 3DS sends to the PC every frame (about 60 times a second).
// pc.py has a matching description of this layout: keep the two in sync.
//
// Version 2.0.0 grew the packet from 12 bytes to 22. The first 12 bytes are
// exactly the old v1.x packet, so pc.py can still understand an old 3DS app.

#include <3ds.h>

// ---- Modes ----
#define PKT_MODE_CONTROLLER 0   // bottom screen "off": the 3DS is a gamepad
#define PKT_MODE_MOUSE      1   // bottom screen is a trackpad
#define PKT_MODE_KEYBOARD   2   // bottom screen is a keyboard

// ---- flags ----
#define PKT_FLAG_CIRCLE_SCROLL (1 << 0)   // Circle Pad scrolls instead of moving the cursor
#define PKT_FLAG_CSTICK_SCROLL (1 << 1)   // C-Stick scrolls instead of moving the cursor
#define PKT_FLAG_NEW3DS        (1 << 2)   // this is a "New" model (has ZL, ZR and a C-Stick)

// ---- mouseButtons (buttons pressed by touching the screen) ----
#define PKT_MOUSE_LEFT (1 << 0)           // a tap on the trackpad

// ---- modifiers: bit n is the USB HID key 0xE0 + n ----
#define MOD_LCTRL  (1 << 0)   // 0xE0
#define MOD_LSHIFT (1 << 1)   // 0xE1
#define MOD_LALT   (1 << 2)   // 0xE2
#define MOD_LWIN   (1 << 3)   // 0xE3
#define MOD_RCTRL  (1 << 4)   // 0xE4
#define MOD_RSHIFT (1 << 5)   // 0xE5
#define MOD_RALT   (1 << 6)   // 0xE6
#define MOD_RWIN   (1 << 7)   // 0xE7 (no key for it on the 3DS keyboard, reserved)

// All numbers are little-endian, with no padding between fields.
typedef struct __attribute__((packed)) {
    // ---- the original v1.x part (12 bytes) ----
    u32 buttons;        // hidKeysHeld(): one bit per 3DS button
    s16 circleX;        // Circle Pad, about -156 to 156
    s16 circleY;        // (up is positive)
    s16 cstickX;        // C-Stick (New 3DS only; 0 otherwise)
    s16 cstickY;

    // ---- new in v2.0.0 (10 bytes) ----
    u8  mode;           // PKT_MODE_*
    u8  flags;          // PKT_FLAG_*
    s16 mouseDX;        // trackpad movement since the last packet, in cursor pixels
    s16 mouseDY;
    u8  mouseButtons;   // PKT_MOUSE_*
    u8  modifiers;      // MOD_* held down right now
    u8  key;            // USB HID usage ID of the typed key (0 = none)
    u8  reserved;
} ControllerPacket;

_Static_assert(sizeof(ControllerPacket) == 22, "ControllerPacket must be exactly 22 bytes");

#endif
