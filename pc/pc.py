#!/usr/bin/env python3
"""3DS Controller - PC receiver (v2.0.0)

Turns the packets from the 3DS into:
  * a virtual gamepad          (controller mode)
  * a virtual mouse + keyboard (mouse and keyboard modes)

Linux:   uses uinput (three virtual devices: gamepad, mouse, keyboard)
Windows: uses vgamepad for the gamepad and SendInput for the mouse/keyboard

Run with --debug to see what is arriving.
"""
import socket, struct, time, signal, sys, os, datetime, platform, math
from collections import namedtuple

PORT = 8888

# ---------------------------------------------------------------------------
# Packet format. Must match source/protocol.h in the 3DS app.
#   v1.x (12 bytes): buttons, circle pad x/y, C-stick x/y
#   v2.0 (22 bytes): the same 12 bytes, then mode, flags, mouse dx/dy,
#                    mouse buttons, modifiers, key, reserved
# ---------------------------------------------------------------------------
PACKET_V1 = struct.Struct("<Ihhhh")
PACKET_V2 = struct.Struct("<IhhhhBBhhBBBB")

Packet = namedtuple("Packet", "buttons circle_x circle_y cstick_x cstick_y "
                              "mode flags mouse_dx mouse_dy mouse_buttons modifiers key")

MODE_CONTROLLER, MODE_MOUSE, MODE_KEYBOARD = 0, 1, 2
FLAG_CIRCLE_SCROLL, FLAG_CSTICK_SCROLL, FLAG_NEW3DS = 1, 2, 4
MOUSE_LEFT = 1   # a tap on the 3DS trackpad

# 3DS buttons (the bit positions libctru uses)
KEY_A, KEY_B, KEY_SELECT, KEY_START = 1, 2, 4, 8
KEY_DRIGHT, KEY_DLEFT, KEY_DUP, KEY_DDOWN = 16, 32, 64, 128
KEY_R, KEY_L, KEY_X, KEY_Y = 256, 512, 1024, 2048
KEY_ZL, KEY_ZR = 16384, 32768


def parse_packet(data):
    """Returns a Packet, or None if the data is too short. Old 12-byte packets still work."""
    if len(data) >= PACKET_V2.size:
        return Packet(*PACKET_V2.unpack(data[:PACKET_V2.size])[:12])
    if len(data) >= PACKET_V1.size:
        return Packet(*PACKET_V1.unpack(data[:PACKET_V1.size]), MODE_CONTROLLER, 0, 0, 0, 0, 0, 0)
    return None


# ---------------------------------------------------------------------------
# Keys. The 3DS sends USB HID usage IDs; this table says what each one is on
# Linux (KEY_<name>) and on Windows (virtual-key code, and whether it is an
# "extended" key). Modifiers are HID 0xE0 + bit number of the modifiers byte.
# ---------------------------------------------------------------------------
def _build_keymap():
    km = {}
    for i in range(26):                                   # A-Z
        km[0x04 + i] = (chr(ord("A") + i), 0x41 + i, False)
    for i, ch in enumerate("1234567890"):                 # 1-9, 0
        km[0x1E + i] = (ch, ord(ch), False)
    for i in range(12):                                   # F1-F12
        km[0x3A + i] = ("F%d" % (i + 1), 0x70 + i, False)
    special = [
        (0x28, "ENTER", 0x0D, False), (0x29, "ESC", 0x1B, False),
        (0x2A, "BACKSPACE", 0x08, False), (0x2B, "TAB", 0x09, False),
        (0x2C, "SPACE", 0x20, False), (0x2D, "MINUS", 0xBD, False),
        (0x2E, "EQUAL", 0xBB, False), (0x2F, "LEFTBRACE", 0xDB, False),
        (0x30, "RIGHTBRACE", 0xDD, False), (0x31, "BACKSLASH", 0xDC, False),
        (0x33, "SEMICOLON", 0xBA, False), (0x34, "APOSTROPHE", 0xDE, False),
        (0x35, "GRAVE", 0xC0, False), (0x36, "COMMA", 0xBC, False),
        (0x37, "DOT", 0xBE, False), (0x38, "SLASH", 0xBF, False),
        (0x39, "CAPSLOCK", 0x14, False),
        (0x49, "INSERT", 0x2D, True), (0x4A, "HOME", 0x24, True),
        (0x4B, "PAGEUP", 0x21, True), (0x4C, "DELETE", 0x2E, True),
        (0x4D, "END", 0x23, True), (0x4E, "PAGEDOWN", 0x22, True),
        (0x4F, "RIGHT", 0x27, True), (0x50, "LEFT", 0x25, True),
        (0x51, "DOWN", 0x28, True), (0x52, "UP", 0x26, True),
        (0x65, "COMPOSE", 0x5D, True),                    # the Menu / Application key
        (0xE0, "LEFTCTRL", 0xA2, False), (0xE1, "LEFTSHIFT", 0xA0, False),
        (0xE2, "LEFTALT", 0xA4, False), (0xE3, "LEFTMETA", 0x5B, True),
        (0xE4, "RIGHTCTRL", 0xA3, True), (0xE5, "RIGHTSHIFT", 0xA1, False),
        (0xE6, "RIGHTALT", 0xA5, True), (0xE7, "RIGHTMETA", 0x5C, True),
    ]
    for hid, name, vk, ext in special:
        km[hid] = (name, vk, ext)
    return km


