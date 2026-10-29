#!/usr/bin/env python3
"""Drive a Yay device over its USB console while developing (docs/design/yay-device.md 12.5).

Only reads and writes the serial port: it never enters download mode, never resets the chip on purpose and never
erases anything. Opening the port may restart the device once; that changes no data.

  yay_dev.py [--port P] info | heap | logs [seconds] [prefix ...]
  yay_dev.py tap | double | hold <ms> | say <seconds> [text]       (development builds, CONFIG_YAY_DEV)
  yay_dev.py state <mode> | action '<json>' | inbox '<json>' | server <url>   (main firmware, development builds)
  yay_dev.py launch <slug> ['<json>'] | switch <slot 1-3> | exit | recovery
  yay_dev.py install --slot <1-3> <build dir or .bin> [--slug s] [--open]
  yay_dev.py snap <out.png>
  yay_dev.py motion-log <seconds> <out.csv>             (firmware with yaykit_motion's console: `motion log`, saved
                                                          as CSV for yaykit_motion/test/motion)
  yay_dev.py rec <ms> <out.wav> [raw|plain|nsonly]       (main firmware: the audio front end's output, or raw TDM)
  yay_dev.py clip <16 kHz mono wav> | firstword [trials] [full|plain|nsonly] [last.wav]
                                                         (main firmware, development builds: the first-word
                                                          test plays the clip through the speaker)
  yay_dev.py bridge [host:port] [seconds]                (main firmware, development builds, `yay server usb`:
                                                          the session over USB to mock-voice-server.ts)
  yay_dev.py run '<console line>' [seconds]
  yay_dev.py wifi-join [ssid]                            (a fallback to the App's setup: lists the networks when no
                                                          SSID is given, asks for the password without showing it)
  yay_dev.py wifi-forget

`install` goes through the recovery (factory), which has the USB install protocol: it asks the running app for
the recovery, streams the image into the slot, then restarts (the recovery hands over to Yay) or, with --open,
boots the slot. Slots are numbered 1-3 as on the device (slot 1 is Yay).
"""
import base64, glob, json, os, struct, sys, time, zlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'launcher', 'tools'))
from install_usb import Console, install  # noqa: E402

PREFIXES = ('YAYKIT_', 'YAY_', 'YK_', 'TEMPLATE_', 'PARTY_', 'HB_', 'BREATHE_', 'TR_', 'HEAP', 'SNAP_ERROR')


def default_port():
    # The Mac sometimes re-enumerates the USB link for a second or two after the previous command closed it.
    deadline = time.time() + 5
    while True:
        ports = sorted(glob.glob('/dev/cu.usbmodem*')) or sorted(glob.glob('/dev/ttyACM*'))
        if ports:
            return ports[0]
        if time.time() > deadline:
            sys.exit('no USB device found; pass --port')
        time.sleep(0.2)


def lines(con, seconds, prefixes=PREFIXES):
    """Yields (seconds since start, line) for lines that contain one of the prefixes."""
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            raw = con.s.readline()
        except Exception:
            try:
                con.s.close()
            except Exception:
                pass
            con.connect(time.time() + max(1, seconds - (time.time() - t0)))
            continue
        text = raw.decode('utf-8', 'replace').strip()
        if text and (not prefixes or any(p in text for p in prefixes)):
            yield time.time() - t0, text


def send(con, text):
    con.s.write(('\r\n' + text + '\r\n').encode())


def run(con, text, seconds=3, prefixes=PREFIXES):
    send(con, text)
    for t, line in lines(con, seconds, prefixes):
        print(f'{t * 1000:7.0f} {line}', flush=True)


def wait_for(con, marker, seconds):
    for _, line in lines(con, seconds, None):
        if marker in line:
            return line
    return None


def cmd_wifi_join(con, ssid=None):
    """Joins a Wi-Fi network. The password is typed here without echo (getpass) and goes to the device only as
    `yk cmd wifi.join -`'s argument line, which the device reads itself: never echoed, never in its console history,
    never logged. Nothing here prints or stores it, and it is not on the command line (shell history). Run it in
    your own terminal, with nothing else holding the serial port (no bridge)."""
    import getpass
    if not ssid:
        run(con, 'wifi scan', 8, ('YK_WIFI_AP', 'YK_WIFI_SCAN'))
        ssid = input('Wi-Fi name (SSID): ').strip()
    if not ssid:
        sys.exit('no SSID')
    password = getpass.getpass(f'Password for {ssid} (not shown; just Enter for an open network): ')
    args = {'ssid': ssid}
    if password:
        args['password'] = password
    payload = (json.dumps(args, ensure_ascii=False, separators=(',', ':')) + '\n').encode()
    del password, args
    con.s.reset_input_buffer()
    send(con, 'yk cmd wifi.join -')
    if not wait_for(con, 'YK_ARGS', 5):
        sys.exit('the device did not ask for the network (a firmware without the Yaykit Link console?)')
    con.s.write(payload)
    del payload
    reply = None
    for _, line in lines(con, 20, ('YK_OK', 'YK_ERR')):
        reply = line
        break
    print(reply or 'no answer from the device', flush=True)
    if not reply or not reply.startswith('YK_OK'):
        sys.exit(1)
    # Follow the connection for a while, then the status (state, SSID, signal, address).
    for t, line in lines(con, 15, ('YK_WIFI', 'YAY_NET', 'wifi')):
        print(f'{t * 1000:7.0f} {line}', flush=True)
    run(con, 'wifi', 2, ('YK_WIFI',))


