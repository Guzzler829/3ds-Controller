#!/usr/bin/env python3
"""3DS Controller - audio streaming, PC -> 3DS (v2.2.0)

Captures what the PC is playing ("what you hear") and sends it to the 3DS, which plays it from its speakers
or headphone jack. The 3DS starts the conversation (so it also works through a router: nothing but the
audio port has to be forwarded to the PC), proves it knows the password, and then the PC sends a stream of
small UDP packets (IMA ADPCM, 4 bits per sample, 20 ms per packet) to wherever the 3DS's packets came from.

Needs:   pip install numpy soundcard
         (soundcard captures the system output: WASAPI loopback on Windows, a PulseAudio/PipeWire
          "monitor" on Linux)

Normally started from pc.py with --audio. It can also run on its own:
    python3 audio_server.py [--port 8889] [--password SECRET] [--device NAME] [--test-tone]

SAFETY
  * With no password only 3DSes on your own network (private addresses, including Tailscale/ZeroTier)
    are allowed. To use it from the internet you MUST set a password (a long random one).
  * Audio is sent unencrypted. Don't stream anything private over the internet.
  * Only this audio port needs to be forwarded for audio over the internet. The controller port (8888)
    has no password: do not forward it.
"""
import argparse, collections, hmac, hashlib, ipaddress, os, socket, struct, sys, threading, time

AUDIO_PORT = 8889
MAGIC = b"3DA1"
HELLO, CHALLENGE, AUTH, ACCEPT, KEEPALIVE, STOP, REJECT, DATA = range(1, 9)
REJ_AUTH, REJ_FORBIDDEN, REJ_BUSY, REJ_FORMAT = 1, 2, 3, 4

CAPTURE_RATE = 48000
BLOCK_MS = 20
ALLOWED_RATES = (16000, 24000, 32000)
SESSION_TIMEOUT = 4.0          # seconds without a keepalive before a session is dropped
CAPTURE_IDLE_STOP = 5.0        # seconds after the last session before the capture device is released

# ---------------------------------------------------------------------------
# IMA ADPCM (the 3DS side is au_adpcm_decode in source/audio_core.c)
# ---------------------------------------------------------------------------
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428,
        4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350,
        22385, 24623, 27086, 29794, 32767]
INDEX_ADJ = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]
# per step index: (step, step>>1, step>>2, step>>3): saves work in the inner loop
_ST = [(s, s >> 1, s >> 2, s >> 3) for s in STEP]
# the index after a code, for every (index, code): saves more work
_NEXT = [[min(88, max(0, i + INDEX_ADJ[c])) for c in range(16)] for i in range(89)]


