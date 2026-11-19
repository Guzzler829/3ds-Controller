#include "audio_core.h"
#include <string.h>

// ===========================================================================
// SHA-256
// ===========================================================================
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

typedef struct { uint32_t h[8]; uint8_t buf[64]; size_t fill; uint64_t total; } Sha;

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        const uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha *s)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->fill = 0; s->total = 0;
}

static void sha_update(Sha *s, const uint8_t *d, size_t n)
{
    s->total += n;
    while (n > 0) {
        size_t take = 64 - s->fill;
        if (take > n) take = n;
        memcpy(s->buf + s->fill, d, take);
        s->fill += take; d += take; n -= take;
        if (s->fill == 64) { sha_block(s, s->buf); s->fill = 0; }
    }
}

static void sha_final(Sha *s, uint8_t out[32])
{
    const uint64_t bits = s->total * 8;
    const uint8_t one = 0x80, zero = 0;
    sha_update(s, &one, 1);
    while (s->fill != 56) sha_update(s, &zero, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
    sha_update(s, len, 8);
    for (int i = 0; i < 8; i++) { out[i * 4] = (uint8_t)(s->h[i] >> 24); out[i * 4 + 1] = (uint8_t)(s->h[i] >> 16); out[i * 4 + 2] = (uint8_t)(s->h[i] >> 8); out[i * 4 + 3] = (uint8_t)s->h[i]; }
}

void au_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
    Sha s;
    sha_init(&s);
    sha_update(&s, data, len);
    sha_final(&s, out);
}

void au_hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *msg, size_t msglen, uint8_t out[32])
{
    uint8_t k[64], pad[64], inner[32];
    memset(k, 0, sizeof k);
    if (keylen > 64) au_sha256(key, keylen, k); else if (keylen) memcpy(k, key, keylen);
    Sha s;
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha_init(&s); sha_update(&s, pad, 64); sha_update(&s, msg, msglen); sha_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha_init(&s); sha_update(&s, pad, 64); sha_update(&s, inner, 32); sha_final(&s, out);
}

// ===========================================================================
// IMA ADPCM
// ===========================================================================
static const int16_t STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552,
    1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487,
    12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };
