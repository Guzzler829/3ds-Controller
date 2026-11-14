#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>

#include "protocol.h"
#include "touchui.h"

#define DEFAULT_PORT 8888
#define DEFAULT_SERVER_IP "192.168.1.1"
#define CONFIG_FILE "config.ini"   // keep relative: "sdmc:/config.ini" hung at startup in testing
#define MAX_LINE 256
#define LCD_TOGGLE_KEY KEY_SELECT
#define LCD_TOGGLE_HOLD_TIME 5000  // hold SELECT this long (ms) to toggle the screens
#define CLOCK_24H 0                // 1 = 13:45, 0 = 1:45 PM

// Connection timing (ms)
#define PING_INTERVAL_MS     1000  // send a ping this often
#define CONNECTED_WINDOW_MS  3500  // "connected" = a pong arrived within this long ago
#define RECONNECT_AFTER_MS   6000  // rebuild the socket after this long without a pong

// ---- Top screen layout (400 x 240) ----
#define TOP_W          400.0f
#define TOP_H          240.0f
#define PANEL_W        380.0f
#define PANEL_H        220.0f
#define PANEL_X        ((TOP_W - PANEL_W) / 2.0f)
#define PANEL_Y        ((TOP_H - PANEL_H) / 2.0f)
#define PANEL_RADIUS   10.0f
#define PAD            14.0f
#define TITLE_SCALE    0.7f
#define STATUS_SCALE   0.5f
#define LINE_SCALE     0.44f
#define CLOCK_SCALE    0.45f
#define STATUS_ROW_H   17.0f
#define LINE_H         15.0f
#define VALUE_X        (PANEL_X + 72.0f)      // status values (left column)
#define RIGHT_LABEL_X  (PANEL_X + 225.0f)     // "Display:" label
#define RIGHT_VALUE_X  (PANEL_X + 292.0f)     // "Display:" value
#define COLUMN2_X      (PANEL_X + 195.0f)     // second column of the controls list
#define BAR_GAP        8.0f                   // space between the battery text and its bar
#define BAR_END_GAP    10.0f                  // space between the end of the bar and "Display:"
#define BAR_HEIGHT_FACTOR 0.8f                // battery bar height, as a fraction of the text's line height
#define MAX_LINES      16

// ---- "New host IP:" overlay ----
#define OV_W           300.0f
#define OV_H           120.0f
#define OV_X           ((TOP_W - OV_W) / 2.0f)
#define OV_Y           ((TOP_H - OV_H) / 2.0f)

typedef struct {
    char serverip[64];
    int port;
} config;

void readconfigfile(config *cfg) {
    strcpy(cfg->serverip, DEFAULT_SERVER_IP);
    cfg->port = DEFAULT_PORT;

    FILE *file = fopen(CONFIG_FILE, "r");
    if (!file) {
        // No config yet: create one with the defaults
        file = fopen(CONFIG_FILE, "w");
        if (file) {
            fprintf(file, "serverip=%s\n", DEFAULT_SERVER_IP);
            fprintf(file, "port=%d\n", DEFAULT_PORT);
            fclose(file);
        }
        return;
    }

    char line[MAX_LINE];
    while (fgets(line, MAX_LINE, file)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");

        if (key && value) {
            while (*key && (*key == ' ' || *key == '\t')) key++;
            while (*value && (*value == ' ' || *value == '\t')) value++;

            if (strcmp(key, "serverip") == 0) {
                strncpy(cfg->serverip, value, sizeof(cfg->serverip) - 1);
                cfg->serverip[sizeof(cfg->serverip) - 1] = '\0';
            } else if (strcmp(key, "port") == 0) {
                cfg->port = atoi(value);
            }
        }
    }

    fclose(file);
}

// Saves the settings back to config.ini. Returns false if the file couldn't be written.
static bool writeconfigfile(const config *cfg) {
    FILE *file = fopen(CONFIG_FILE, "w");
    if (!file) return false;
    fprintf(file, "serverip=%s\n", cfg->serverip);
    fprintf(file, "port=%d\n", cfg->port);
    fclose(file);
    return true;
}

