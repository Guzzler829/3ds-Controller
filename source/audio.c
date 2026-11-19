#include "audio.h"
#include "audio_core.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define RING_BUFS     8      // blocks handed to the DSP at most (8 x 20 ms)
#define QUEUE_TARGET  5      // keep about this many queued in the DSP; the rest wait in the jitter buffer
#define PREBUFFER     4      // blocks collected before playing starts (and again after an underrun)
#define JB_MAX_DEPTH  7      // if more than this has piled up, drop the oldest so the delay can't grow
#define NDSP_CHANNEL  0
#define RX_PER_FRAME  48     // packets read per call at most

static struct {
    bool enabled, configured, dspReady, dspFailed, playing;
    AudioState state;

    char host[96], password[AU_KEY_MAX + 1];
    int port, rate, channels;

    int sock;
    struct sockaddr_in addr;
    bool addrValid;

    AudioClient client;
    ndspWaveBuf wave[RING_BUFS];
    int16_t *pcm;            // RING_BUFS blocks, in linear memory (the DSP reads it)
    size_t blockSamples;     // int16 samples per block (block * channels)
    int next;                // ring slot to fill next
} g;

static bool resolve_host(void)
{
    memset(&g.addr, 0, sizeof g.addr);
    g.addr.sin_family = AF_INET;
    g.addr.sin_port = htons((uint16_t)g.port);
    if (inet_pton(AF_INET, g.host, &g.addr.sin_addr) == 1) return true;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(g.host, NULL, &hints, &res) != 0 || !res) return false;
    g.addr.sin_addr = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return true;
}

static void close_socket(void)
{
    if (g.sock >= 0) { close(g.sock); g.sock = -1; }
}

static void stop_playback(void)
{
    if (g.dspReady) {
        ndspChnWaveBufClear(NDSP_CHANNEL);
        ndspChnReset(NDSP_CHANNEL);
    }
    memset(g.wave, 0, sizeof g.wave);
    g.next = 0;
    g.playing = false;
}

static void send_pending(void)
{
    uint8_t out[AU_MAX_PACKET];
    int len, guard = 8;
    while (g.sock >= 0 && guard-- > 0 && (len = ac_poll(&g.client, osGetTime(), out, sizeof out)) > 0)
        send(g.sock, out, (size_t)len, 0);
}

static bool start_stream(u64 now)
{
    if (!g.dspReady && !g.dspFailed) {
        if (R_SUCCEEDED(ndspInit())) g.dspReady = true;
        else g.dspFailed = true;
    }
    if (!g.dspReady) { g.state = AUDIO_NO_DSP; return false; }

    if (!g.addrValid) g.addrValid = resolve_host();
    if (!g.addrValid) { g.state = AUDIO_NO_HOST; return false; }

    g.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g.sock < 0) { g.state = AUDIO_NO_HOST; return false; }
    fcntl(g.sock, F_SETFL, fcntl(g.sock, F_GETFL, 0) | O_NONBLOCK);
    if (connect(g.sock, (struct sockaddr *)&g.addr, sizeof g.addr) < 0) { close_socket(); g.state = AUDIO_NO_HOST; return false; }

    if (!g.pcm) {
        g.pcm = (int16_t *)linearAlloc(sizeof(int16_t) * AU_MAX_BLOCK * AU_MAX_CH * RING_BUFS);
        if (!g.pcm) { close_socket(); g.state = AUDIO_NO_DSP; return false; }
    }

    ndspSetOutputMode(g.channels == 2 ? NDSP_OUTPUT_STEREO : NDSP_OUTPUT_MONO);
    ndspChnReset(NDSP_CHANNEL);
    ndspChnSetInterp(NDSP_CHANNEL, NDSP_INTERP_LINEAR);
    ndspChnSetRate(NDSP_CHANNEL, (float)g.rate);
    ndspChnSetFormat(NDSP_CHANNEL, g.channels == 2 ? NDSP_FORMAT_STEREO_PCM16 : NDSP_FORMAT_MONO_PCM16);
    float mix[12] = { 0 };
    mix[0] = mix[1] = 1.0f;
    ndspChnSetMix(NDSP_CHANNEL, mix);

    memset(g.wave, 0, sizeof g.wave);
    g.next = 0;
    g.playing = false;
    ac_init(&g.client, g.rate, g.channels, g.password, now * 2654435761u ^ svcGetSystemTick());
    ac_start(&g.client, now);
    g.state = AUDIO_CONNECTING;
    send_pending();
    return true;
}

static void end_stream(void)
{
    if (g.sock >= 0) {
        ac_request_stop(&g.client);
        send_pending();
        close_socket();
    }
    stop_playback();
}

void audio_init(void)
{
    memset(&g, 0, sizeof g);
    g.sock = -1;
    g.port = 8889; g.rate = 24000; g.channels = 2;
    g.state = AUDIO_OFF;
}

