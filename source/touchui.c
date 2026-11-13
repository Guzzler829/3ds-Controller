#include "touchui.h"
#include <string.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Tuning
// ---------------------------------------------------------------------------
#define MOUSE_SENSITIVITY  2.0f   // cursor pixels per touch pixel (before acceleration)
#define MOUSE_ACCEL        0.04f  // extra speed for fast swipes (0 = none)
#define PAD_SLOP_PX        5      // movement smaller than this still counts as a "tap"
#define TAP_MAX_MS         220    // a touch shorter than this (without moving) is a click
#define TAP_CLICK_FRAMES   4      // how long the left button is held for a tap (frames)
#define KEY_MIN_FRAMES     3      // a typed key is held at least this many frames
#define WIN_TAP_FRAMES     4      // how long a lone Windows-key tap is held (frames)

// ---------------------------------------------------------------------------
// Layout (bottom screen is 320 x 240)
// ---------------------------------------------------------------------------
#define PAD_BOTTOM    204.0f   // the trackpad ends here; the two toggles sit below it
#define CHIP_Y        208.0f
#define CHIP_H        26.0f
#define KEYBOARD_TOP  26.0f
#define KEY_UNIT      (BOTTOM_W / 15.0f)   // the keyboard is 15 key-widths wide

enum { TARGET_NONE, TARGET_BAR, TARGET_PAD, TARGET_CHIP, TARGET_KEY };

#define CLR(r, g, b) C2D_Color32((r), (g), (b), 255)

// ---------------------------------------------------------------------------
// Keyboard layout
//   Each key: label, USB HID usage ID (what the PC receives), width in key-units.
//   Toggle keys (Ctrl, Alt, Shift, Win) also carry the MOD_* bit they latch.
//   Every row adds up to 15 units wide.
// ---------------------------------------------------------------------------
typedef struct { const char *label; u8 hid; u8 modBit; float units; } KeyDef;
typedef struct { const KeyDef *keys; int count; float height; } RowDef;

#define K(label, hid, units)        { label, hid, 0, units }
#define M(label, hid, bit, units)   { label, hid, bit, units }
#define WIN_LABEL "#WIN"            // drawn as a 4-square logo instead of text

static const KeyDef ROW0[] = {
    K("Esc", 0x29, 1.0f),
    K("F1", 0x3A, 1.0f), K("F2", 0x3B, 1.0f), K("F3", 0x3C, 1.0f), K("F4", 0x3D, 1.0f),
    K("F5", 0x3E, 1.0f), K("F6", 0x3F, 1.0f), K("F7", 0x40, 1.0f), K("F8", 0x41, 1.0f),
    K("F9", 0x42, 1.0f), K("F10", 0x43, 1.0f), K("F11", 0x44, 1.0f), K("F12", 0x45, 1.0f),
    K("Del", 0x4C, 2.0f),
};
static const KeyDef ROW1[] = {
    K("`", 0x35, 1.0f), K("1", 0x1E, 1.0f), K("2", 0x1F, 1.0f), K("3", 0x20, 1.0f),
    K("4", 0x21, 1.0f), K("5", 0x22, 1.0f), K("6", 0x23, 1.0f), K("7", 0x24, 1.0f),
    K("8", 0x25, 1.0f), K("9", 0x26, 1.0f), K("0", 0x27, 1.0f), K("-", 0x2D, 1.0f),
    K("=", 0x2E, 1.0f), K("Bksp", 0x2A, 2.0f),
};
static const KeyDef ROW2[] = {
    K("Tab", 0x2B, 1.5f), K("Q", 0x14, 1.0f), K("W", 0x1A, 1.0f), K("E", 0x08, 1.0f),
    K("R", 0x15, 1.0f), K("T", 0x17, 1.0f), K("Y", 0x1C, 1.0f), K("U", 0x18, 1.0f),
    K("I", 0x0C, 1.0f), K("O", 0x12, 1.0f), K("P", 0x13, 1.0f), K("[", 0x2F, 1.0f),
    K("]", 0x30, 1.0f), K("\\", 0x31, 1.5f),
};
static const KeyDef ROW3[] = {
    K("Caps", 0x39, 1.75f), K("A", 0x04, 1.0f), K("S", 0x16, 1.0f), K("D", 0x07, 1.0f),
    K("F", 0x09, 1.0f), K("G", 0x0A, 1.0f), K("H", 0x0B, 1.0f), K("J", 0x0D, 1.0f),
    K("K", 0x0E, 1.0f), K("L", 0x0F, 1.0f), K(";", 0x33, 1.0f), K("'", 0x34, 1.0f),
    K("Enter", 0x28, 2.25f),
};
static const KeyDef ROW4[] = {
    M("Shift", 0xE1, MOD_LSHIFT, 2.25f),
    K("Z", 0x1D, 1.0f), K("X", 0x1B, 1.0f), K("C", 0x06, 1.0f), K("V", 0x19, 1.0f),
    K("B", 0x05, 1.0f), K("N", 0x11, 1.0f), K("M", 0x10, 1.0f), K(",", 0x36, 1.0f),
    K(".", 0x37, 1.0f), K("/", 0x38, 1.0f),
    M("Shift", 0xE5, MOD_RSHIFT, 2.75f),
};
static const KeyDef ROW5[] = {
    M("Ctrl", 0xE0, MOD_LCTRL, 1.5f), M(WIN_LABEL, 0xE3, MOD_LWIN, 1.5f),
    M("Alt", 0xE2, MOD_LALT, 1.5f), K("Space", 0x2C, 6.0f),
    M("Alt", 0xE6, MOD_RALT, 1.5f), K("Menu", 0x65, 1.5f),
    M("Ctrl", 0xE4, MOD_RCTRL, 1.5f),
};
static const KeyDef ROW6[] = {
    K("Ins", 0x49, 1.5f), K("Home", 0x4A, 1.5f), K("PgUp", 0x4B, 1.5f),
    K("PgDn", 0x4E, 1.5f), K("End", 0x4D, 1.5f),
    K("<", 0x50, 1.875f), K("v", 0x51, 1.875f), K("^", 0x52, 1.875f), K(">", 0x4F, 1.875f),
};