int initsocket(const config *cfg) {
    int sockfd;
    struct sockaddr_in serveraddr;

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        return -1;
    }

    memset(&serveraddr, 0, sizeof(serveraddr));

    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(cfg->port);

    if (inet_pton(AF_INET, cfg->serverip, &serveraddr.sin_addr) <= 0) {
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&serveraddr, sizeof(serveraddr)) < 0) {
        close(sockfd);
        return -1;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

    return sockfd;
}

// ---------------------------------------------------------------------------
// Battery, wifi and clock
// ---------------------------------------------------------------------------
static bool g_mcuReady = false;   // true if the MCU service (exact percentage) could be opened

// Battery charge as a whole number from 0 to 100, or -1 if it can't be read.
static int readBatteryPercent(void) {
    if (g_mcuReady) {
        u8 level = 0;
        if (R_SUCCEEDED(MCUHWC_GetBatteryLevel(&level)))
            return level > 100 ? 100 : level;
    }
    // Fallback when the MCU service isn't available: the PTM service only reports 6 coarse steps
    u8 step = 0;
    if (R_FAILED(PTMU_GetBatteryLevel(&step)))
        return -1;
    if (step > 5) step = 5;
    return step == 0 ? 5 : step * 20;
}

static bool readCharging(void) {
    u8 charging = 0;
    PTMU_GetBatteryChargeState(&charging);
    return charging != 0;
}

// Color for the battery bar: 100% = pure green, 50% = pure yellow, 0% = pure red.
static u32 batteryColor(float p) {
    if (p < 0.0f) p = 0.0f;
    if (p > 1.0f) p = 1.0f;
    float r, g;
    if (p >= 0.5f) { r = (1.0f - p) * 2.0f; g = 1.0f; }   // above 50%: red fades out
    else           { r = 1.0f;               g = p * 2.0f; } // below 50%: green fades in
    return C2D_Color32((u8)(r * 255.0f + 0.5f), (u8)(g * 255.0f + 0.5f), 0, 255);
}

// Is the 3DS connected to a wifi network at all? (This is about the network, not the PC.)
static bool readWifiConnected(void) {
    u32 status = 0;
    return R_SUCCEEDED(ACU_GetWifiStatus(&status)) && status != 0;
}

// The current time as shown on the 3DS's own clock
static void formatClock(char *out, size_t size) {
    time_t now = time(NULL);
    struct tm *t = gmtime(&now);   // the console's clock already holds local time
    if (!t) { snprintf(out, size, "--:--"); return; }
#if CLOCK_24H
    snprintf(out, size, "%02d:%02d", t->tm_hour, t->tm_min);
#else
    int hour = t->tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(out, size, "%d:%02d %s", hour, t->tm_min, t->tm_hour >= 12 ? "PM" : "AM");
#endif
}

// ---------------------------------------------------------------------------
// LCD
// ---------------------------------------------------------------------------
// Turn the backlight on or off. The LCD service is opened only for the
// instant it is needed and closed again right away, so no session to it is
// held open while the app is suspended (HOME button) or exiting.
static void lcdset(int on) {
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    else    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
}

// ---------------------------------------------------------------------------
// Drawing helpers
// ---------------------------------------------------------------------------
static void parsetext(C2D_Text *t, C2D_TextBuf buf, const char *s) {
    C2D_TextParse(t, buf, s);
    C2D_TextOptimize(t);
}

static void drawtextz(const C2D_Text *t, float x, float y, float scale, u32 clr, float z) {
    C2D_DrawText(t, C2D_WithColor, x, y, z, scale, scale, clr);
}

static void drawtext(const C2D_Text *t, float x, float y, float scale, u32 clr) {
    drawtextz(t, x, y, scale, clr, 0.5f);
}

