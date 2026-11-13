#include <3ds.h>
#include <citro2d.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
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
#define STATUS_ROW_H   17.0f
#define LINE_H         15.0f
#define VALUE_X        (PANEL_X + 72.0f)      // status values (left column)
#define RIGHT_LABEL_X  (PANEL_X + 205.0f)     // "Display:" label
#define RIGHT_VALUE_X  (PANEL_X + 270.0f)     // "Display:" value
#define COLUMN2_X      (PANEL_X + 195.0f)     // second column of the controls list
#define MAX_LINES      16

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

void getbatterystatus(char *buffer, size_t size) {
    u8 percentage;
    PTMU_GetBatteryLevel(&percentage);

    u8 charging;
    PTMU_GetBatteryChargeState(&charging);

    char icon[6];

    // Convert battery level (0-4) to percentage (20-100)
    int actualpercentage = (percentage + 1) * 20;
    if (actualpercentage > 100) actualpercentage = 100;

    for (int i = 0; i < 5; i++) {
        icon[i] = (i <= percentage) ? '|' : ' ';
    }
    icon[5] = '\0';

    if (charging) {
        snprintf(buffer, size, "[%s] +", icon);
    } else {
        snprintf(buffer, size, "[%s] %d%%", icon, actualpercentage);
    }
}

// Turn the backlight on or off. The LCD service is opened only for the
// instant it is needed and closed again right away, so no session to it is
// held open while the app is suspended (HOME button) or exiting.
static void lcdset(int on) {
    if (R_FAILED(gspLcdInit())) return;
    if (on) GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTH);
    else    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
    gspLcdExit();
}

static void parsetext(C2D_Text *t, C2D_TextBuf buf, const char *s) {
    C2D_TextParse(t, buf, s);
    C2D_TextOptimize(t);
}

static void drawtext(const C2D_Text *t, float x, float y, float scale, u32 clr) {
    C2D_DrawText(t, C2D_WithColor, x, y, 0.5f, scale, scale, clr);
}

// ---------------------------------------------------------------------------
// Text for the top screen (parsed once, at startup)
// ---------------------------------------------------------------------------
typedef struct {
    // title and status rows
    C2D_Text title, lblStatus, lblServer, lblBattery, lblDisplay;
    C2D_Text vConnected, vNoConn, vOn, vOff, vServer;
    C2D_Text battery;                       // refilled once a second
    // headers over the controls list
    C2D_Text hdrController, hdrMouse, hdrKeyboard;
    // lines of the controls list
    C2D_Text ctlCircle, ctlCstick, ctlFace, ctlShoulderNew, ctlShoulderOld, ctlDpad;
    C2D_Text touchMove, touchTap, touchKeys, modKeys;
    C2D_Text btnLeft, btnRight, btnBackNew, btnBackOld, btnFwdNew, btnFwdOld;
    C2D_Text btnEnter, btnDpad;
    C2D_Text cpMove, cpScroll, csMove, csScroll;
    C2D_Text lcdHint, exitHint;
} TopTexts;