#define ROW(arr, h) { arr, (int)(sizeof(arr) / sizeof((arr)[0])), h }
static const RowDef ROWS[] = {
    ROW(ROW0, 20.0f), ROW(ROW1, 31.0f), ROW(ROW2, 31.0f), ROW(ROW3, 31.0f),
    ROW(ROW4, 31.0f), ROW(ROW5, 31.0f), ROW(ROW6, 26.0f),
};
#define NUM_ROWS ((int)(sizeof(ROWS) / sizeof(ROWS[0])))

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void fillRect(float x, float y, float w, float h, u32 clr)
{
    C2D_DrawRectSolid(x, y, 0.0f, w, h, clr);
}

static void drawLabel(const C2D_Text *t, float x, float y, float scale, u32 clr)
{
    C2D_DrawText(t, C2D_WithColor, x, y, 0.5f, scale, scale, clr);
}

void ui_draw_rounded_rect(float x, float y, float w, float h, float r, u32 clr)
{
    C2D_DrawRectSolid(x + r, y,     0.0f, w - 2.0f * r, h, clr);
    C2D_DrawRectSolid(x,     y + r, 0.0f, r, h - 2.0f * r, clr);
    C2D_DrawRectSolid(x + w - r, y + r, 0.0f, r, h - 2.0f * r, clr);
    C2D_DrawCircleSolid(x + r,     y + r,     0.0f, r, clr);
    C2D_DrawCircleSolid(x + w - r, y + r,     0.0f, r, clr);
    C2D_DrawCircleSolid(x + r,     y + h - r, 0.0f, r, clr);
    C2D_DrawCircleSolid(x + w - r, y + h - r, 0.0f, r, clr);
}

// Three simple monochrome icons, each about 14 x 14 pixels.
// `fg` is the icon color; `bg` is the button color behind it (used for the cut-out details).
static void drawIconController(float x, float y, u32 fg, u32 bg)
{
    ui_draw_rounded_rect(x, y + 3.0f, 14.0f, 8.0f, 3.0f, fg);            // body
    C2D_DrawCircleSolid(x + 3.5f,  y + 9.5f, 0.0f, 2.8f, fg);            // the two grips
    C2D_DrawCircleSolid(x + 10.5f, y + 9.5f, 0.0f, 2.8f, fg);
    fillRect(x + 3.0f, y + 6.2f, 3.4f, 1.2f, bg);                        // D-pad
    fillRect(x + 4.1f, y + 5.1f, 1.2f, 3.4f, bg);
    C2D_DrawCircleSolid(x + 9.2f,  y + 6.0f, 0.0f, 0.9f, bg);            // two buttons
    C2D_DrawCircleSolid(x + 10.8f, y + 7.6f, 0.0f, 0.9f, bg);
}