KEYMAP = _build_keymap()
HID_ENTER, HID_RIGHT, HID_LEFT, HID_DOWN, HID_UP = 0x28, 0x4F, 0x50, 0x51, 0x52
FIRST_MODIFIER_HID = 0xE0

# ---------------------------------------------------------------------------
# Tuning
# ---------------------------------------------------------------------------
STICK_DEADZONE = 20       # Circle Pad / C-Stick values (about -156..156) below this are ignored
STICK_MAX = 140           # ...and this much counts as "all the way"
STICK_CURVE = 2.0         # 1 = linear; higher = finer control near the center
CURSOR_MAX_SPEED = 18.0   # cursor pixels per packet at full deflection (about 1000 px/s at 60 Hz)
SCROLL_MAX_SPEED = 0.30   # scroll-wheel notches per packet at full deflection (about 18 notches/s)
STALE_AFTER = 1.0         # seconds without a packet before everything held down is released

# ---------------------------------------------------------------------------
# Gamepad axis helpers (same behaviour as v1.1)
# ---------------------------------------------------------------------------
def map_axis_value(value, min_in=-140, max_in=140, min_out=-32768, max_out=32767):
    if abs(value) < 20:
        return 0
    value = max(min_in, min(max_in, value))
    return int(((value - min_in) * (max_out - min_out)) / (max_in - min_in) + min_out)


def map_axis_to_uinput(value):
    return int((value + 32768) * 255 / 65535)


def stick_amount(v):
    """-1.0 .. 1.0 from a raw stick value, with dead zone and response curve."""
    a = abs(v)
    if a <= STICK_DEADZONE:
        return 0.0
    n = min(1.0, (a - STICK_DEADZONE) / float(STICK_MAX - STICK_DEADZONE)) ** STICK_CURVE
    return math.copysign(n, v)


# ---------------------------------------------------------------------------
# Backends: the only code that touches the operating system.
# ---------------------------------------------------------------------------
class Backend:
    def gamepad_update(self, pkt): raise NotImplementedError
    def gamepad_reset(self): raise NotImplementedError
    def mouse_move(self, dx, dy): raise NotImplementedError          # relative, in pixels
    def mouse_wheel(self, notches): raise NotImplementedError        # + = up
    def mouse_button(self, name, down): raise NotImplementedError    # left right back forward
    def key(self, hid, down): raise NotImplementedError
    def close(self): pass