static void buildTopTexts(TopTexts *t, C2D_TextBuf buf, const char *serverText) {
    parsetext(&t->title,      buf, "3DS CONTROLLER");
    parsetext(&t->lblStatus,  buf, "Status:");
    parsetext(&t->lblServer,  buf, "Server:");
    parsetext(&t->lblBattery, buf, "Battery:");
    parsetext(&t->lblDisplay, buf, "Display:");
    parsetext(&t->vConnected, buf, "CONNECTED");
    parsetext(&t->vNoConn,    buf, "NO CONNECTION");
    parsetext(&t->vOn,        buf, "ON");
    parsetext(&t->vOff,       buf, "OFF");
    parsetext(&t->vServer,    buf, serverText);

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

    parsetext(&t->cpMove,   buf, "Circle Pad: Move cursor");
    parsetext(&t->cpScroll, buf, "Circle Pad: Scroll");
    parsetext(&t->csMove,   buf, "C-Stick: Move cursor");
    parsetext(&t->csScroll, buf, "C-Stick: Scroll");

    parsetext(&t->lcdHint,  buf, "Hold SELECT: Toggle LCD");
    parsetext(&t->exitHint, buf, "START+SELECT: Exit");
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
    } else {
        lines[n++] = &t->touchKeys;
        lines[n++] = &t->modKeys;
    }
    lines[n++] = &t->btnLeft;
    lines[n++] = &t->btnRight;
    lines[n++] = isNew ? &t->btnBackNew : &t->btnBackOld;
    lines[n++] = isNew ? &t->btnFwdNew : &t->btnFwdOld;
    lines[n++] = &t->btnEnter;
    lines[n++] = &t->btnDpad;
    lines[n++] = ui->circleScroll ? &t->cpScroll : &t->cpMove;
    if (isNew) lines[n++] = ui->cstickScroll ? &t->csScroll : &t->csMove;
    lines[n++] = &t->lcdHint;
    lines[n++] = &t->exitHint;
    return n;
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
    const u32 clrlgreen = C2D_Color32(192, 255, 192, 255);
    const u32 clrlred   = C2D_Color32(255, 192, 192, 255);

    // ---- Bottom screen: buttons, trackpad, keyboard ----
    TouchUI ui;
    touchui_init(&ui, isNew3DS);

    // ---- Top screen text ----
    C2D_TextBuf staticbuf = C2D_TextBufNew(2048);
    C2D_TextBuf dynbuf    = C2D_TextBufNew(64);   // battery text, refilled once a second
    TopTexts tx;
    char servertext[96];
    snprintf(servertext, sizeof(servertext), "%s:%d", cfg.serverip, cfg.port);
    buildTopTexts(&tx, staticbuf, servertext);

    float titlew, titleh;
    C2D_TextGetDimensions(&tx.title, TITLE_SCALE, TITLE_SCALE, &titlew, &titleh);
    const float titlex = PANEL_X + (PANEL_W - titlew) / 2.0f;

    char batterystr[32] = "";
    u64 laststatusupdate = 0;

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

        if (isconnected && sockfd >= 0)
            send(sockfd, &pkt, sizeof(pkt), 0);

        // ---- Refresh the battery text once a second ----
        if (now - laststatusupdate > 1000) {
            getbatterystatus(batterystr, sizeof(batterystr));
            C2D_TextBufClear(dynbuf);
            parsetext(&tx.battery, dynbuf, batterystr);
            laststatusupdate = now;
        }

        // ---- Draw ----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);

        // Top screen: status + the controls for the current mode
        C2D_TargetClear(top, clrblack);
        C2D_SceneBegin(top);
        ui_draw_rounded_rect(PANEL_X, PANEL_Y, PANEL_W, PANEL_H, PANEL_RADIUS, clrpanel);

        drawtext(&tx.title, titlex, PANEL_Y + 6.0f, TITLE_SCALE, clrwhite);

        const float labelx = PANEL_X + PAD;
        float y = PANEL_Y + 34.0f;

        drawtext(&tx.lblStatus, labelx, y, STATUS_SCALE, clrgray);
        if (isconnected) drawtext(&tx.vConnected, VALUE_X, y, STATUS_SCALE, clrlgreen);
        else             drawtext(&tx.vNoConn,    VALUE_X, y, STATUS_SCALE, clrlred);
        y += STATUS_ROW_H;

        drawtext(&tx.lblServer, labelx, y, STATUS_SCALE, clrgray);
        drawtext(&tx.vServer,   VALUE_X, y, STATUS_SCALE, clrwhite);
        y += STATUS_ROW_H;

        drawtext(&tx.lblBattery, labelx, y, STATUS_SCALE, clrgray);
        drawtext(&tx.battery,    VALUE_X, y, STATUS_SCALE, clrwhite);
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
    C2D_TextBufDelete(dynbuf);
    C2D_TextBufDelete(staticbuf);
    C2D_Fini();
    C3D_Fini();

    // Never leave the screens dark on exit
    if (!lcdstate) {
        lcdset(1);
    }

    ptmuExit();
    socExit();
    gfxExit();
    return 0;
}