// A small three-bar signal icon (about 14 x 12 px). Lit bars show the signal strength (1-3);
// with no wifi connection the bars are dark and a red X is drawn over them.
static void drawWifiIcon(float x, float y, bool connected, int strength) {
    const u32 lit  = C2D_Color32(160, 255, 160, 255);
    const u32 dim  = C2D_Color32(70, 70, 76, 255);
    const u32 red  = C2D_Color32(255, 160, 160, 255);
    int bars = connected ? (strength < 1 ? 1 : (strength > 3 ? 3 : strength)) : 0;

    for (int i = 0; i < 3; i++) {
        const float h = 4.0f * (i + 1);                  // 4, 8, 12 px tall
        C2D_DrawRectSolid(x + i * 5.0f, y + 12.0f - h, 0.0f, 3.5f, h, (i < bars) ? lit : dim);
    }
    if (!connected) {
        for (int d = -4; d <= 4; d += 2) {               // a stepped X, 2 px squares
            C2D_DrawRectSolid(x + 7.0f + d - 1.0f, y + 6.0f + d - 1.0f, 0.0f, 2.0f, 2.0f, red);
            C2D_DrawRectSolid(x + 7.0f + d - 1.0f, y + 6.0f - d - 1.0f, 0.0f, 2.0f, 2.0f, red);
        }
    }
}

// ---------------------------------------------------------------------------
// Text for the top screen (parsed once, at startup)
// ---------------------------------------------------------------------------
typedef struct {
    // title and status rows
    C2D_Text title, lblStatus, lblServer, lblBattery, lblDisplay;
    C2D_Text vConnected, vNoConn, vOn, vOff;
    C2D_Text vServer, battery, clock;       // these three are refilled when they change
    // headers over the controls list
    C2D_Text hdrController, hdrMouse, hdrKeyboard;
    // lines of the controls list
    C2D_Text ctlCircle, ctlCstick, ctlFace, ctlShoulderNew, ctlShoulderOld, ctlDpad;
    C2D_Text touchMove, touchTap, touchKeys, modKeys;
    C2D_Text btnLeft, btnRight, btnBackNew, btnBackOld, btnFwdNew, btnFwdOld;
    C2D_Text btnEnter, btnDpad;
    C2D_Text kbEnter, kbBackspace, kbShift, kbCopyPaste, kbBackFwd;
    C2D_Text cpMove, cpScroll, csMove, csScroll;
    C2D_Text lcdHint, exitHint;
    // the "New host IP:" overlay
    C2D_Text ipLabel, ipHint, ipError;
} TopTexts;

static void buildTopTexts(TopTexts *t, C2D_TextBuf buf) {
    parsetext(&t->title,      buf, "3DS CONTROLLER");
    parsetext(&t->lblStatus,  buf, "Status:");
    parsetext(&t->lblServer,  buf, "Server:");
    parsetext(&t->lblBattery, buf, "Battery:");
    parsetext(&t->lblDisplay, buf, "Display:");
    parsetext(&t->vConnected, buf, "CONNECTED");
    parsetext(&t->vNoConn,    buf, "NO CONNECTION");
    parsetext(&t->vOn,        buf, "ON");
    parsetext(&t->vOff,       buf, "OFF");

    parsetext(&t->hdrController, buf, "CONTROLS - CONTROLLER MODE");
    parsetext(&t->hdrMouse,      buf, "CONTROLS - MOUSE MODE");
    parsetext(&t->hdrKeyboard,   buf, "CONTROLS - KEYBOARD MODE");

    parsetext(&t->ctlCircle,      buf, "Circle Pad: Left Stick");
    parsetext(&t->ctlCstick,      buf, "C-Stick: Right Stick");
    parsetext(&t->ctlFace,        buf, "A/B/X/Y: Face Buttons");
    parsetext(&t->ctlShoulderNew, buf, "L/R/ZL/ZR: Shoulders");
    parsetext(&t->ctlShoulderOld, buf, "L/R: Shoulders");
    parsetext(&t->ctlDpad,        buf, "D-Pad: Directional");

    parsetext(&t->touchMove, buf, "Touch: Move cursor");
    parsetext(&t->touchTap,  buf, "Tap: Left click");
    parsetext(&t->touchKeys, buf, "Touch: Press keys");
    parsetext(&t->modKeys,   buf, "Ctrl/Alt/Shift/Win: Latch");

    parsetext(&t->btnLeft,    buf, "A / L: Left click");
    parsetext(&t->btnRight,   buf, "B / R: Right click");
    parsetext(&t->btnBackNew, buf, "Y / ZL: Back");
    parsetext(&t->btnBackOld, buf, "Y: Back");
    parsetext(&t->btnFwdNew,  buf, "X / ZR: Forward");
    parsetext(&t->btnFwdOld,  buf, "X: Forward");
    parsetext(&t->btnEnter,   buf, "START: Enter");
    parsetext(&t->btnDpad,    buf, "D-Pad: Arrow keys");

    parsetext(&t->kbEnter,     buf, "A / START: Enter");
    parsetext(&t->kbBackspace, buf, "B: Backspace");
    parsetext(&t->kbShift,     buf, "L / R: Shift");
    parsetext(&t->kbCopyPaste, buf, "Y / X: Copy / Paste");
    parsetext(&t->kbBackFwd,   buf, "ZL / ZR: Back / Forward");

    parsetext(&t->cpMove,   buf, "Circle Pad: Move cursor");
    parsetext(&t->cpScroll, buf, "Circle Pad: Scroll");
    parsetext(&t->csMove,   buf, "C-Stick: Move cursor");
    parsetext(&t->csScroll, buf, "C-Stick: Scroll");

    parsetext(&t->lcdHint,  buf, "Hold SELECT: Toggle LCD");
    parsetext(&t->exitHint, buf, "START+SELECT: Exit");

    parsetext(&t->ipLabel, buf, "New host IP:");
    parsetext(&t->ipHint,  buf, "Press Enter to set IP");
    parsetext(&t->ipError, buf, "Not a valid IPv4 address");
}

