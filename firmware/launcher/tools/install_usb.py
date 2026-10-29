#!/usr/bin/env python3
"""Installs an app image into a launcher slot over the USB console.

install_usb.py <port> <slot> <image.bin> [slug] [--open]

Protocol (firmware/components/yaykit_install, the recovery's and Yay's `install`): `install <slot> <size> <sha256> [slug]`, wait for
YK_INSTALL_READY, then send 4 KB chunks, each answered with YK_ACK <bytes>; ends with
YK_INSTALL OK|<code>. The launcher keeps running; nothing else on the device is touched.
"""
import hashlib, json, sys, time
import serial


class Console:
    def __init__(self, port):
        self.port = port
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.2
        self.connect(time.time() + 15)

    def connect(self, deadline):
        while time.time() < deadline:
            try:
                self.s.open()
                # Release RTS, then DTR, so the chip is not reset or put into download mode.
                self.s.rts = False
                self.s.dtr = False
                return True
            except (serial.SerialException, OSError):
                time.sleep(0.2)
        return False

    def wait_for(self, prefixes, seconds):
        """Returns the first line starting with one of prefixes; reconnects across chip restarts."""
        end = time.time() + seconds
        while time.time() < end:
            try:
                line = self.s.readline().decode('utf-8', 'replace').strip()
            except (serial.SerialException, OSError):
                try:
                    self.s.close()
                except Exception:
                    pass
                self.connect(end)
                continue
            for p in prefixes:
                if p in line:
                    return line[line.index(p):]
        return None

    def send(self, text):
        self.s.reset_input_buffer()
        self.s.write(text.encode())

    def info(self):
        self.send('\r\nyk info\r\n')
        line = self.wait_for(['YK_INFO '], 4)
        return json.loads(line[line.index('{'):]) if line else None


def install(con, slot, data, slug='', sha256=None, stop_after=None):
    """Streams data into a slot. stop_after simulates a cable pulled after that many bytes."""
    digest = sha256 or hashlib.sha256(data).hexdigest()
    t0 = time.time()
    con.send(f'\r\ninstall {slot} {len(data)} {digest} {slug}\r\n')
    ready = con.wait_for(['YK_INSTALL_READY', 'YK_INSTALL '], 10)
    if not ready or not ready.startswith('YK_INSTALL_READY'):
        return {'result': ready, 'seconds': round(time.time() - t0, 2)}
    chunk = int(ready.split('chunk=')[1]) if 'chunk=' in ready else 4096
    sent = 0
    while sent < len(data):
        if stop_after is not None and sent >= stop_after:
            break
        part = data[sent:sent + chunk]
        con.s.write(part)
        sent += len(part)
        ack = con.wait_for(['YK_ACK ', 'YK_INSTALL '], 15)
        if not ack or not ack.startswith('YK_ACK ') or int(ack.split()[1]) != sent:
            return {'result': ack, 'sent': sent, 'seconds': round(time.time() - t0, 2)}
    result = con.wait_for(['YK_INSTALL '], 60)
    seconds = round(time.time() - t0, 2)
    return {'result': result, 'sent': sent, 'seconds': seconds, 'kbps': round(sent / seconds / 1024)}


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    if len(args) < 3:
        sys.exit(__doc__)
    con = Console(args[0])
    slot, data = int(args[1]), open(args[2], 'rb').read()
    outcome = install(con, slot, data, args[3] if len(args) > 3 else '')
    print(json.dumps({'slot': slot, 'bytes': len(data), **outcome}))
    if not (outcome['result'] or '').startswith('YK_INSTALL OK'):
        sys.exit(1)
    if '--open' in sys.argv:
        con.send(f'yk cmd launcher.switch {{"slot":{slot}}}\r\n')
        print(con.wait_for(['LAUNCHER_SWITCH', 'YK_ERR'], 5))