def image_of(path):
    """(bytes, slug) from a build directory (project_description.json) or a .bin."""
    if os.path.isdir(path):
        desc = json.load(open(os.path.join(path, 'project_description.json')))
        data = open(os.path.join(path, desc['app_bin']), 'rb').read()
        name = desc['project_name']
    else:
        data = open(path, 'rb').read()
        name = data[0x50:0x70].split(b'\0')[0].decode()  # esp_app_desc_t (at 0x20) .project_name (at +0x30)
    slug = name[7:] if name.startswith('yaykit_') else name
    return data, slug.replace('_', '-')


def cmd_install(con, args):
    slot = int(args[args.index('--slot') + 1])
    rest = [a for i, a in enumerate(args) if a not in ('--slot', '--open', '--slug') and args[i - 1] not in ('--slot', '--slug')]
    data, slug = image_of(rest[0])
    if '--slug' in args:
        slug = args[args.index('--slug') + 1]
    info = con.info()
    if not info or info['app']['slug'] != 'launcher':
        send(con, 'app recovery')
        if not wait_for(con, 'YAYKIT_LAUNCHER_READY', 20):
            sys.exit('the recovery did not start')
        time.sleep(0.5)
    print(f'installing {slug} ({len(data)} bytes) into slot {slot}', flush=True)
    result = install(con, slot - 1, data, slug)
    print(json.dumps(result), flush=True)
    if not str(result.get('result', '')).startswith('YK_INSTALL OK'):
        sys.exit(1)
    run(con, f'yk cmd launcher.switch {{"slot":{slot - 1}}}' if '--open' in args else 'reboot', 8)


def snap_once(con):
    """One `snap`: (w, h, stride, RGB565 bytes), or None when a line was damaged or missing."""
    con.s.reset_input_buffer()
    send(con, 'snap')
    w = h = stride = 0
    data = bytearray()
    seq = bad = 0
    deadline = time.time() + 60
    while time.time() < deadline:
        line = con.s.readline().decode('ascii', 'replace').strip()
        if line.startswith('SNAP_BEGIN'):
            _, w, h, stride, _ = line.split()
            w, h, stride = int(w), int(h), int(stride)
        elif line == 'SNAP_END':
            break
        elif line.startswith('SNAP_ERROR'):
            sys.exit(line)
        elif w and line.startswith('SN '):
            # "SN <seq> <base64>"; anything else is another task's log line printed in between
            parts = line.split(' ')
            try:
                if len(parts) != 3 or int(parts[1]) != seq:
                    raise ValueError(line)
                data += base64.b64decode(parts[2], validate=True)
                seq += 1
            except Exception:
                bad += 1
        elif w and line and not seq:
            try:
                data += base64.b64decode(line, validate=True)  # firmware from before numbered lines
            except Exception:
                pass
    if not w or bad or len(data) < h * stride:
        print(f'snap: {bad} damaged lines, {len(data)} of {h * stride} bytes', file=sys.stderr)
        return None
    return w, h, stride, data


def cmd_motion_log(con, seconds, out):
    """`motion log <seconds>`: the numbered lines "MC <seq> <t_us,ax,ay,az,gx,gy,gz>" into a CSV with its header."""
    con.s.reset_input_buffer()
    send(con, f'motion log {seconds}')
    rows, seq, missing, begun = [], 0, 0, False
    deadline = time.time() + seconds + 15
    while time.time() < deadline:
        line = con.s.readline().decode('ascii', 'replace').strip()
        if 'MOTION_CSV_BEGIN' in line:
            begun = True
        elif begun and 'MOTION_CSV_END' in line:
            break
        elif begun and line.startswith('MC '):
            parts = line.split(' ')
            if len(parts) == 3 and parts[1].isdigit():
                missing += max(0, int(parts[1]) - seq)
                seq = int(parts[1]) + 1
                rows.append(parts[2])
            else:
                missing += 1  # another task's line broke this one
        elif 'MOTION not running' in line:
            sys.exit(line)
    with open(out, 'w') as f:
        f.write('t_us,ax,ay,az,gx,gy,gz\n' + ''.join(r + '\n' for r in rows))
    print(f'motion-log: {len(rows)} samples, {missing} missing -> {out}', file=sys.stderr)