// Picks which lines to show for the current mode. ZL, ZR and the C-Stick are
// only listed on a New 3DS; an old 3DS never sees them.
static int buildControlLines(const TopTexts *t, const TouchUI *ui, const C2D_Text **lines) {
    int n = 0;
    const bool isNew = ui->isNew3DS;

    if (ui->mode == UI_MODE_CONTROLLER) {
        lines[n++] = &t->ctlCircle;
        if (isNew) lines[n++] = &t->ctlCstick;
        lines[n++] = &t->ctlFace;
        lines[n++] = isNew ? &t->ctlShoulderNew : &t->ctlShoulderOld;
        lines[n++] = &t->ctlDpad;
        lines[n++] = &t->lcdHint;
        lines[n++] = &t->exitHint;
        return n;
    }

    if (ui->mode == UI_MODE_MOUSE) {
        lines[n++] = &t->touchMove;
        lines[n++] = &t->touchTap;
        lines[n++] = &t->btnLeft;
        lines[n++] = &t->btnRight;
        lines[n++] = isNew ? &t->btnBackNew : &t->btnBackOld;
        lines[n++] = isNew ? &t->btnFwdNew : &t->btnFwdOld;
        lines[n++] = &t->btnEnter;
        lines[n++] = &t->btnDpad;
    } else {
        lines[n++] = &t->touchKeys;
        lines[n++] = &t->modKeys;
        lines[n++] = &t->kbEnter;
        lines[n++] = &t->kbBackspace;
        lines[n++] = &t->kbShift;
        lines[n++] = &t->kbCopyPaste;
        lines[n++] = &t->btnDpad;
        if (isNew) lines[n++] = &t->kbBackFwd;
    }
    lines[n++] = ui->circleScroll ? &t->cpScroll : &t->cpMove;
    if (isNew) lines[n++] = ui->cstickScroll ? &t->csScroll : &t->csMove;
    lines[n++] = &t->lcdHint;
    lines[n++] = &t->exitHint;
    return n;
}