class LinuxBackend(Backend):
    def __init__(self, uinput):
        self.u = uinput
        pad_events = (
            uinput.BTN_A, uinput.BTN_B, uinput.BTN_X, uinput.BTN_Y,
            uinput.BTN_TL, uinput.BTN_TR, uinput.BTN_TL2, uinput.BTN_TR2,
            uinput.BTN_START, uinput.BTN_SELECT, uinput.BTN_THUMBL, uinput.BTN_THUMBR,
            uinput.ABS_X + (0, 255, 0, 0), uinput.ABS_Y + (0, 255, 0, 0),
            uinput.ABS_RX + (0, 255, 0, 0), uinput.ABS_RY + (0, 255, 0, 0),
            uinput.ABS_Z + (0, 255, 0, 0), uinput.ABS_RZ + (0, 255, 0, 0),
            uinput.ABS_HAT0X + (-1, 1, 0, 0), uinput.ABS_HAT0Y + (-1, 1, 0, 0),
        )
        self.pad = uinput.Device(pad_events, name="3DS Controller")
        self.mouse = uinput.Device((
            uinput.REL_X, uinput.REL_Y, uinput.REL_WHEEL,
            uinput.BTN_LEFT, uinput.BTN_RIGHT, uinput.BTN_MIDDLE,
            uinput.BTN_SIDE, uinput.BTN_EXTRA,                  # the "back" and "forward" buttons
        ), name="3DS Mouse")
        keys = [getattr(uinput, "KEY_" + name) for name, _vk, _ext in KEYMAP.values()]
        self.kbd = uinput.Device(keys, name="3DS Keyboard")
        self._mouse_buttons = {"left": uinput.BTN_LEFT, "right": uinput.BTN_RIGHT,
                               "back": uinput.BTN_SIDE, "forward": uinput.BTN_EXTRA}
        self._pad_last = {}
        time.sleep(0.5)                                         # let the system notice the new devices
        self.gamepad_reset()
        print("Linux uinput devices ready: gamepad, mouse, keyboard")

    def _pad_set(self, changes):
        """Sends only the values that changed, with one sync at the end."""
        dirty = False
        for ev, value in changes:
            if self._pad_last.get(ev) != value:
                self._pad_last[ev] = value
                self.pad.emit(ev, value, syn=False)
                dirty = True
        if dirty:
            self.pad.syn()

    def gamepad_reset(self):
        u = self.u
        self._pad_last = {}
        changes = [(b, 0) for b in (u.BTN_A, u.BTN_B, u.BTN_X, u.BTN_Y, u.BTN_TL, u.BTN_TR, u.BTN_TL2,
                                    u.BTN_TR2, u.BTN_START, u.BTN_SELECT)]
        changes += [(a, 128) for a in (u.ABS_X, u.ABS_Y, u.ABS_RX, u.ABS_RY)]
        changes += [(a, 0) for a in (u.ABS_Z, u.ABS_RZ, u.ABS_HAT0X, u.ABS_HAT0Y)]
        self._pad_set(changes)

    def gamepad_update(self, pkt):
        u, b = self.u, pkt.buttons
        changes = [
            (u.BTN_A, 1 if b & KEY_A else 0), (u.BTN_B, 1 if b & KEY_B else 0),
            (u.BTN_X, 1 if b & KEY_X else 0), (u.BTN_Y, 1 if b & KEY_Y else 0),
            (u.BTN_TL, 1 if b & KEY_L else 0), (u.BTN_TR, 1 if b & KEY_R else 0),
            (u.BTN_START, 1 if b & KEY_START else 0), (u.BTN_SELECT, 1 if b & KEY_SELECT else 0),
            (u.BTN_TL2, 1 if b & KEY_ZL else 0), (u.BTN_TR2, 1 if b & KEY_ZR else 0),
            (u.ABS_Z, 255 if b & KEY_ZL else 0), (u.ABS_RZ, 255 if b & KEY_ZR else 0),
            (u.ABS_HAT0Y, -1 if b & KEY_DUP else 1 if b & KEY_DDOWN else 0),
            (u.ABS_HAT0X, -1 if b & KEY_DLEFT else 1 if b & KEY_DRIGHT else 0),
            (u.ABS_X, map_axis_to_uinput(map_axis_value(pkt.circle_x))),
            (u.ABS_Y, map_axis_to_uinput(map_axis_value(-pkt.circle_y))),
            (u.ABS_RX, map_axis_to_uinput(map_axis_value(pkt.cstick_x))),
            (u.ABS_RY, map_axis_to_uinput(map_axis_value(-pkt.cstick_y))),
        ]
        self._pad_set(changes)

    def mouse_move(self, dx, dy):
        if dx:
            self.mouse.emit(self.u.REL_X, dx, syn=False)
        if dy:
            self.mouse.emit(self.u.REL_Y, dy, syn=False)
        self.mouse.syn()

    def mouse_wheel(self, notches):
        self.mouse.emit(self.u.REL_WHEEL, notches)

    def mouse_button(self, name, down):
        self.mouse.emit(self._mouse_buttons[name], 1 if down else 0)

    def key(self, hid, down):
        name = KEYMAP[hid][0]
        self.kbd.emit(getattr(self.u, "KEY_" + name), 1 if down else 0)