static const int8_t INDEX_ADJ[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

void au_adpcm_decode(const uint8_t *nibbles, int samples, int predictor, int index, int16_t *out)
{
    int pred = predictor, idx = index;
    if (idx < 0) idx = 0;
    if (idx > 88) idx = 88;
    for (int i = 0; i < samples; i++) {
        const int code = (i & 1) ? (nibbles[i >> 1] >> 4) : (nibbles[i >> 1] & 15);
        const int step = STEP[idx];
        int diff = step >> 3;
        if (code & 4) diff += step;
        if (code & 2) diff += step >> 1;
        if (code & 1) diff += step >> 2;
        pred += (code & 8) ? -diff : diff;
        if (pred > 32767) pred = 32767;
        if (pred < -32768) pred = -32768;
        idx += INDEX_ADJ[code];
        if (idx < 0) idx = 0;
        if (idx > 88) idx = 88;
        out[i] = (int16_t)pred;
    }
}

// ===========================================================================
// Jitter buffer
// ===========================================================================
static int seq_diff(uint16_t a, uint16_t b) { return (int16_t)(uint16_t)(a - b); }   // a - b, as a signed distance

void jb_reset(JitterBuf *jb, int block, int channels)
{
    memset(jb->valid, 0, sizeof jb->valid);
    jb->any = false;
    jb->next = jb->newest = 0;
    jb->block = block;
    jb->channels = channels;
    jb->received = jb->lost = jb->late = jb->resyncs = 0;
}

void jb_put(JitterBuf *jb, uint16_t seq, const int16_t *pcm)
{
    if (!jb->any) {                                        // the very first block decides where we start
        jb->any = true;
        jb->next = jb->newest = seq;
    }
    const int d = seq_diff(seq, jb->next);
    if (d < 0) { jb->late++; return; }                     // too late: that moment has been played already (or covered)
    if (d >= AU_JB_SLOTS) {                                // far ahead: we lost touch with the stream; start over from here
        memset(jb->valid, 0, sizeof jb->valid);
        jb->next = jb->newest = seq;
        jb->resyncs++;
    }
    const int slot = seq % AU_JB_SLOTS;
    memcpy(jb->pcm[slot], pcm, sizeof(int16_t) * (size_t)jb->block * (size_t)jb->channels);
    jb->seq[slot] = seq;
    jb->valid[slot] = true;
    jb->received++;
    if (seq_diff(seq, jb->newest) > 0) jb->newest = seq;
}

int jb_depth(const JitterBuf *jb)
{
    if (!jb->any) return 0;
    const int d = seq_diff(jb->newest, jb->next) + 1;
    return d < 0 ? 0 : d;
}

int jb_pop(JitterBuf *jb, int16_t *out)
{
    if (!jb->any) return JB_EMPTY;
    const int slot = jb->next % AU_JB_SLOTS;
    const size_t n = (size_t)jb->block * (size_t)jb->channels;
    if (jb->valid[slot] && jb->seq[slot] == jb->next) {
        memcpy(out, jb->pcm[slot], sizeof(int16_t) * n);
        jb->valid[slot] = false;
        jb->next++;
        return JB_OK;
    }
    if (seq_diff(jb->newest, jb->next) >= 2) {             // later blocks are here but this one is not: give it up
        memset(out, 0, sizeof(int16_t) * n);
        jb->next++;
        jb->lost++;
        return JB_LOST;
    }
    return JB_EMPTY;
}

void jb_trim(JitterBuf *jb, int maxDepth)
{
    while (jb_depth(jb) > maxDepth) {
        jb->valid[jb->next % AU_JB_SLOTS] = false;
        jb->next++;
    }
}

// ===========================================================================
// The client's side of the conversation
// ===========================================================================
static uint64_t rng_next(uint64_t *s)
{
    uint64_t x = *s ? *s : 0x9E3779B97F4A7C15ull;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return x * 0x2545F4914F6CDD1Dull;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xFFFF); put16(p + 2, v >> 16); }