static void drawIconMouse(float x, float y, u32 fg, u32 bg)
{
    ui_draw_rounded_rect(x + 3.0f, y, 8.0f, 14.0f, 4.0f, fg);            // body
    fillRect(x + 6.5f, y + 1.0f, 1.0f, 5.0f, bg);                        // button divider
    fillRect(x + 3.0f, y + 6.0f, 8.0f, 1.0f, bg);
}

static void drawIconKeyboard(float x, float y, u32 fg, u32 bg)
{
    ui_draw_rounded_rect(x, y + 2.0f, 14.0f, 10.0f, 2.0f, fg);           // body
    for (int i = 0; i < 4; i++)                                          // top row of keys
        fillRect(x + 2.0f + 3.0f * i, y + 4.0f, 1.6f, 1.6f, bg);
    for (int i = 0; i < 3; i++)                                          // middle row
        fillRect(x + 3.5f + 3.0f * i, y + 6.6f, 1.6f, 1.6f, bg);
    fillRect(x + 4.0f, y + 9.4f, 6.0f, 1.4f, bg);                        // space bar
}

static void drawBar(const TouchUI *ui)
{
    const float bw = BOTTOM_W / 3.0f;
    for (int i = 0; i < 3; i++)
    {
        const bool active = ((int)ui->mode == i);
        const u32 bg = active ? CLR(92, 92, 102) : CLR(38, 38, 44);
        const u32 fg = active ? CLR(255, 255, 255) : CLR(190, 190, 198);
        const float x = bw * i;

        fillRect(x, 0.0f, bw, BAR_H, bg);
        fillRect(x + bw - 1.0f, 0.0f, 1.0f, BAR_H, CLR(0, 0, 0));        // thin divider

        // icon + label, centered together inside the button
        const float groupW = 14.0f + 5.0f + ui->barTextW[i];
        const float gx = x + (bw - groupW) / 2.0f;
        const float iy = (BAR_H - 14.0f) / 2.0f;
        if (i == 0)      drawIconController(gx, iy, fg, bg);
        else if (i == 1) drawIconMouse(gx, iy, fg, bg);
        else             drawIconKeyboard(gx, iy, fg, bg);
        drawLabel(&ui->barText[i], gx + 19.0f, (BAR_H - ui->barTextH[i]) / 2.0f, 0.45f, fg);
    }
}

// ---------------------------------------------------------------------------
// The two settings toggles at the bottom of the mouse screen
// ---------------------------------------------------------------------------
static bool chipRect(const TouchUI *ui, int idx, float *x, float *y, float *w, float *h)
{
    if (idx == 1 && !ui->isNew3DS)
        return false;                 // an old 3DS has no C-Stick, so no toggle for it
    *y = CHIP_Y;
    *h = CHIP_H;
    if (ui->isNew3DS)
    {
        *w = 150.0f;
        *x = (idx == 0) ? 6.0f : 164.0f;
    }
    else
    {
        *w = 200.0f;
        *x = 60.0f;
    }
    return true;
}

static int chipAt(const TouchUI *ui, int px, int py)
{
    for (int i = 0; i < 2; i++)
    {
        float x, y, w, h;
        if (chipRect(ui, i, &x, &y, &w, &h) && px >= x && px < x + w && py >= y && py < y + h)
            return i;
    }
    return -1;
}

static void drawChips(const TouchUI *ui)
{
    for (int i = 0; i < 2; i++)
    {
        float x, y, w, h;
        if (!chipRect(ui, i, &x, &y, &w, &h))
            continue;
        const bool scroll = (i == 0) ? ui->circleScroll : ui->cstickScroll;
        const int t = i * 2 + (scroll ? 1 : 0);
        ui_draw_rounded_rect(x, y, w, h, 5.0f, CLR(46, 46, 54));
        drawLabel(&ui->chipText[t], x + (w - ui->chipTextW[t]) / 2.0f,
                  y + (h - ui->chipTextH[t]) / 2.0f, 0.45f, CLR(225, 225, 232));
    }
}