void audio_exit(void)
{
    end_stream();
    if (g.dspReady) { ndspExit(); g.dspReady = false; }
    if (g.pcm) { linearFree(g.pcm); g.pcm = NULL; }
}

void audio_configure(const char *host, int port, int rate, int channels, const char *password)
{
    if (rate != 16000 && rate != 24000 && rate != 32000) rate = 24000;
    if (channels != 1 && channels != 2) channels = 2;
    if (port <= 0 || port > 65535) port = 8889;

    const bool same = g.configured && !strcmp(g.host, host) && g.port == port && g.rate == rate &&
                      g.channels == channels && !strcmp(g.password, password);
    if (same) return;

    const bool wasOn = g.enabled;
    if (wasOn) audio_set_enabled(false);
    snprintf(g.host, sizeof g.host, "%s", host);
    snprintf(g.password, sizeof g.password, "%s", password);
    g.port = port; g.rate = rate; g.channels = channels;
    g.addrValid = false;
    g.configured = true;
    if (wasOn) audio_set_enabled(true);
}

void audio_set_enabled(bool on)
{
    if (on == g.enabled) return;
    g.enabled = on;
    if (on) {
        if (!g.configured || !start_stream(osGetTime())) { /* state says why; audio_update retries nothing */ }
    } else {
        end_stream();
        g.state = AUDIO_OFF;
    }
}

bool audio_enabled(void) { return g.enabled; }
AudioState audio_state(void) { return g.state; }

const char *audio_state_text(AudioState s)
{
    switch (s) {
    case AUDIO_OFF:          return "OFF";
    case AUDIO_CONNECTING:   return "CONNECTING";
    case AUDIO_BUFFERING:    return "BUFFERING";
    case AUDIO_PLAYING:      return "PLAYING";
    case AUDIO_BAD_PASSWORD: return "BAD PASSWORD";
    case AUDIO_BLOCKED:      return "BLOCKED";
    case AUDIO_BUSY:         return "PC BUSY";
    case AUDIO_BAD_FORMAT:   return "BAD FORMAT";
    case AUDIO_NO_DSP:       return "NO DSP FILE";
    case AUDIO_NO_HOST:      return "NO HOST";
    }
    return "?";
}

static int queued_count(void)
{
    int n = 0;
    for (int i = 0; i < RING_BUFS; i++)
        if (g.wave[i].status == NDSP_WBUF_QUEUED || g.wave[i].status == NDSP_WBUF_PLAYING) n++;
    return n;
}

void audio_update(u64 now)
{
    if (!g.enabled || g.sock < 0) return;

    // 1. receive
    uint8_t buf[AU_MAX_PACKET];
    for (int i = 0; i < RX_PER_FRAME; i++) {
        const ssize_t n = recv(g.sock, buf, sizeof buf, 0);
        if (n <= 0) break;
        ac_on_packet(&g.client, buf, (int)n, now);
    }

    // 2. handshake, keepalives, and (re)connect timers
    send_pending();

    // 3. what the PC said about us
    AudioClient *c = &g.client;
    if (c->phase == AC_REJECTED) {
        if (g.playing) stop_playback();
        g.state = c->reason == AU_REJ_AUTH ? AUDIO_BAD_PASSWORD
                : c->reason == AU_REJ_FORBIDDEN ? AUDIO_BLOCKED
                : c->reason == AU_REJ_BUSY ? AUDIO_BUSY : AUDIO_BAD_FORMAT;
        return;
    }
    if (c->phase != AC_ACTIVE) {
        if (g.playing) stop_playback();
        g.state = AUDIO_CONNECTING;
        return;
    }

    // 4. feed the DSP
    g.blockSamples = (size_t)c->block * (size_t)c->channels;
    jb_trim(&c->jb, JB_MAX_DEPTH);

    if (!g.playing) {
        g.state = AUDIO_BUFFERING;
        if (jb_depth(&c->jb) < PREBUFFER) return;
        g.playing = true;
    }

    int queued = queued_count();
    while (queued < QUEUE_TARGET) {
        ndspWaveBuf *w = &g.wave[g.next];
        if (w->status == NDSP_WBUF_QUEUED || w->status == NDSP_WBUF_PLAYING) break;   // ring is full
        int16_t *dst = g.pcm + (size_t)g.next * AU_MAX_BLOCK * AU_MAX_CH;
        if (jb_pop(&c->jb, dst) == JB_EMPTY) break;
        DSP_FlushDataCache(dst, (u32)(g.blockSamples * sizeof(int16_t)));
        w->data_vaddr = dst;
        w->nsamples = (u32)c->block;
        w->looping = false;
        w->status = NDSP_WBUF_FREE;
        ndspChnWaveBufAdd(NDSP_CHANNEL, w);
        g.next = (g.next + 1) % RING_BUFS;
        queued++;
    }

    // Ran dry (a network stall): stop and refill, rather than stuttering through silence
    if (queued == 0) { g.playing = false; g.state = AUDIO_BUFFERING; }
    else g.state = AUDIO_PLAYING;
}
