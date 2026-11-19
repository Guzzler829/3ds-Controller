#ifndef AUDIO_CORE_H
#define AUDIO_CORE_H

// Audio streaming, PC -> 3DS: the part that has nothing 3DS-specific in it (so it can be tested on a PC).
//   * the packet format and the handshake (client side)
//   * SHA-256 / HMAC-SHA256 (the handshake proves the 3DS knows the password without sending it)
//   * IMA ADPCM decoding (4 bits per sample)
//   * a small jitter buffer that puts packets back in order and covers lost ones
// The PC side of the same protocol is pc/audio_server.py: keep the two in sync.
//
// Everything is little-endian. Every packet starts with the 4 bytes "3DA1" and a type byte.
//
//   3DS -> PC   HELLO      nonce[8], rate u16, channels u8                      (16 bytes)
//   PC  -> 3DS  CHALLENGE  server nonce[8], rate u16, channels u8               (16 bytes)
//   3DS -> PC   AUTH       HMAC-SHA256(password, "3DA1auth" + server nonce + client nonce + rate + channels)[0..16]
//   PC  -> 3DS  ACCEPT     session u32, rate u16, channels u8, block samples u16
//   3DS -> PC   KEEPALIVE  session u32                                          (about once a second)
//   3DS -> PC   STOP       session u32
//   PC  -> 3DS  REJECT     reason u8
//   PC  -> 3DS  DATA       session u32, sequence u16, then per channel: predictor s16, index u8, 0 u8,
//                          then per channel: block/2 bytes of ADPCM (low nibble first)
//
// The 3DS always starts the conversation, so it also works through a router (NAT): the PC just answers to
// wherever the 3DS's packets came from. A DATA packet carries the decoder state at its start, so a lost
// packet only damages its own 20 ms.

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define AU_MAGIC_0 '3'
#define AU_MAGIC_1 'D'
#define AU_MAGIC_2 'A'
#define AU_MAGIC_3 '1'

enum { AU_HELLO = 1, AU_CHALLENGE = 2, AU_AUTH = 3, AU_ACCEPT = 4, AU_KEEPALIVE = 5, AU_STOP = 6, AU_REJECT = 7, AU_DATA = 8 };
enum { AU_REJ_AUTH = 1, AU_REJ_FORBIDDEN = 2, AU_REJ_BUSY = 3, AU_REJ_FORMAT = 4 };

#define AU_BLOCK_MS     20
#define AU_MAX_CH       2
#define AU_MAX_BLOCK    640           // samples per channel in one block (the most, at 32000 Hz)
#define AU_MAX_PACKET   1400
#define AU_JB_SLOTS     32
#define AU_KEY_MAX      64

// ---------------------------------------------------------------- SHA-256 / HMAC
void au_sha256(const uint8_t *data, size_t len, uint8_t out[32]);
void au_hmac_sha256(const uint8_t *key, size_t keylen, const uint8_t *msg, size_t msglen, uint8_t out[32]);

// ---------------------------------------------------------------- IMA ADPCM
// Decodes `samples` samples (even) from `nibbles` (samples/2 bytes) into out[0], out[1]... starting from the given state.
void au_adpcm_decode(const uint8_t *nibbles, int samples, int predictor, int index, int16_t *out);

// ---------------------------------------------------------------- jitter buffer
enum { JB_EMPTY = 0, JB_OK = 1, JB_LOST = 2 };

typedef struct {
    int16_t  pcm[AU_JB_SLOTS][AU_MAX_BLOCK * AU_MAX_CH];   // interleaved when stereo
    uint16_t seq[AU_JB_SLOTS];
    bool     valid[AU_JB_SLOTS];
    bool     any;                 // has anything arrived since the last reset?
    uint16_t next;                // the sequence number due next
    uint16_t newest;              // the highest sequence number seen
    int      block, channels;
    uint32_t received, lost, late, resyncs;
} JitterBuf;

void jb_reset(JitterBuf *jb, int block, int channels);
void jb_put(JitterBuf *jb, uint16_t seq, const int16_t *pcm);                 // pcm: block * channels samples
int  jb_depth(const JitterBuf *jb);                                           // blocks from `next` up to the newest one
int  jb_pop(JitterBuf *jb, int16_t *out);                                     // JB_OK / JB_LOST (out = silence) / JB_EMPTY
void jb_trim(JitterBuf *jb, int maxDepth);                                    // drops the oldest blocks so depth <= maxDepth

// ---------------------------------------------------------------- the client side of the conversation
typedef enum { AC_IDLE, AC_HELLO, AC_AUTH, AC_ACTIVE, AC_REJECTED } AcPhase;

typedef struct {
    AcPhase  phase;
    uint8_t  key[AU_KEY_MAX];
    int      keyLen;
    int      rate, channels;      // what we ask for
    int      block;               // samples per channel per block, once accepted
    uint8_t  cnonce[8], snonce[8];
    uint32_t session;
    int      reason;              // AU_REJ_* after a reject
    uint64_t lastSendMs, lastRecvMs, phaseStartMs, rejectedMs;
    uint64_t rng;
    bool     stopPending;
    JitterBuf jb;
} AudioClient;

void ac_init(AudioClient *c, int rate, int channels, const char *password, uint64_t seed);
void ac_start(AudioClient *c, uint64_t nowMs);                // begin (or restart) the handshake
void ac_request_stop(AudioClient *c);                         // queues one STOP packet (if a session is open)
// Fills `out` with the next packet to send (returns its length) or returns 0 if there is nothing to send now.
int  ac_poll(AudioClient *c, uint64_t nowMs, uint8_t *out, int cap);
// Feeds a packet that arrived from the PC.
void ac_on_packet(AudioClient *c, const uint8_t *p, int n, uint64_t nowMs);

#endif