def cmd_snap(con, out):
    # A log line from another task can break one of the numbered lines: then the picture is taken again.
    for attempt in range(3):
        got = snap_once(con)
        if got:
            break
        print(f'snap: damaged, taking it again ({attempt + 1})', file=sys.stderr)
    else:
        sys.exit('snap: three damaged pictures in a row (something on the device is printing a lot)')
    w, h, stride, data = got
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            v = data[y * stride + x * 2] | data[y * stride + x * 2 + 1] << 8
            row += bytes(((v >> 11 & 31) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
        rows.append(bytes(row))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body) & 0xFFFFFFFF)

    png = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
           chunk(b'IDAT', zlib.compress(b''.join(rows), 9)) + chunk(b'IEND', b''))
    open(out, 'wb').write(png)
    print(out, w, h)


def read_wav(con, seconds, on_line=None):
    """Collects a WAV printed by yk_audio_print_wav(): numbered "AW <seq> <base64>" lines between AUDIO_WAV_BEGIN
    and AUDIO_WAV_END. Other lines go to on_line. Returns (bytes, damaged line count)."""
    data = bytearray()
    started = False
    seq = bad = 0
    deadline = time.time() + seconds
    while time.time() < deadline:
        line = con.s.readline().decode('ascii', 'replace').strip()
        if line.startswith('AUDIO_WAV_BEGIN'):
            started = True
        elif line == 'AUDIO_WAV_END':
            break
        elif started and line.startswith('AW '):
            # anything else is another task's log line printed in between
            parts = line.split(' ')
            try:
                if len(parts) != 3 or int(parts[1]) != seq:
                    raise ValueError(line)
                data += base64.b64decode(parts[2], validate=True)
                seq += 1
            except Exception:
                bad += 1
        elif line and on_line:
            on_line(line)
    return data, bad


def save_wav(out, data, bad):
    open(out, 'wb').write(data)
    print(out, len(data), 'bytes', f'({bad} damaged lines; the recording is unusable)' if bad else '')


def cmd_rec(con, ms, out, how=''):
    """Records with the main firmware's `audio rec` and saves the WAV it prints; prints its AUDIO_STAT line."""
    con.s.reset_input_buffer()
    send(con, f'audio rec {ms} {how}'.strip())
    data, bad = read_wav(con, int(ms) / 1000 + 60,
                         lambda line: print(line, flush=True) if line.startswith('AUDIO_STAT') else None)
    save_wav(out, data, bad)


def cmd_clip(con, path):
    """Loads a 16 kHz mono 16-bit WAV into the main firmware's PSRAM for `yay firstword` (development builds)."""
    import wave
    w = wave.open(path)
    if (w.getframerate(), w.getnchannels(), w.getsampwidth()) != (16000, 1, 2):
        sys.exit('the clip must be 16 kHz mono 16-bit (afconvert -f WAVE -d LEI16@16000 -c 1 in out.wav)')
    pcm = w.readframes(w.getnframes())
    con.s.reset_input_buffer()
    send(con, f'yay clip begin {len(pcm) // 2}')
    if not wait_for(con, 'YAY_CLIP begin', 5):
        sys.exit('no answer to `yay clip` (a development build of the main firmware?)')
    for at in range(0, len(pcm), 384):
        piece = base64.b64encode(pcm[at:at + 384]).decode()
        send(con, f'yay clip {at} {piece}')
        if not wait_for(con, f'YAY_CLIP at={at + len(pcm[at:at + 384])}', 5):
            sys.exit(f'clip upload stopped at byte {at}')
    send(con, 'yay clip end')
    print(wait_for(con, 'YAY_CLIP done', 5))


def cmd_firstword(con, trials, how='full', out=None):
    """Runs `yay firstword` and prints its lines; with out, saves the last trial's recording."""
    con.s.reset_input_buffer()
    send(con, f'yay firstword {trials} {how}' + (' dump' if out else ''))
    deadline = time.time() + int(trials) * 6 + 30
    summary = None
    while time.time() < deadline and not summary:
        line = con.s.readline().decode('utf-8', 'replace').strip()
        if line.startswith('YAY_FIRSTWORD'):
            print(line, flush=True)
            summary = line if line.startswith('YAY_FIRSTWORD_SUMMARY') else None
    if out and summary:
        save_wav(out, *read_wav(con, 60))