static unsigned get16(const uint8_t *p) { return (unsigned)(p[0] | p[1] << 8); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static int header(uint8_t *out, uint8_t type)
{
    out[0] = AU_MAGIC_0; out[1] = AU_MAGIC_1; out[2] = AU_MAGIC_2; out[3] = AU_MAGIC_3; out[4] = type;
    return 5;
}

void ac_init(AudioClient *c, int rate, int channels, const char *password, uint64_t seed)
{
    memset(c, 0, sizeof *c);
    c->phase = AC_IDLE;
    c->rate = rate;
    c->channels = channels;
    c->rng = seed ? seed : 1;
    c->keyLen = 0;
    if (password) {
        size_t n = strlen(password);
        if (n > AU_KEY_MAX) n = AU_KEY_MAX;
        memcpy(c->key, password, n);
        c->keyLen = (int)n;
    }
}

void ac_start(AudioClient *c, uint64_t nowMs)
{
    for (int i = 0; i < 8; i++) c->cnonce[i] = (uint8_t)(rng_next(&c->rng) >> 24);
    c->phase = AC_HELLO;
    c->phaseStartMs = nowMs;
    c->lastSendMs = 0;                 // send HELLO right away
    c->lastRecvMs = nowMs;
    c->session = 0;
    c->stopPending = false;
    c->reason = 0;
    c->block = 0;
}

void ac_request_stop(AudioClient *c)
{
    if (c->phase == AC_ACTIVE) c->stopPending = true; else c->phase = AC_IDLE;
}

int ac_poll(AudioClient *c, uint64_t nowMs, uint8_t *out, int cap)
{
    if (cap < 32) return 0;
    if (c->stopPending) {
        int n = header(out, AU_STOP);
        put32(out + n, c->session); n += 4;
        c->stopPending = false;
        c->phase = AC_IDLE;
        return n;
    }
    switch (c->phase) {
    case AC_IDLE:
        return 0;
    case AC_HELLO:
    case AC_AUTH: {
        if (c->lastSendMs != 0 && nowMs - c->lastSendMs < 1000) {
            if (nowMs - c->lastRecvMs > 8000 && c->phase == AC_AUTH) ac_start(c, nowMs);
            return 0;
        }
        c->lastSendMs = nowMs ? nowMs : 1;
        if (c->phase == AC_HELLO) {
            int n = header(out, AU_HELLO);
            memcpy(out + n, c->cnonce, 8); n += 8;
            put16(out + n, (unsigned)c->rate); n += 2;
            out[n++] = (uint8_t)c->channels;
            return n;
        }
        uint8_t msg[8 + 8 + 8 + 3], mac[32];
        memcpy(msg, "3DA1auth", 8);
        memcpy(msg + 8, c->snonce, 8);
        memcpy(msg + 16, c->cnonce, 8);
        put16(msg + 24, (unsigned)c->rate);
        msg[26] = (uint8_t)c->channels;
        au_hmac_sha256(c->key, (size_t)c->keyLen, msg, sizeof msg, mac);
        int n = header(out, AU_AUTH);
        memcpy(out + n, mac, 16); n += 16;
        return n;
    }
    case AC_ACTIVE:
        if (nowMs - c->lastRecvMs > 4000) { ac_start(c, nowMs); return 0; }          // nothing from the PC for 4 s: start over
        if (nowMs - c->lastSendMs >= 1000) {
            c->lastSendMs = nowMs;
            int n = header(out, AU_KEEPALIVE);
            put32(out + n, c->session); n += 4;
            return n;
        }
        return 0;
    case AC_REJECTED:
        if (nowMs - c->rejectedMs > 5000) ac_start(c, nowMs);                         // try again every 5 s (the PC's settings may have changed)
        return 0;
    }
    return 0;
}

void ac_on_packet(AudioClient *c, const uint8_t *p, int n, uint64_t nowMs)
{
    if (n < 5 || p[0] != AU_MAGIC_0 || p[1] != AU_MAGIC_1 || p[2] != AU_MAGIC_2 || p[3] != AU_MAGIC_3) return;
    const uint8_t type = p[4];
    if (type == AU_CHALLENGE && c->phase == AC_HELLO && n >= 16) {
        memcpy(c->snonce, p + 5, 8);
        if ((int)get16(p + 13) != c->rate || p[15] != c->channels) return;          // the PC must echo what we asked for
        c->phase = AC_AUTH;
        c->lastSendMs = 0;
        c->lastRecvMs = nowMs;
    } else if (type == AU_ACCEPT && c->phase == AC_AUTH && n >= 14) {
        const int rate = (int)get16(p + 9), ch = p[11], block = (int)get16(p + 12);
        if (rate != c->rate || ch != c->channels || block < 2 || (block & 1) || block > AU_MAX_BLOCK) return;
        c->session = get32(p + 5);
        c->block = block;
        jb_reset(&c->jb, block, ch);
        c->phase = AC_ACTIVE;
        c->lastRecvMs = nowMs;
        c->lastSendMs = nowMs;
    } else if (type == AU_REJECT && n >= 6 && (c->phase == AC_HELLO || c->phase == AC_AUTH || c->phase == AC_ACTIVE)) {
        c->reason = p[5];
        c->phase = AC_REJECTED;
        c->rejectedMs = nowMs;
    } else if (type == AU_DATA && c->phase == AC_ACTIVE) {
        const int ch = c->channels, block = c->block;
        const int need = 5 + 4 + 2 + ch * 4 + ch * (block / 2);
        if (n < need || get32(p + 5) != c->session) return;
        c->lastRecvMs = nowMs;
        const uint16_t seq = (uint16_t)get16(p + 9);
        const uint8_t *hdr = p + 11, *data = p + 11 + ch * 4;
        int16_t tmp[AU_MAX_BLOCK], pcm[AU_MAX_BLOCK * AU_MAX_CH];
        for (int k = 0; k < ch; k++) {
            const int pred = (int16_t)get16(hdr + k * 4), idx = hdr[k * 4 + 2];
            au_adpcm_decode(data + k * (block / 2), block, pred, idx, tmp);
            for (int i = 0; i < block; i++) pcm[i * ch + k] = tmp[i];
        }
        jb_put(&c->jb, seq, pcm);
    }
}