class WindowsBackend(Backend):
    """Gamepad through vgamepad (needs the ViGEmBus driver); mouse and keyboard through SendInput."""

    def __init__(self, vg):
        import ctypes
        from ctypes import wintypes
        self.vg = vg
        self.pad = vg.VX360Gamepad()
        self.pad.reset()
        self.pad.update()
        self._last_buttons = 0
        print("Xbox 360 controller initialized")

        ULONG_PTR = ctypes.c_size_t

        class MOUSEINPUT(ctypes.Structure):
            _fields_ = [("dx", wintypes.LONG), ("dy", wintypes.LONG), ("mouseData", wintypes.DWORD),
                        ("dwFlags", wintypes.DWORD), ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

        class KEYBDINPUT(ctypes.Structure):
            _fields_ = [("wVk", wintypes.WORD), ("wScan", wintypes.WORD), ("dwFlags", wintypes.DWORD),
                        ("time", wintypes.DWORD), ("dwExtraInfo", ULONG_PTR)]

        class HARDWAREINPUT(ctypes.Structure):
            _fields_ = [("uMsg", wintypes.DWORD), ("wParamL", wintypes.WORD), ("wParamH", wintypes.WORD)]

        class _Union(ctypes.Union):
            _fields_ = [("mi", MOUSEINPUT), ("ki", KEYBDINPUT), ("hi", HARDWAREINPUT)]

        class INPUT(ctypes.Structure):
            _fields_ = [("type", wintypes.DWORD), ("u", _Union)]

        self._ct, self._INPUT, self._MI, self._KI = ctypes, INPUT, MOUSEINPUT, KEYBDINPUT
        self._user32 = ctypes.windll.user32
        # SendInput constants
        self.KEYEVENTF_EXTENDEDKEY, self.KEYEVENTF_KEYUP = 0x0001, 0x0002
        self.MOVE, self.WHEEL = 0x0001, 0x0800
        self.BUTTONS = {  # name: (down flag, up flag, mouseData)
            "left": (0x0002, 0x0004, 0), "right": (0x0008, 0x0010, 0),
            "back": (0x0080, 0x0100, 1), "forward": (0x0080, 0x0100, 2),   # XBUTTON1 / XBUTTON2
        }

    def _send(self, inp):
        self._user32.SendInput(1, self._ct.byref(inp), self._ct.sizeof(inp))

    def _mouse(self, flags, dx=0, dy=0, data=0):
        inp = self._INPUT(type=0)
        inp.u.mi = self._MI(dx, dy, data & 0xFFFFFFFF, flags, 0, 0)
        self._send(inp)

    def mouse_move(self, dx, dy):
        self._mouse(self.MOVE, dx, dy)

    def mouse_wheel(self, notches):
        self._mouse(self.WHEEL, data=notches * 120)

    def mouse_button(self, name, down):
        dflag, uflag, data = self.BUTTONS[name]
        self._mouse(dflag if down else uflag, data=data)

    def key(self, hid, down):
        _name, vk, ext = KEYMAP[hid]
        flags = (self.KEYEVENTF_EXTENDEDKEY if ext else 0) | (0 if down else self.KEYEVENTF_KEYUP)
        inp = self._INPUT(type=1)
        inp.u.ki = self._KI(vk, 0, flags, 0, 0)
        self._send(inp)

    def gamepad_reset(self):
        self.pad.reset()
        self.pad.update()
        self._last_buttons = 0

    def gamepad_update(self, pkt):
        vg, pad, buttons = self.vg, self.pad, pkt.buttons
        changes = buttons ^ self._last_buttons
        X = vg.XUSB_BUTTON
        for key, btn in [(KEY_A, X.XUSB_GAMEPAD_A), (KEY_B, X.XUSB_GAMEPAD_B),
                         (KEY_X, X.XUSB_GAMEPAD_X), (KEY_Y, X.XUSB_GAMEPAD_Y),
                         (KEY_L, X.XUSB_GAMEPAD_LEFT_SHOULDER), (KEY_R, X.XUSB_GAMEPAD_RIGHT_SHOULDER),
                         (KEY_START, X.XUSB_GAMEPAD_START), (KEY_SELECT, X.XUSB_GAMEPAD_BACK),
                         (KEY_DUP, X.XUSB_GAMEPAD_DPAD_UP), (KEY_DDOWN, X.XUSB_GAMEPAD_DPAD_DOWN),
                         (KEY_DLEFT, X.XUSB_GAMEPAD_DPAD_LEFT), (KEY_DRIGHT, X.XUSB_GAMEPAD_DPAD_RIGHT)]:
            if changes & key:
                pad.press_button(btn) if buttons & key else pad.release_button(btn)
        pad.left_trigger(255 if buttons & KEY_ZL else 0)
        pad.right_trigger(255 if buttons & KEY_ZR else 0)
        pad.left_joystick(map_axis_value(pkt.circle_x), map_axis_value(-pkt.circle_y))
        pad.right_joystick(map_axis_value(pkt.cstick_x), map_axis_value(-pkt.cstick_y))
        pad.update()
        self._last_buttons = buttons


# ---------------------------------------------------------------------------
# The bridge: decides what each packet means. No operating-system code here.
# ---------------------------------------------------------------------------
class Bridge:
    def __init__(self, backend, debug=False):
        self.b = backend
        self.debug = debug
        self.mode = None
        self.keys_down = set()       # HID codes currently pressed on the PC
        self.buttons_down = set()    # mouse buttons currently pressed
        self._move = [0.0, 0.0]      # cursor movement not yet sent (fractions of a pixel)
        self._scroll = 0.0           # scroll not yet sent (fractions of a notch)

    # -- changing state on the PC, always as press/release pairs --
    def _sync_keys(self, wanted):
        # releases: typed keys first, then modifiers; presses: modifiers first, then the key
        for hid in sorted(self.keys_down - wanted, key=lambda h: h >= FIRST_MODIFIER_HID):
            self.b.key(hid, False)
            self.keys_down.discard(hid)
        for hid in sorted(wanted - self.keys_down, key=lambda h: h < FIRST_MODIFIER_HID):
            if hid in KEYMAP:
                self.b.key(hid, True)
                self.keys_down.add(hid)

    def _sync_buttons(self, wanted):
        for name in sorted(self.buttons_down - wanted):
            self.b.mouse_button(name, False)
            self.buttons_down.discard(name)
        for name in sorted(wanted - self.buttons_down):
            self.b.mouse_button(name, True)
            self.buttons_down.add(name)

    def release_all(self):
        """Lets go of every key and mouse button (used on mode changes and on timeouts)."""
        self._sync_keys(set())
        self._sync_buttons(set())
        self._move = [0.0, 0.0]
        self._scroll = 0.0

    @property
    def anything_held(self):
        return bool(self.keys_down or self.buttons_down)

    # -- packets --
    def handle(self, pkt):
        if pkt.mode != self.mode:
            if self.debug:
                print("Mode: %s -> %s" % (self.mode, pkt.mode))
            self.release_all()
            if pkt.mode != MODE_CONTROLLER:
                self.b.gamepad_reset()        # no stuck gamepad input while using mouse/keyboard
            self.mode = pkt.mode

        if pkt.mode == MODE_CONTROLLER:
            self.b.gamepad_update(pkt)
        else:
            self._desktop(pkt)

    def _desktop(self, pkt):
        b = pkt.buttons

        # --- mouse buttons: the touchscreen tap plus the physical buttons ---
        wanted = set()
        if (pkt.mouse_buttons & MOUSE_LEFT) or (b & (KEY_A | KEY_L)):
            wanted.add("left")
        if b & (KEY_B | KEY_R):
            wanted.add("right")
        if b & (KEY_Y | KEY_ZL):
            wanted.add("back")
        if b & (KEY_X | KEY_ZR):
            wanted.add("forward")
        self._sync_buttons(wanted)

        # --- keyboard: modifiers + typed key from the touch keyboard, plus Enter and arrows ---
        keys = set()
        for bit in range(8):
            if pkt.modifiers & (1 << bit):
                keys.add(FIRST_MODIFIER_HID + bit)
        if pkt.key:
            keys.add(pkt.key)
        if b & KEY_START: keys.add(HID_ENTER)
        if b & KEY_DUP:   keys.add(HID_UP)
        if b & KEY_DDOWN: keys.add(HID_DOWN)
        if b & KEY_DLEFT: keys.add(HID_LEFT)
        if b & KEY_DRIGHT: keys.add(HID_RIGHT)
        self._sync_keys(keys)

        # --- cursor movement and scrolling ---
        move_x = float(pkt.mouse_dx)      # from the touch trackpad
        move_y = float(pkt.mouse_dy)
        scroll = 0.0

        sticks = [(pkt.circle_x, pkt.circle_y, bool(pkt.flags & FLAG_CIRCLE_SCROLL))]
        if pkt.flags & FLAG_NEW3DS:       # an old 3DS has no C-Stick
            sticks.append((pkt.cstick_x, pkt.cstick_y, bool(pkt.flags & FLAG_CSTICK_SCROLL)))
        for sx, sy, scrolls in sticks:
            ax, ay = stick_amount(sx), stick_amount(sy)
            if scrolls:
                scroll += ay * SCROLL_MAX_SPEED               # stick up = scroll up
            else:
                move_x += ax * CURSOR_MAX_SPEED
                move_y -= ay * CURSOR_MAX_SPEED               # stick up = cursor up (screen y goes down)

        self._move[0] += move_x
        self._move[1] += move_y
        ix, iy = int(self._move[0]), int(self._move[1])       # whole pixels only; keep the remainder
        self._move[0] -= ix
        self._move[1] -= iy
        if ix or iy:
            self.b.mouse_move(ix, iy)

        self._scroll += scroll
        notches = int(self._scroll)
        self._scroll -= notches
        if notches:
            self.b.mouse_wheel(notches)


# ---------------------------------------------------------------------------
# Startup
# ---------------------------------------------------------------------------
def create_backend():
    os_name = platform.system()
    print("Detected OS: %s" % os_name)

    if os_name == "Windows":
        try:
            import vgamepad as vg
        except ImportError:
            os.system("pip install vgamepad")
            try:
                import vgamepad as vg
            except ImportError:
                print("Failed to install vgamepad. Install it manually: pip install vgamepad")
                print("Make sure ViGEmBus driver is installed: https://github.com/ViGEm/ViGEmBus/releases")
                sys.exit(1)
        return WindowsBackend(vg)

    if os_name == "Linux":
        try:
            import uinput
        except ImportError:
            os.system("pip install python-uinput")
            try:
                import uinput
            except ImportError:
                print("Failed to install python-uinput. Install it manually: pip install python-uinput")
                print("You may also need: sudo apt-get install libudev-dev && sudo modprobe uinput")
                sys.exit(1)
        try:
            return LinuxBackend(uinput)
        except Exception as e:
            print("Failed to create the virtual devices: %s" % e)
            print("Make sure the uinput module is loaded (sudo modprobe uinput) and run with sudo.")
            sys.exit(1)

    print("Unsupported OS: %s" % os_name)
    sys.exit(1)


def main():
    debug = "--debug" in sys.argv
    running = [True]

    def stop(sig, frame):
        print("\nExiting...")
        running[0] = False

    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    backend = create_backend()
    bridge = Bridge(backend, debug)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", PORT))
    sock.settimeout(0.05)

    print("3DS Controller PC Receiver (v2.0.0)")
    print("Listening on port %d..." % PORT)
    print("Press Ctrl+C to exit")
    if debug:
        print("Debug mode enabled")

    devices = {}                 # ip -> last time we heard from it
    last_packet = time.time()

    try:
        while running[0]:
            now = time.time()
            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                data = None
            except ConnectionResetError:
                continue

            if data is not None:
                ip = addr[0]
                if ip not in devices:
                    print("[%s] New 3DS connected from %s" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), ip))
                devices[ip] = now

                if data == b"ping\x00":
                    sock.sendto(b"pong", addr)
                    continue

                pkt = parse_packet(data)
                if pkt is None:
                    if debug:
                        print("Ignoring a %d byte packet" % len(data))
                    continue
                last_packet = now
                if debug:
                    print(pkt)
                try:
                    bridge.handle(pkt)
                except Exception as e:
                    print("Error handling packet: %s" % e)

            # If the 3DS goes quiet, never leave keys or mouse buttons stuck down
            if bridge.anything_held and now - last_packet > STALE_AFTER:
                bridge.release_all()
                if debug:
                    print("No packets for a while: released everything")

            for ip in [ip for ip, seen in devices.items() if now - seen > 10]:
                print("[%s] 3DS at %s disconnected (timeout)" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), ip))
                del devices[ip]
    finally:
        print("Shutting down...")
        try:
            bridge.release_all()
            backend.gamepad_reset()
            backend.close()
        except Exception:
            pass
        sock.close()
        print("Socket closed. Exiting.")


if __name__ == "__main__":
    main()