class AdpcmEncoder:
    """Encodes one channel. The state (predictor and step index) carries on from block to block."""

    def __init__(self):
        self.pred = 0
        self.idx = 0

    def encode_block(self, samples):
        """samples: a list of ints (-32768..32767), even length. Returns (predictor, index, bytes) where the first
        two are the state at the START of the block (the decoder starts from them)."""
        pred0, idx0 = self.pred, self.idx
        pred, idx = pred0, idx0
        st, nxt = _ST, _NEXT
        out = bytearray(len(samples) // 2)
        low = True
        o = 0
        cur = 0
        for s in samples:
            step, h1, h2, h3 = st[idx]
            diff = s - pred
            code = 0
            if diff < 0:
                code = 8
                diff = -diff
            vp = h3
            if diff >= step:
                code |= 4
                diff -= step
                vp += step
            if diff >= h1:
                code |= 2
                diff -= h1
                vp += h1
            if diff >= h2:
                code |= 1
                vp += h2
            if code & 8:
                pred -= vp
                if pred < -32768:
                    pred = -32768
            else:
                pred += vp
                if pred > 32767:
                    pred = 32767
            idx = nxt[idx][code]
            if low:
                cur = code
                low = False
            else:
                out[o] = cur | (code << 4)
                o += 1
                low = True
        self.pred, self.idx = pred, idx
        return pred0, idx0, bytes(out)


def adpcm_decode(data, samples, pred, idx):
    """Reference decoder (the tests compare it with the C one)."""
    out = []
    for i in range(samples):
        code = (data[i >> 1] >> 4) if (i & 1) else (data[i >> 1] & 15)
        step = STEP[idx]
        diff = step >> 3
        if code & 4: diff += step
        if code & 2: diff += step >> 1
        if code & 1: diff += step >> 2
        pred += -diff if code & 8 else diff
        pred = max(-32768, min(32767, pred))
        idx = max(0, min(88, idx + INDEX_ADJ[code]))
        out.append(pred)
    return out


# ---------------------------------------------------------------------------
# Resampling: capture at 48 kHz, deliver at 16 / 24 / 32 kHz (low-pass filter, then linear interpolation)
# ---------------------------------------------------------------------------
class Resampler:
    def __init__(self, in_rate, out_rate, channels, np):
        self.np = np
        self.ratio = in_rate / float(out_rate)
        self.taps = 63
        cutoff = 0.46 * min(1.0, out_rate / float(in_rate))            # as a fraction of the input rate
        n = np.arange(self.taps) - (self.taps - 1) / 2.0
        h = np.sinc(2 * cutoff * n) * 2 * cutoff * np.hamming(self.taps)
        self.h = (h / h.sum()).astype(np.float32)
        self.hist = np.zeros((self.taps - 1, channels), dtype=np.float32)
        self.channels = channels

    def process(self, x, out_count):
        """x: float32 array (frames, channels). Returns exactly out_count frames."""
        np = self.np
        buf = np.concatenate([self.hist, x], axis=0)
        self.hist = buf[-(self.taps - 1):]
        filt = np.empty((x.shape[0], self.channels), dtype=np.float32)
        for c in range(self.channels):
            filt[:, c] = np.convolve(buf[:, c], self.h, mode="valid")[:x.shape[0]]
        pos = np.arange(out_count, dtype=np.float64) * self.ratio
        i0 = np.minimum(pos.astype(np.int64), x.shape[0] - 2)
        frac = (pos - i0).astype(np.float32)[:, None]
        return filt[i0] * (1.0 - frac) + filt[i0 + 1] * frac


# ---------------------------------------------------------------------------
# Where the sound comes from
# ---------------------------------------------------------------------------
class ToneSource:
    """A test tone (440 Hz left, 660 Hz right), paced in real time. Needs only numpy."""

    def __init__(self, np):
        self.np = np
        self.t = 0
        self.next = time.monotonic()

    def read(self, frames):
        np = self.np
        self.next += frames / float(CAPTURE_RATE)
        delay = self.next - time.monotonic()
        if delay > 0:
            time.sleep(delay)
        n = np.arange(self.t, self.t + frames) / float(CAPTURE_RATE)
        self.t += frames
        left = 0.4 * np.sin(2 * np.pi * 440 * n)
        right = 0.4 * np.sin(2 * np.pi * 660 * n)
        return np.stack([left, right], axis=1).astype(np.float32)

    def close(self):
        pass


class PulseRedirect:
    """Linux (PulseAudio / PipeWire): sends the PC's sound to a silent virtual output while a 3DS is listening, so the
    PC's own speakers stay quiet. Capturing the speakers' "monitor" can't do that, because muting the speakers
    mutes the copy too. start() makes a virtual output ("3DS_Audio"), makes it the default and moves the playing
    programs onto it; stop() puts everything back. `run` runs a pactl command and returns its output (a test replaces it)."""
    SINK = "g3d_3ds_audio"
    DESC = "3DS_Audio"

    def __init__(self, run=None):
        self.run = run or self._pactl
        self.old_default = None
        self.module = None
        self.active = False

    @staticmethod
    def _pactl(*args):
        import subprocess
        return subprocess.run(["pactl"] + list(args), check=True, capture_output=True, text=True, timeout=10).stdout

    def _default_sink(self):
        for line in self.run("info").splitlines():
            if line.startswith("Default Sink:"):
                return line.split(":", 1)[1].strip()
        return None

    def _sink_index(self, name):
        for line in self.run("list", "short", "sinks").splitlines():
            parts = line.split("\t")
            if len(parts) > 1 and parts[1] == name:
                return parts[0]
        return None

    def _inputs(self):
        """[(input id, sink index it is playing on)]"""
        out = []
        for line in self.run("list", "short", "sink-inputs").splitlines():
            parts = line.split("\t")
            if len(parts) > 1:
                out.append((parts[0], parts[1]))
        return out

    def start(self):
        for line in self.run("list", "short", "modules").splitlines():          # a leftover from a crashed run
            parts = line.split("\t")
            if len(parts) > 2 and parts[1] == "module-null-sink" and "sink_name=" + self.SINK in line:
                self.run("unload-module", parts[0])
        self.old_default = self._default_sink()
        if not self.old_default:
            raise RuntimeError("could not find the default output with pactl")
        self.module = self.run("load-module", "module-null-sink", "sink_name=" + self.SINK,
                               "sink_properties=device.description=" + self.DESC).strip()
        self.active = True
        try:
            self.run("set-default-sink", self.SINK)
            idx = self._sink_index(self.SINK)
            for inp, sink in self._inputs():
                if sink != idx:
                    self.run("move-sink-input", inp, self.SINK)
        except Exception:
            self.stop()
            raise

    def stop(self):
        if not self.active:
            return
        self.active = False
        try:
            idx = self._sink_index(self.SINK)
            if self.old_default and self._sink_index(self.old_default) is not None:
                self.run("set-default-sink", self.old_default)
                for inp, sink in self._inputs():
                    if sink == idx:
                        self.run("move-sink-input", inp, self.old_default)
        except Exception:
            pass
        try:
            if self.module:
                self.run("unload-module", self.module)
        except Exception:
            pass


class LoopbackSource:
    """What the PC is playing, through the `soundcard` package."""

    def __init__(self, np, device=None, redirect=None):
        import soundcard as sc
        self.np = np
        self.redirect = redirect
        try:
            self._open(sc, device)
        except Exception:
            if redirect:
                redirect.stop()
            raise

    def _open(self, sc, device):
        np = self.np
        if device:
            spk = None
            for attempt in range(10):                      # a just-created virtual output can take a moment to appear
                for s in sc.all_speakers():
                    if device.lower() in s.name.lower():
                        spk = s
                        break
                if spk is not None:
                    break
                time.sleep(0.2)
            if spk is None:
                raise RuntimeError("No output device with %r in its name. Use --list-audio-devices." % device)
        else:
            spk = sc.default_speaker()
        self.name = spk.name
        mic = sc.get_microphone(id=str(spk.name), include_loopback=True)
        self.ctx = mic.recorder(samplerate=CAPTURE_RATE, channels=2, blocksize=CAPTURE_RATE // 25)
        self.rec = self.ctx.__enter__()

    def read(self, frames):
        data = self.rec.record(numframes=frames)
        if data.ndim == 1:
            data = data[:, None]
        if data.shape[1] == 1:
            data = self.np.repeat(data, 2, axis=1)
        return data[:, :2].astype(self.np.float32)

    def close(self):
        try:
            self.ctx.__exit__(None, None, None)
        except Exception:
            pass
        if self.redirect:
            self.redirect.stop()


def list_devices():
    try:
        import soundcard as sc
    except ImportError:
        print("The 'soundcard' package is not installed: pip install numpy soundcard")
        return
    default = sc.default_speaker()
    for s in sc.all_speakers():
        print("%s%s" % (s.name, "   (default)" if s.name == default.name else ""))


# ---------------------------------------------------------------------------
# The server
# ---------------------------------------------------------------------------
class Session:
    def __init__(self, addr, rate, channels):
        self.addr = addr
        self.rate = rate
        self.channels = channels
        self.id = struct.unpack("<I", os.urandom(4))[0]
        self.seq = 0
        self.last_seen = time.monotonic()
        self.enc = [AdpcmEncoder() for _ in range(channels)]


def is_lan(ip_text):
    """True for addresses that are not on the public internet (private ranges, loopback, link-local, CGNAT / Tailscale)."""
    try:
        return not ipaddress.ip_address(ip_text).is_global
    except ValueError:
        return False


class AudioServer:
    def __init__(self, port=AUDIO_PORT, password="", device=None, test_tone=False, log=print, source_factory=None, silence_pc=None):
        self.port = port
        self.key = password.encode("utf-8")
        self.device = device
        self.test_tone = test_tone
        self.log = log
        self.source_factory = source_factory
        # Linux: keep the PC's own speakers quiet while a 3DS listens (default on; off with --audio-pc-sound)
        self.silence_pc = sys.platform.startswith("linux") if silence_pc is None else silence_pc
        self.redirect = None
        self.sock = None
        self.session = None
        self.pending = {}              # addr -> (server nonce, client nonce, rate, channels, time)
        self.failures = {}             # ip -> (count, time of first failure)
        self.lock = threading.Lock()
        self.stop_flag = threading.Event()
        self.threads = []
        self.np = None
        self.sent_blocks = 0

    # ---- start / stop ----
    def start(self):
        try:
            import numpy
        except ImportError:
            raise RuntimeError("Audio needs numpy: pip install numpy soundcard")
        self.np = numpy
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("0.0.0.0", self.port))
        self.sock.settimeout(0.2)
        for target in (self._rx_loop, self._tx_loop):
            t = threading.Thread(target=target, daemon=True)
            t.start()
            self.threads.append(t)
        self.log("Audio: listening on UDP port %d (%s)" % (
            self.port, "password set: the internet is allowed" if self.key else "no password: local network only"))

    def stop(self):
        self.stop_flag.set()
        for t in self.threads:
            t.join(timeout=2)
        if self.redirect:
            self.redirect.stop()           # always give the PC its sound back
        if self.sock:
            self.sock.close()

    # ---- the conversation ----
    def _send(self, addr, data):
        try:
            self.sock.sendto(data, addr)
        except OSError:
            pass

    def _reject(self, addr, reason):
        self._send(addr, MAGIC + bytes([REJECT, reason]))

    def _auth_blocked(self, ip, now):
        count, first = self.failures.get(ip, (0, now))
        if now - first > 60:
            self.failures.pop(ip, None)
            return False
        return count >= 10

    def _note_failure(self, ip, now):
        count, first = self.failures.get(ip, (0, now))
        if now - first > 60:
            count, first = 0, now
        self.failures[ip] = (count + 1, first)

    def handle(self, data, addr, now=None):
        """One packet from the network. (Public so the tests can call it.)"""
        now = time.monotonic() if now is None else now
        if len(data) < 5 or data[:4] != MAGIC:
            return
        kind, ip = data[4], addr[0]
        with self.lock:
            if self.session and now - self.session.last_seen > SESSION_TIMEOUT:
                self.log("Audio: %s timed out" % self.session.addr[0])
                self.session = None

            if kind == HELLO and len(data) >= 16:
                if not self.key and not is_lan(ip):
                    self._reject(addr, REJ_FORBIDDEN)
                    return
                if self._auth_blocked(ip, now):
                    return
                rate, channels = struct.unpack("<HB", data[13:16])
                if rate not in ALLOWED_RATES or channels not in (1, 2):
                    self._reject(addr, REJ_FORMAT)
                    return
                if self.session and self.session.addr != addr:
                    self._reject(addr, REJ_BUSY)
                    return
                for a in [a for a, v in self.pending.items() if now - v[4] > 10]:
                    del self.pending[a]
                if len(self.pending) > 32:
                    return
                snonce = os.urandom(8)
                self.pending[addr] = (snonce, data[5:13], rate, channels, now)
                self._send(addr, MAGIC + bytes([CHALLENGE]) + snonce + struct.pack("<HB", rate, channels))

            elif kind == AUTH and len(data) >= 21 and addr in self.pending:
                snonce, cnonce, rate, channels, _ = self.pending.pop(addr)
                msg = b"3DA1auth" + snonce + cnonce + struct.pack("<HB", rate, channels)
                good = hmac.new(self.key, msg, hashlib.sha256).digest()[:16]
                if not hmac.compare_digest(good, data[5:21]):
                    self._note_failure(ip, now)
                    self._reject(addr, REJ_AUTH)
                    self.log("Audio: wrong password from %s" % ip)
                    return
                if self.session and self.session.addr != addr:
                    self._reject(addr, REJ_BUSY)
                    return
                self.session = Session(addr, rate, channels)
                block = rate * BLOCK_MS // 1000
                self._send(addr, MAGIC + bytes([ACCEPT]) + struct.pack("<IHBH", self.session.id, rate, channels, block))
                self.log("Audio: streaming to %s:%d (%d Hz, %s)" % (addr[0], addr[1], rate, "stereo" if channels == 2 else "mono"))

            elif kind in (KEEPALIVE, STOP) and len(data) >= 9 and self.session:
                sid = struct.unpack("<I", data[5:9])[0]
                if sid == self.session.id and addr == self.session.addr:
                    self.session.last_seen = now
                    if kind == STOP:
                        self.log("Audio: %s stopped the stream" % ip)
                        self.session = None

    def _rx_loop(self):
        while not self.stop_flag.is_set():
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                if self.stop_flag.is_set():
                    return
                continue
            try:
                self.handle(data, addr)
            except Exception as e:
                self.log("Audio: error handling a packet: %s" % e)

    # ---- the audio ----
    def _make_source(self):
        if self.source_factory:
            return self.source_factory(self.np)
        if self.test_tone:
            return ToneSource(self.np)
        if self.silence_pc and sys.platform.startswith("linux") and not self.device:
            redirect = PulseRedirect()
            try:
                redirect.start()
                self.redirect = redirect
                self.log("Audio: the PC's sound now goes to the 3DS only (virtual output '%s'); it is restored when the 3DS stops" % PulseRedirect.DESC)
                return LoopbackSource(self.np, PulseRedirect.DESC, redirect)
            except Exception as e:
                redirect.stop()
                self.log("Audio: couldn't silence the PC's speakers (%s); capturing them instead, so the PC will play it too" % e)
        return LoopbackSource(self.np, self.device)

    def _tx_loop(self):
        np = self.np
        source = None
        capture_q = collections.deque(maxlen=12)
        capture_thread = None
        capture_stop = threading.Event()
        idle_since = None
        resamplers = {}
        frames = CAPTURE_RATE * BLOCK_MS // 1000

        def capture():
            try:
                while not capture_stop.is_set():
                    capture_q.append(source.read(frames))
            except Exception as e:
                self.log("Audio: capture stopped: %s" % e)

        while not self.stop_flag.is_set():
            with self.lock:
                sess = self.session
                if sess and time.monotonic() - sess.last_seen > SESSION_TIMEOUT:
                    self.log("Audio: %s timed out" % sess.addr[0])
                    self.session = sess = None
            if sess is None:
                if source is not None:
                    idle_since = idle_since or time.monotonic()
                    if time.monotonic() - idle_since > CAPTURE_IDLE_STOP:           # nobody listening: let go of the sound device
                        capture_stop.set()
                        capture_thread.join(timeout=2)
                        source.close()
                        source, capture_thread, idle_since = None, None, None
                        capture_q.clear()
                        resamplers.clear()
                        capture_stop = threading.Event()
                time.sleep(0.05)
                if source is not None:
                    capture_q.clear()
                continue
            idle_since = None
            if source is None:
                try:
                    source = self._make_source()
                except Exception as e:
                    self.log("Audio: can't capture the sound: %s" % e)
                    with self.lock:
                        self.session = None
                    time.sleep(1)
                    continue
                capture_thread = threading.Thread(target=capture, daemon=True)
                capture_thread.start()
                self.log("Audio: capturing %s" % getattr(source, "name", "the test tone"))

            if not capture_q:
                time.sleep(0.004)
                # nothing captured (some systems deliver nothing while it is silent): keep the clock going with silence
                if not hasattr(self, "_silence_at") or time.monotonic() - self._silence_at > 0.05 + BLOCK_MS / 1000.0:
                    self._silence_at = time.monotonic()
                    block = np.zeros((frames, 2), dtype=np.float32)
                else:
                    continue
            else:
                block = capture_q.popleft()
                self._silence_at = time.monotonic()

            key = (sess.rate, sess.channels)
            rs = resamplers.get(key)
            if rs is None:
                rs = resamplers[key] = Resampler(CAPTURE_RATE, sess.rate, 2, np)
            n = sess.rate * BLOCK_MS // 1000
            out = rs.process(block, n)
            if sess.channels == 1:
                out = out.mean(axis=1, keepdims=True)
            pcm = np.clip(out * 32767.0, -32768, 32767).astype(np.int16)
            headers, bodies = b"", b""
            for ch in range(sess.channels):
                pred, idx, body = sess.enc[ch].encode_block(pcm[:, ch].tolist())
                headers += struct.pack("<hBB", pred, idx, 0)
                bodies += body
            pkt = MAGIC + bytes([DATA]) + struct.pack("<IH", sess.id, sess.seq & 0xFFFF) + headers + bodies
            sess.seq += 1
            self._send(sess.addr, pkt)
            self.sent_blocks += 1
        capture_stop.set()
        if source is not None:
            source.close()


# ---------------------------------------------------------------------------
def add_arguments(parser):
    g = parser.add_argument_group("audio (PC -> 3DS)")
    g.add_argument("--audio", action="store_true", help="stream the PC's sound to the 3DS")
    g.add_argument("--audio-port", type=int, default=AUDIO_PORT, help="UDP port for audio (default %d)" % AUDIO_PORT)
    g.add_argument("--audio-password", default=os.environ.get("G3D_AUDIO_PASSWORD", ""),
                   help="required for the internet; must match audiopass= in the 3DS's config.ini "
                        "(or set the environment variable G3D_AUDIO_PASSWORD)")
    g.add_argument("--audio-device", default=None, help="part of the name of the output device to capture (default: the default one)")
    g.add_argument("--audio-pc-sound", action="store_true",
                   help="Linux: keep playing on the PC's own speakers too (by default they go quiet while the 3DS listens)")
    g.add_argument("--audio-test", action="store_true", help="send a test tone instead of the PC's sound")
    g.add_argument("--list-audio-devices", action="store_true", help="list the output devices and exit")


def main():
    parser = argparse.ArgumentParser(description="3DS Controller audio streaming")
    add_arguments(parser)
    args = parser.parse_args()
    if args.list_audio_devices:
        list_devices()
        return
    server = AudioServer(args.audio_port, args.audio_password, args.audio_device, args.audio_test, silence_pc=not args.audio_pc_sound)
    try:
        server.start()
    except RuntimeError as e:
        print(e)
        sys.exit(1)
    print("Press Ctrl+C to stop")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    server.stop()


if __name__ == "__main__":
    main()
