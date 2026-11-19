#ifndef AUDIO_H
#define AUDIO_H

// Plays the PC's sound on the 3DS (speakers or the 3.5 mm jack): the 3DS side of the audio streaming.
// The protocol, decoder and jitter buffer are in audio_core.c; this file adds the UDP socket and the DSP playback.
// Everything here is optional: with audio off, nothing is started and nothing is sent.

#include <3ds.h>
#include <stdbool.h>

typedef enum {
    AUDIO_OFF,
    AUDIO_CONNECTING,      // asking the PC (also: waiting for it to answer)
    AUDIO_BUFFERING,       // connected, filling the buffer
    AUDIO_PLAYING,
    AUDIO_BAD_PASSWORD,    // the PC refused the password
    AUDIO_BLOCKED,         // the PC only accepts local-network addresses and this isn't one
    AUDIO_BUSY,            // the PC is already streaming to another 3DS
    AUDIO_BAD_FORMAT,      // the PC doesn't support the requested sample rate / channels
    AUDIO_NO_DSP,          // sdmc:/3ds/dspfirm.cdc is missing (see the README)
    AUDIO_NO_HOST,         // the host name or IP could not be resolved
} AudioState;

void audio_init(void);
void audio_exit(void);                                   // sends a goodbye to the PC if streaming

// Settings. Changing them while audio is on restarts the stream. `host` may be an IP or a name.
void audio_configure(const char *host, int port, int rate, int channels, const char *password);

void audio_set_enabled(bool on);
bool audio_enabled(void);

// Call once per frame (nowMs: osGetTime()). Receives, decodes, feeds the DSP, and answers the PC.
void audio_update(u64 nowMs);

AudioState audio_state(void);
const char *audio_state_text(AudioState s);              // short, upper case, for the status panel

#endif