// The "New host IP:" overlay: a dimmed screen, a rounded box with a label, the text box
// (with a blinking cursor) and a hint underneath. Everything is on higher layers than
// the panel, so nothing underneath shows through.
static void drawIpOverlay(const TouchUI *ui, const TopTexts *tx, C2D_TextBuf buf, u64 now) {
    const u32 clrwhite = C2D_Color32(255, 255, 255, 255);
    const u32 clrgray  = C2D_Color32(210, 210, 210, 255);
    const u32 clrlred  = C2D_Color32(255, 160, 160, 255);

    C2D_DrawRectSolid(0.0f, 0.0f, 0.55f, TOP_W, TOP_H, C2D_Color32(0, 0, 0, 170));          // dim what is behind
    ui_draw_rounded_rect_z(OV_X - 2.0f, OV_Y - 2.0f, OV_W + 4.0f, OV_H + 4.0f, 12.0f,
                           C2D_Color32(110, 110, 125, 255), 0.58f);                          // border
    ui_draw_rounded_rect_z(OV_X, OV_Y, OV_W, OV_H, 10.0f, C2D_Color32(40, 40, 46, 255), 0.6f);

    drawtextz(&tx->ipLabel, OV_X + 20.0f, OV_Y + 12.0f, 0.6f, clrwhite, 0.9f);

    // the text box
    const float bx = OV_X + 20.0f, by = OV_Y + 40.0f, bw = OV_W - 40.0f, bh = 32.0f;
    ui_draw_rounded_rect_z(bx - 1.5f, by - 1.5f, bw + 3.0f, bh + 3.0f, 6.0f, C2D_Color32(120, 120, 135, 255), 0.62f);
    ui_draw_rounded_rect_z(bx, by, bw, bh, 5.0f, C2D_Color32(16, 16, 20, 255), 0.65f);

    // text plus the width of everything before the cursor (to place the cursor)
    C2D_TextBufClear(buf);
    float prefixW = 0.0f, prefixH = 0.0f;
    if (ui->ipLen > 0) {
        C2D_Text full;
        parsetext(&full, buf, ui->ipText);
        drawtextz(&full, bx + 8.0f, by + 6.0f, 0.6f, clrwhite, 0.9f);
    }
    if (ui->ipCursor > 0) {
        char prefix[IP_MAX_LEN + 1];
        memcpy(prefix, ui->ipText, ui->ipCursor);
        prefix[ui->ipCursor] = '\0';
        C2D_Text pre;
        parsetext(&pre, buf, prefix);
        C2D_TextGetDimensions(&pre, 0.6f, 0.6f, &prefixW, &prefixH);
    }
    if ((now / 500) % 2 == 0)                                   // blinking cursor
        C2D_DrawRectSolid(bx + 8.0f + prefixW, by + 6.0f, 0.8f, 1.5f, 20.0f, clrwhite);

    // smaller hint underneath (or the error, if Enter was pressed on something invalid)
    if (ui->ipError)
        drawtextz(&tx->ipError, bx, by + bh + 10.0f, 0.45f, clrlred, 0.9f);
    else
        drawtextz(&tx->ipHint, bx, by + bh + 10.0f, 0.42f, clrgray, 0.9f);
}