def cmd_bridge(con, target, seconds):
    """The development USB bridge (yaykit_session, `yay server usb`): the device's session frames ("YS> t|b <b64>"
    lines) go to the mock voice server's TCP port as "t|b <b64>" lines, and its lines come back as `ys t|b <b64>`.
    Also prints the device's YAY_ lines, timed, so it doubles as the log."""
    import socket, threading
    host, _, port = target.rpartition(':')
    sock = socket.create_connection((host or '127.0.0.1', int(port or 8788)))
    t0 = time.time()
    stop = threading.Event()
    lock = threading.Lock()

    def to_device(text):
        with lock:
            con.s.write((text + '\r\n').encode())

    def from_server():
        buf = b''
        while not stop.is_set():
            try:
                data = sock.recv(65536)
            except OSError:
                break
            if not data:
                break
            buf += data
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                parts = line.decode().split(' ')
                if len(parts) == 2 and parts[0] in ('t', 'b'):
                    to_device(f'ys {parts[0]} {parts[1]}')
        stop.set()

    def from_stdin():
        # Console lines typed or piped in go to the device too: `(sleep 5; echo "yay say 3 你好") | yay_dev.py bridge`
        for line in sys.stdin:
            if line.strip():
                to_device(line.strip())

    con.s.reset_input_buffer()
    to_device('ys c')
    threading.Thread(target=from_server, daemon=True).start()
    if not sys.stdin.isatty():
        threading.Thread(target=from_stdin, daemon=True).start()
    deadline = t0 + float(seconds)
    try:
        while time.time() < deadline and not stop.is_set():
            raw = con.s.readline()
            if not raw:
                continue
            text = raw.decode('utf-8', 'replace').strip()
            at = text.find('YS> ')
            if at >= 0:
                kind, _, b64 = text[at + 4:].partition(' ')
                if kind in ('t', 'b') and b64:
                    sock.sendall(f'{kind} {b64}\n'.encode())
            elif any(p in text for p in PREFIXES) and not text.startswith('ys '):
                print(f'{(time.time() - t0) * 1000:8.0f} {text}', flush=True)
    finally:
        stop.set()
        try:
            to_device('ys d')
        except Exception:
            pass
        sock.close()


def main():
    args = sys.argv[1:]
    port = default_port()
    if args[:1] == ['--port']:
        port, args = args[1], args[2:]
    if not args:
        sys.exit(__doc__)
    con = Console(port)
    time.sleep(0.3)
    cmd, rest = args[0], args[1:]
    if cmd == 'info':
        print(json.dumps(con.info(), ensure_ascii=False, indent=1))
    elif cmd == 'heap':
        run(con, 'heap', 2, ('HEAP',))
    elif cmd == 'logs':
        seconds = float(rest[0]) if rest else 30
        for t, line in lines(con, seconds, tuple(rest[1:]) or PREFIXES):
            print(f'{t * 1000:7.0f} {line}', flush=True)
    elif cmd in ('tap', 'double'):
        run(con, f'yay {cmd}', 2)
    elif cmd == 'hold':
        run(con, f'yay hold {rest[0]}', int(rest[0]) / 1000 + 2)
    elif cmd == 'say':
        run(con, 'yay say ' + ' '.join(rest), float(rest[0]) + 8)
    elif cmd in ('state', 'server'):
        run(con, f'yay {cmd} {rest[0]}', 3)
    elif cmd in ('action', 'inbox'):
        run(con, f"yay {cmd} {rest[0]}", 5)
    elif cmd == 'launch':
        run(con, 'yay launch ' + ' '.join(rest), 8)
    elif cmd == 'switch':  # slots 1-3 here; launcher.switch has always taken 0-2
        run(con, f'yk cmd launcher.switch {{"slot":{int(rest[0]) - 1}}}', 8)
    elif cmd == 'exit':
        run(con, 'yk cmd app.exit', 8)
    elif cmd == 'recovery':
        run(con, 'app recovery', 8)
    elif cmd == 'install':
        cmd_install(con, rest)
    elif cmd == 'snap':
        cmd_snap(con, rest[0])
    elif cmd == 'motion-log':
        cmd_motion_log(con, int(rest[0]), rest[1])
    elif cmd == 'rec':
        cmd_rec(con, rest[0], rest[1], rest[2] if len(rest) > 2 else '')
    elif cmd == 'bridge':
        cmd_bridge(con, rest[0] if rest else '127.0.0.1:8788', rest[1] if len(rest) > 1 else 3600)
    elif cmd == 'clip':
        cmd_clip(con, rest[0])
    elif cmd == 'firstword':
        cmd_firstword(con, rest[0] if rest else '20', rest[1] if len(rest) > 1 else 'full',
                      rest[2] if len(rest) > 2 else None)
    elif cmd == 'wifi-join':
        cmd_wifi_join(con, ' '.join(rest) if rest else None)
    elif cmd == 'wifi-forget':
        run(con, 'wifi forget', 3, ('YK_WIFI',))
    elif cmd == 'run':
        run(con, rest[0], float(rest[1]) if len(rest) > 1 else 3, None)
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