// ---------------------------------------------------------------------------
// Keyboard drawing
// ---------------------------------------------------------------------------
static void drawKeyboard(const TouchUI *ui)
{
    for (int i = 0; i < ui->numKeys; i++)
    {
        const UiKey *k = &ui->keys[i];
        const bool pressed = (ui->heldKey != 0 && ui->activeKey == i);
        const bool lit = (k->modBit != 0) && (ui->latchedMods & k->modBit);

        u32 clr = k->modBit ? CLR(48, 48, 58) : CLR(64, 64, 74);
        if (lit)     clr = CLR(70, 140, 230);
        if (pressed) clr = CLR(130, 130, 148);

        // draw slightly smaller than the key's cell, so neighbours have a gap between them
        fillRect(k->x + 1.0f, k->y + 1.0f, k->w - 2.0f, k->h - 2.0f, clr);

        if (strcmp(k->label, WIN_LABEL) == 0)
        {
            // generic four-square logo
            const float s = 4.6f, g = 1.4f;
            const float lx = k->x + (k->w - (2.0f * s + g)) / 2.0f;
            const float ly = k->y + (k->h - (2.0f * s + g)) / 2.0f;
            for (int a = 0; a < 2; a++)
                for (int b = 0; b < 2; b++)
                    fillRect(lx + a * (s + g), ly + b * (s + g), s, s, CLR(240, 240, 245));
        }
        else
        {
            drawLabel(&k->text, k->x + (k->w - k->textW) / 2.0f,
                      k->y + (k->h - k->textH) / 2.0f, k->textScale, CLR(240, 240, 245));
        }
    }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------
static void layoutKeys(TouchUI *ui)
{
    float y = KEYBOARD_TOP;
    ui->numKeys = 0;

    for (int r = 0; r < NUM_ROWS; r++)
    {
        float x = 0.0f;
        for (int c = 0; c < ROWS[r].count; c++)
        {
            if (ui->numKeys >= MAX_KEYS)
                return;
            const KeyDef *def = &ROWS[r].keys[c];
            UiKey *k = &ui->keys[ui->numKeys++];

            k->label  = def->label;
            k->hid    = def->hid;
            k->modBit = def->modBit;
            k->x = x;
            k->y = y;
            k->w = def->units * KEY_UNIT;
            k->h = ROWS[r].height;
            x += k->w;

            if (strcmp(def->label, WIN_LABEL) != 0)
            {
                k->textScale = (strlen(def->label) == 1) ? 0.5f : 0.38f;
                C2D_TextParse(&k->text, ui->textBuf, def->label);
                C2D_TextOptimize(&k->text);
                C2D_TextGetDimensions(&k->text, k->textScale, k->textScale, &k->textW, &k->textH);
            }
        }
        y += ROWS[r].height + 1.0f;
    }
}

void touchui_init(TouchUI *ui, bool isNew3DS)
{
    memset(ui, 0, sizeof(*ui));
    ui->mode = UI_MODE_CONTROLLER;      // starts as a plain gamepad, bottom screen "off"
    ui->isNew3DS = isNew3DS;
    ui->circleScroll = false;           // Circle Pad moves the cursor...
    ui->cstickScroll = true;            // ...and the C-Stick scrolls (change with the toggles)
    ui->activeKey = -1;
    ui->textBuf = C2D_TextBufNew(1024);

    static const char *barLabels[3] = { "Controller", "Mouse", "Keyboard" };
    for (int i = 0; i < 3; i++)
    {
        C2D_TextParse(&ui->barText[i], ui->textBuf, barLabels[i]);
        C2D_TextOptimize(&ui->barText[i]);
        C2D_TextGetDimensions(&ui->barText[i], 0.45f, 0.45f, &ui->barTextW[i], &ui->barTextH[i]);
    }

    static const char *chipLabels[4] = {
        "Circle Pad: Move", "Circle Pad: Scroll", "C-Stick: Move", "C-Stick: Scroll"
    };
    for (int i = 0; i < 4; i++)
    {
        C2D_TextParse(&ui->chipText[i], ui->textBuf, chipLabels[i]);
        C2D_TextOptimize(&ui->chipText[i]);
        C2D_TextGetDimensions(&ui->chipText[i], 0.45f, 0.45f, &ui->chipTextW[i], &ui->chipTextH[i]);
    }

    layoutKeys(ui);
}

void touchui_exit(TouchUI *ui)
{
    C2D_TextBufDelete(ui->textBuf);
}

// ---------------------------------------------------------------------------
// Input handling
// ---------------------------------------------------------------------------
static void releaseKey(TouchUI *ui)
{
    ui->heldKey = 0;
    ui->appliedMods = 0;
    ui->heldFrames = 0;
    ui->releasePending = false;
    ui->activeKey = -1;
}

// Forget everything in flight (used when the mode changes).
static void resetInputs(TouchUI *ui)
{
    ui->padMoved = false;
    ui->accX = ui->accY = 0.0f;
    ui->tapFrames = 0;
    ui->latchedMods = 0;
    ui->winPulseFrames = 0;
    releaseKey(ui);
}

static void setMode(TouchUI *ui, UiMode mode)
{
    if (ui->mode == mode)
        return;
    ui->mode = mode;
    resetInputs(ui);
}

static int keyAt(const TouchUI *ui, int px, int py)
{
    for (int i = 0; i < ui->numKeys; i++)
    {
        const UiKey *k = &ui->keys[i];
        if (px >= k->x && px < k->x + k->w && py >= k->y && py < k->y + k->h)
            return i;
    }
    return -1;
}

static void pressKey(TouchUI *ui, int idx)
{
    const UiKey *k = &ui->keys[idx];

    if (k->modBit)
    {
        // A toggle key: lights up (or turns off) and waits for the next regular key.
        // Tapping an already-lit Windows key sends a lone Windows-key press instead
        // (this is how you open the Start menu).
        if (k->modBit == MOD_LWIN && (ui->latchedMods & MOD_LWIN))
        {
            ui->latchedMods &= ~MOD_LWIN;
            ui->winPulseFrames = WIN_TAP_FRAMES;
        }
        else
        {
            ui->latchedMods ^= k->modBit;
        }
        return;
    }

    // A regular key: the lit toggles apply to it, then switch off.
    if (ui->heldKey)
        releaseKey(ui);
    ui->heldKey = k->hid;
    ui->appliedMods = ui->latchedMods;
    ui->latchedMods = 0;
    ui->heldFrames = 0;
    ui->releasePending = false;
    ui->activeKey = idx;
}

static void beginTouch(TouchUI *ui, int px, int py, u64 now)
{
    ui->startX = ui->lastX = px;
    ui->startY = ui->lastY = py;
    ui->touchStartMs = now;
    ui->padMoved = false;
    ui->target = TARGET_NONE;

    // The three mode buttons along the top work in every mode
    if (py < (int)BAR_H)
    {
        ui->target = TARGET_BAR;
        int i = (px * 3) / (int)BOTTOM_W;
        if (i < 0) i = 0;
        if (i > 2) i = 2;
        setMode(ui, (UiMode)i);
        return;
    }

    if (ui->mode == UI_MODE_MOUSE)
    {
        if (py < (int)PAD_BOTTOM)
        {
            ui->target = TARGET_PAD;
            return;
        }
        const int chip = chipAt(ui, px, py);
        if (chip == 0)      { ui->circleScroll = !ui->circleScroll; ui->target = TARGET_CHIP; }
        else if (chip == 1) { ui->cstickScroll = !ui->cstickScroll; ui->target = TARGET_CHIP; }
        return;
    }

    if (ui->mode == UI_MODE_KEYBOARD)
    {
        const int k = keyAt(ui, px, py);
        if (k >= 0)
        {
            ui->target = TARGET_KEY;
            pressKey(ui, k);
        }
    }
}

static void moveTouch(TouchUI *ui, int px, int py)
{
    int dx, dy;

    if (!ui->padMoved)
    {
        // Ignore small wobbles so a tap doesn't nudge the cursor
        const int ox = px - ui->startX;
        const int oy = py - ui->startY;
        if (ox * ox + oy * oy <= PAD_SLOP_PX * PAD_SLOP_PX)
        {
            ui->lastX = px;
            ui->lastY = py;
            return;
        }
        ui->padMoved = true;
        dx = ox;
        dy = oy;
    }
    else
    {
        dx = px - ui->lastX;
        dy = py - ui->lastY;
    }
    ui->lastX = px;
    ui->lastY = py;

    // Relative movement with a little acceleration for fast swipes
    float mag = sqrtf((float)(dx * dx + dy * dy));
    if (mag > 25.0f) mag = 25.0f;
    const float gain = MOUSE_SENSITIVITY * (1.0f + MOUSE_ACCEL * mag);
    ui->accX += dx * gain;
    ui->accY += dy * gain;
}

static void endTouch(TouchUI *ui, u64 now)
{
    if (ui->target == TARGET_PAD)
    {
        if (!ui->padMoved && (now - ui->touchStartMs) <= TAP_MAX_MS)
            ui->tapFrames = TAP_CLICK_FRAMES;           // a tap is a left click
    }
    else if (ui->target == TARGET_KEY)
    {
        if (ui->heldKey && ui->activeKey >= 0)
        {
            if (ui->heldFrames >= KEY_MIN_FRAMES)
                releaseKey(ui);
            else
                ui->releasePending = true;              // too quick: hold it a few more frames
        }
    }
    ui->target = TARGET_NONE;
}

void touchui_update(TouchUI *ui, u32 down, u32 held, u32 up, touchPosition touch, u64 nowMs)
{
    if (ui->heldKey)
    {
        if (ui->heldFrames < 255)
            ui->heldFrames++;
        if (ui->releasePending && ui->heldFrames >= KEY_MIN_FRAMES)
            releaseKey(ui);
    }

    if (up & KEY_TOUCH)
        endTouch(ui, nowMs);

    if (down & KEY_TOUCH)
        beginTouch(ui, touch.px, touch.py, nowMs);
    else if ((held & KEY_TOUCH) && ui->target == TARGET_PAD)
        moveTouch(ui, touch.px, touch.py);
}

void touchui_fill_packet(TouchUI *ui, ControllerPacket *pkt)
{
    pkt->mode = (u8)ui->mode;

    pkt->flags = 0;
    if (ui->circleScroll) pkt->flags |= PKT_FLAG_CIRCLE_SCROLL;
    if (ui->cstickScroll) pkt->flags |= PKT_FLAG_CSTICK_SCROLL;
    if (ui->isNew3DS)     pkt->flags |= PKT_FLAG_NEW3DS;

    // Send the whole pixels of accumulated movement; keep the fractions for next time
    int dx = (int)ui->accX;
    int dy = (int)ui->accY;
    ui->accX -= dx;
    ui->accY -= dy;
    if (dx > 32767) dx = 32767;
    if (dx < -32768) dx = -32768;
    if (dy > 32767) dy = 32767;
    if (dy < -32768) dy = -32768;
    pkt->mouseDX = (s16)dx;
    pkt->mouseDY = (s16)dy;

    pkt->mouseButtons = (ui->tapFrames > 0) ? PKT_MOUSE_LEFT : 0;
    if (ui->tapFrames > 0)
        ui->tapFrames--;

    // Lit toggles count as held, and so do the ones applied to the key being typed
    u8 mods = ui->latchedMods | ui->appliedMods;
    if (ui->winPulseFrames > 0)
    {
        mods |= MOD_LWIN;
        ui->winPulseFrames--;
    }
    pkt->modifiers = mods;
    pkt->key = ui->heldKey;
    pkt->reserved = 0;
}

u32 touchui_background(const TouchUI *ui)
{
    switch (ui->mode)
    {
        case UI_MODE_MOUSE:    return CLR(24, 24, 24);   // very dark gray
        case UI_MODE_KEYBOARD: return CLR(14, 14, 18);
        default:               return CLR(0, 0, 0);      // "off"
    }
}

void touchui_draw(const TouchUI *ui)
{
    if (ui->mode == UI_MODE_MOUSE)
        drawChips(ui);
    else if (ui->mode == UI_MODE_KEYBOARD)
        drawKeyboard(ui);

    drawBar(ui);
}