int main(int argc, char **argv) {
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget *top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget *bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    socInit((u32*)memalign(0x1000, 0x100000), 0x100000);
    ptmuInit();
    acInit();                                  // wifi status
    g_mcuReady = R_SUCCEEDED(mcuHwcInit());    // exact battery percentage (optional)

    // Is this a "New" 3DS? (They have ZL, ZR and a C-Stick.)
    bool isNew3DS = false;
    APT_CheckNew3DS(&isNew3DS);

    config cfg;
    readconfigfile(&cfg);

    int sockfd = initsocket(&cfg);
    int isconnected = 0;
    int lcdstate = 1;
    u32 lcdtoggleheldtime = 0;
    u64 lastPingMs = 0;
    u64 lastPongMs = 0;                 // 0 = no pong yet
    u64 lastReconnectMs = osGetTime();

    // ---- Colors ----
    const u32 clrblack  = C2D_Color32(0, 0, 0, 255);
    const u32 clrpanel  = C2D_Color32(32, 32, 32, 255);
    const u32 clrwhite  = C2D_Color32(255, 255, 255, 255);
    const u32 clrgray   = C2D_Color32(210, 210, 210, 255);
    const u32 clrclock  = C2D_Color32(140, 140, 140, 255);   // dim: the clock is a background detail
    const u32 clrlgreen = C2D_Color32(160, 255, 160, 255);
    const u32 clrlred   = C2D_Color32(255, 160, 160, 255);
    const u32 clrtrack  = C2D_Color32(62, 62, 68, 255);      // empty part of the battery bar

    // ---- Bottom screen: buttons, trackpad, keyboard ----
    TouchUI ui;
    touchui_init(&ui, isNew3DS);
    touchui_set_current_ip(&ui, cfg.serverip);

    // ---- Top screen text ----
    C2D_TextBuf staticbuf = C2D_TextBufNew(2048);
    C2D_TextBuf dynbuf    = C2D_TextBufNew(128);   // battery text, server text and clock (refilled when they change)
    C2D_TextBuf ipbuf     = C2D_TextBufNew(128);   // the text typed into the IP box (refilled every frame while open)
    TopTexts tx;
    buildTopTexts(&tx, staticbuf);

    float titlew, titleh;
    C2D_TextGetDimensions(&tx.title, TITLE_SCALE, TITLE_SCALE, &titlew, &titleh);
    const float titlex = PANEL_X + (PANEL_W - titlew) / 2.0f;

    // Battery bar geometry: it starts after the widest possible percentage text ("100%+")
    // and its full length ends just before "Display:".
    C2D_Text widest;
    C2D_TextBuf measurebuf = C2D_TextBufNew(16);
    parsetext(&widest, measurebuf, "100%+");
    float widestW, lineH;
    C2D_TextGetDimensions(&widest, STATUS_SCALE, STATUS_SCALE, &widestW, &lineH);
    C2D_TextBufDelete(measurebuf);
    const float barX = VALUE_X + widestW + BAR_GAP;
    const float barMaxW = RIGHT_LABEL_X - BAR_END_GAP - barX;
    const float barH = lineH * BAR_HEIGHT_FACTOR;

    char servertext[96], batterystr[16] = "", clockstr[16] = "", lastclock[16] = "";
    float clockW = 0.0f, clockH = 0.0f;
    int batteryPercent = -1;
    bool wifiConnected = false;
    int wifiStrength = 0;
    u64 laststatusupdate = 0;
    bool serverDirty = true;
    bool firstClock = true;

    while (aptMainLoop()) {
        hidScanInput();
        const u32 down = hidKeysDown();
        const u32 held = hidKeysHeld();
        const u32 up   = hidKeysUp();

        if ((held & KEY_START) && (held & KEY_SELECT))
            break;

        // HOME is blocked while the display is off. If it is pressed anyway,
        // turn the display back on and allow HOME again; the next press opens
        // the Home Menu normally. This avoids suspending with the backlight off.
        if (aptCheckHomePressRejected()) {
            lcdstate = 1;
            lcdset(1);
            aptSetHomeAllowed(true);
        }

        const u64 now = osGetTime();

        // ---- Connection: never blocks, so the touch screen stays responsive ----
        if (sockfd >= 0) {
            char reply[16];
            ssize_t got;
            while ((got = recv(sockfd, reply, sizeof(reply) - 1, 0)) > 0) {
                reply[got] = '\0';
                if (strcmp(reply, "pong") == 0)
                    lastPongMs = now;
            }
            if (now - lastPingMs >= PING_INTERVAL_MS) {
                send(sockfd, "ping", 5, 0);
                lastPingMs = now;
            }
        }
        isconnected = (sockfd >= 0 && lastPongMs != 0 && now - lastPongMs < CONNECTED_WINDOW_MS);

        // No pong for a long time: rebuild the socket (helps after Wi-Fi drops)
        {
            u64 lastGood = (lastPongMs > lastReconnectMs) ? lastPongMs : lastReconnectMs;
            if (sockfd < 0 || now - lastGood > RECONNECT_AFTER_MS) {
                if (sockfd >= 0) close(sockfd);
                sockfd = initsocket(&cfg);
                lastReconnectMs = now;
            }
        }

        // ---- Hold SELECT to toggle the screens ----
        if (held & LCD_TOGGLE_KEY) {
            if (lcdtoggleheldtime == 0) {
                lcdtoggleheldtime = (u32)now;
            } else if ((u32)now - lcdtoggleheldtime > LCD_TOGGLE_HOLD_TIME) {
                lcdstate = !lcdstate;
                lcdset(lcdstate);
                aptSetHomeAllowed(lcdstate ? true : false);
                lcdtoggleheldtime = (u32)now;
            }
        } else {
            lcdtoggleheldtime = 0;
        }

        // ---- Touch screen ----
        touchPosition touch;
        memset(&touch, 0, sizeof(touch));
        if (held & KEY_TOUCH)
            hidTouchRead(&touch);
        touchui_update(&ui, down, held, up, touch, now);

        // ---- A new host IP was entered: save it, use it, reconnect ----
        char newip[IP_MAX_LEN + 1];
        if (touchui_take_submitted_ip(&ui, newip, sizeof(newip))) {
            snprintf(cfg.serverip, sizeof(cfg.serverip), "%s", newip);
            writeconfigfile(&cfg);
            touchui_set_current_ip(&ui, cfg.serverip);
            if (sockfd >= 0) close(sockfd);
            sockfd = initsocket(&cfg);
            lastPongMs = 0;                      // "NO CONNECTION" until the new PC answers
            lastPingMs = 0;
            lastReconnectMs = now;
            serverDirty = true;
        }

        // ---- Build and send this frame's packet ----
        ControllerPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.buttons = held & ~KEY_TOUCH;

        circlePosition cp, cs;
        hidCircleRead(&cp);
        hidCstickRead(&cs);
        pkt.circleX = cp.dx;
        pkt.circleY = cp.dy;
        pkt.cstickX = cs.dx;
        pkt.cstickY = cs.dy;

        touchui_fill_packet(&ui, &pkt);   // must run every frame (it counts down click timers)

        // While the IP box is open the buttons edit the box: send the PC nothing
        if (ui.ipEntry) {
            pkt.buttons = 0;
            pkt.circleX = pkt.circleY = pkt.cstickX = pkt.cstickY = 0;
        }

        if (isconnected && sockfd >= 0)
            send(sockfd, &pkt, sizeof(pkt), 0);

        // ---- Refresh battery, wifi and the server text (battery and wifi once a second) ----
        if (now - laststatusupdate > 1000) {
            batteryPercent = readBatteryPercent();
            if (batteryPercent < 0)
                snprintf(batterystr, sizeof(batterystr), "N/A");
            else
                snprintf(batterystr, sizeof(batterystr), "%d%%%s", batteryPercent, readCharging() ? "+" : "");
            wifiConnected = readWifiConnected();
            wifiStrength = osGetWifiStrength();
            laststatusupdate = now;
            serverDirty = true;   // the dynamic buffer is rebuilt below, so everything in it gets re-parsed
        }

        // The clock text changes once a minute; rebuild everything in the dynamic buffer when anything changed
        formatClock(clockstr, sizeof(clockstr));
        if (serverDirty || firstClock || strcmp(clockstr, lastclock) != 0) {
            C2D_TextBufClear(dynbuf);
            snprintf(servertext, sizeof(servertext), "%s:%d", cfg.serverip, cfg.port);
            parsetext(&tx.vServer, dynbuf, servertext);
            parsetext(&tx.battery, dynbuf, batterystr);
            parsetext(&tx.clock, dynbuf, clockstr);
            C2D_TextGetDimensions(&tx.clock, CLOCK_SCALE, CLOCK_SCALE, &clockW, &clockH);
            snprintf(lastclock, sizeof(lastclock), "%s", clockstr);
            serverDirty = false;
            firstClock = false;
        }

        // ---- Draw ----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        // Top screen: status + the controls for the current mode
        C2D_TargetClear(top, clrblack);
        C2D_SceneBegin(top);
        ui_draw_rounded_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_RADIUS, clrpanel);

        drawtext(&tx.title, titlex, PANEL_Y + 6.0f, TITLE_SCALE, clrwhite);

        // Wifi icon and clock, top right
        const float clockX = PANEL_X + PANEL_W - PAD - clockW;
        drawtext(&tx.clock, clockX, PANEL_Y + 8.0f, CLOCK_SCALE, clrclock);
        drawWifiIcon(clockX - 10.0f - 14.0f, PANEL_Y + 9.0f, wifiConnected, wifiStrength);

        const float labelx = PANEL_X + PAD;
        float y = PANEL_Y + 34.0f;

        drawtext(&tx.lblStatus, labelx, y, STATUS_SCALE, clrgray);
        if (isconnected) drawtext(&tx.vConnected, VALUE_X, y, STATUS_SCALE, clrlgreen);
        else             drawtext(&tx.vNoConn,    VALUE_X, y, STATUS_SCALE, clrlred);
        y += STATUS_ROW_H;

        drawtext(&tx.lblServer, labelx, y, STATUS_SCALE, clrgray);
        drawtext(&tx.vServer,   VALUE_X, y, STATUS_SCALE, clrwhite);
        y += STATUS_ROW_H;

        // Battery: the percentage (with a + while charging), then a bar that grows to the right
        drawtext(&tx.lblBattery, labelx, y, STATUS_SCALE, clrgray);
        drawtext(&tx.battery,    VALUE_X, y, STATUS_SCALE, clrwhite);
        {
            const float barY = y + (lineH - barH) / 2.0f;
            const float pct = (batteryPercent < 0) ? 0.0f : batteryPercent / 100.0f;
            C2D_DrawRectSolid(barX, barY, 0.0f, barMaxW, barH, clrtrack);
            if (pct > 0.0f)
                C2D_DrawRectSolid(barX, barY, 0.0f, barMaxW * pct, barH, batteryColor(pct));
        }
        drawtext(&tx.lblDisplay, RIGHT_LABEL_X, y, STATUS_SCALE, clrgray);
        if (lcdstate) drawtext(&tx.vOn,  RIGHT_VALUE_X, y, STATUS_SCALE, clrlgreen);
        else          drawtext(&tx.vOff, RIGHT_VALUE_X, y, STATUS_SCALE, clrlred);
        y += STATUS_ROW_H + 8.0f;

        const C2D_Text *header = (ui.mode == UI_MODE_MOUSE) ? &tx.hdrMouse
                               : (ui.mode == UI_MODE_KEYBOARD) ? &tx.hdrKeyboard
                               : &tx.hdrController;
        drawtext(header, labelx, y, STATUS_SCALE, clrgray);
        y += STATUS_ROW_H + 3.0f;

        // The controls list, in two columns (first half down the left, the rest on the right)
        const C2D_Text *lines[MAX_LINES];
        const int n = buildControlLines(&tx, &ui, lines);
        const int perColumn = (n + 1) / 2;
        for (int i = 0; i < n; i++) {
            const float cx = (i < perColumn) ? labelx : COLUMN2_X;
            const float cy = y + (i % perColumn) * LINE_H;
            drawtext(lines[i], cx, cy, LINE_SCALE, clrwhite);
        }

        if (ui.ipEntry)
            drawIpOverlay(&ui, &tx, ipbuf, now);

        // Bottom screen: mode buttons, then the trackpad or keyboard
        C2D_TargetClear(bottom, touchui_background(&ui));
        C2D_SceneBegin(bottom);
        touchui_draw(&ui);

        C3D_FrameEnd(0);
    }

    if (sockfd >= 0) {
        close(sockfd);
    }

    touchui_exit(&ui);
    C2D_TextBufDelete(ipbuf);
    C2D_TextBufDelete(dynbuf);
    C2D_TextBufDelete(staticbuf);
    C2D_Fini();
    C3D_Fini();

    // Never leave the screens dark on exit
    if (!lcdstate) {
        lcdset(1);
    }

    if (g_mcuReady)
        mcuHwcExit();
    acExit();
    ptmuExit();
    socExit();
    gfxExit();
    return 0;
}
