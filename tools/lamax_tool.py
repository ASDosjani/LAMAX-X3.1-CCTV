"""LAMAX X3.1 camera tool (GUI for Windows, Linux and macOS).

Per camera, over USB/adb:
  1. Back up the whole NOR flash (all 8 MTD partitions, each read twice and compared)
  2. Install the SD-card boot hook (built from THIS camera's own backup), verified by read-back
  3. Restore the original boot partition from the backup
  4. Write the SD card files (programs + config.txt) to the camera or to a drive / folder
The WiFi name can be picked from the networks the camera itself sees (once our software runs on it).
Run from source:  python tools/lamax_tool.py   (Linux: python3, needs python3-tk and adb)
Build: tools/build_exe.ps1 (Windows exe), tools/build_linux.sh (Linux binary),
       tools/build_macos.sh (macOS app)
"""
import datetime, hashlib, io, os, queue, re, shutil, subprocess, sys, tempfile, threading, time, zipfile
import urllib.error, urllib.request
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP = 'LAMAX X3.1 camera tool'
# resource dir (PyInstaller unpacks data here) and the folder next to the exe / repo root
RES = getattr(sys, '_MEIPASS', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOME = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else RES
if getattr(sys, 'frozen', False) and sys.platform == 'darwin':
    # A macOS app runs from inside LamaxTool.app (read-only, possibly a randomised copy when
    # started from Downloads), so backups, adb and the settings go to ~/LamaxTool instead.
    HOME = os.path.join(os.path.expanduser('~'), 'LamaxTool')
    os.makedirs(HOME, exist_ok=True)
sys.path.insert(0, os.path.join(RES, 'boot'))
import make_boot  # noqa: E402

SD_SRC = os.path.join(RES, 'sdcard', 'cam')
BACKUPS = os.path.join(HOME, 'backups')
# reference dump of an untouched camera (repo folder; its boot image + SHA256SUMS are
# also bundled into the program), used when a camera has no backup of its own
ORIGINAL_FW_DIRS = [os.path.join(HOME, 'ORIGINAL_FW'), os.path.join(RES, 'ORIGINAL_FW')]
PART_SIZE = make_boot.PART_SIZE
EXPECTED_MTD = [('uboot', 0x40000), ('boot', 0x280000), ('system', 0x480000), ('cfg', 0x50000),
                ('boot_logo', 0x10000), ('shutdown_logo', 0x10000), ('env', 0x10000), ('private', 0x10000)]
WINDOWS = os.name == 'nt'
PLATFORM_TOOLS_URL = 'https://dl.google.com/android/repository/platform-tools-latest-%s.zip' % (
    'windows' if WINDOWS else 'darwin' if sys.platform == 'darwin' else 'linux')
ADB_NAME = 'adb.exe' if WINDOWS else 'adb'
NOWIN = 0x08000000 if WINDOWS else 0   # CREATE_NO_WINDOW: no console flashes on Windows
UDEV_HINT = ('On Linux, allow USB access to the camera (Allwinner, vendor 1f3a):\n'
             '  echo \'SUBSYSTEM=="usb", ATTR{idVendor}=="1f3a", MODE="0666"\' | '
             'sudo tee /etc/udev/rules.d/51-lamax.rules\n'
             '  sudo udevadm control --reload-rules\n'
             'then re-plug the camera.')


# ----------------------------------------------------------------------------- adb

def find_adb():
    cands = [os.path.join(HOME, 'platform-tools', ADB_NAME),
             os.path.join(os.path.expanduser('~'), 'Downloads', 'platform-tools', ADB_NAME),
             os.path.join(os.path.expanduser('~'), 'platform-tools', ADB_NAME)]
    p = shutil.which('adb')
    if p:
        cands.insert(0, p)
    return next((c for c in cands if c and os.path.isfile(c)), None)


class Adb:
    def __init__(self, exe, serial=None):
        self.exe, self.serial = exe, serial

    def run(self, *args, timeout=120, check=True):
        cmd = [self.exe] + (['-s', self.serial] if self.serial else []) + list(args)
        r = subprocess.run(cmd, stdin=subprocess.DEVNULL, capture_output=True, timeout=timeout, creationflags=NOWIN)
        out = (r.stdout + r.stderr).decode('utf-8', 'replace')
        if check and r.returncode != 0:
            raise RuntimeError('adb %s failed: %s' % (' '.join(args), out.strip()))
        return out

    def sh(self, script, timeout=120):
        return self.run('shell', script, timeout=timeout).replace('\r\n', '\n')

    def devices(self):
        out = self.run('devices')
        if 'no permissions' in out:
            raise RuntimeError('adb has no permission to open the camera.\n' + UDEV_HINT)
        return [l.split('\t')[0] for l in out.splitlines()[1:] if l.endswith('\tdevice')]

    def push(self, local, remote, timeout=300):
        self.run('push', local, remote, timeout=timeout)

    def pull(self, remote, local, timeout=300):
        self.run('pull', remote, local, timeout=timeout)


class CamSsh:
    """The camera over WiFi through its SSH server (Dropbear, user root, the admin
    password): the same interface as Adb (sh / push / pull / run), so every USB function
    works over WiFi too. Files go through "cat" on an exec channel (Dropbear has no SFTP).
    The camera's host key is remembered on first use; a changed key is refused."""
    KNOWN_HOSTS = os.path.join(HOME, 'LamaxTool_known_hosts')

    def __init__(self, host, passwords, port=22):
        try:
            import paramiko
        except ImportError as e:
            raise RuntimeError('No SSH support in this build (%s).' % e)
        self.host, self.serial = host, 'wifi-' + host
        c = paramiko.SSHClient()
        if os.path.isfile(self.KNOWN_HOSTS):
            c.load_host_keys(self.KNOWN_HOSTS)

        class Remember(paramiko.MissingHostKeyPolicy):          # trust on first use, then pinned
            def missing_host_key(self_, client, hostname, key):
                client.get_host_keys().add(hostname, key.get_name(), key)
                try:
                    client.save_host_keys(CamSsh.KNOWN_HOSTS)
                except OSError:
                    pass
        c.set_missing_host_key_policy(Remember())
        last = None
        for pw in [p for i, p in enumerate(passwords) if p and p not in passwords[:i]]:
            try:
                c.connect(host, port, 'root', pw, look_for_keys=False, allow_agent=False,
                          timeout=10, banner_timeout=10, auth_timeout=15)
                self.client, self.password = c, pw
                return
            except paramiko.AuthenticationException as e:
                last = e
            except paramiko.BadHostKeyException:
                raise RuntimeError('The SSH key of %s is not the one seen before. If the camera got a new SD card '
                                   'this is expected: delete its line from %s. Otherwise someone may be '
                                   'impersonating the camera.' % (host, self.KNOWN_HOSTS))
            except (OSError, paramiko.SSHException) as e:
                raise RuntimeError('No SSH connection to %s (%s). The camera needs the current programs '
                                   '(with SSH) and an admin password.' % (host, e))
        raise RuntimeError('SSH: wrong admin password for %s (%s).' % (host, last))

    def _exec(self, cmd, timeout, data=None, out_file=None):
        ch = self.client.get_transport().open_session()
        ch.settimeout(timeout)
        ch.exec_command(cmd)
        if data is not None:
            for i in range(0, len(data), 32768):
                ch.sendall(data[i:i + 32768])
            ch.shutdown_write()
        out = []
        while True:
            d = ch.recv(65536)
            if not d:
                break
            if out_file:
                out_file.write(d)
            else:
                out.append(d)
        err = b''
        while ch.recv_stderr_ready():
            err += ch.recv_stderr(65536)
        code = ch.recv_exit_status()
        ch.close()
        return code, b''.join(out), err

    def sh(self, script, timeout=120):
        code, out, err = self._exec(script, timeout)
        return (out + err).decode('utf-8', 'replace').replace('\r\n', '\n')

    def push(self, local, remote, timeout=300):
        code, _, err = self._exec("cat > '%s'" % remote, timeout, data=open(local, 'rb').read())
        if code != 0:
            raise RuntimeError('ssh push %s failed: %s' % (remote, err.decode('utf-8', 'replace').strip()))

    def pull(self, remote, local, timeout=300):
        with open(local, 'wb') as f:
            code, _, err = self._exec("cat '%s'" % remote, timeout, out_file=f)
        if code != 0:
            raise RuntimeError('ssh pull %s failed: %s' % (remote, err.decode('utf-8', 'replace').strip()))

    def run(self, *args, timeout=120, check=True):
        """adb-style commands used by the tool: shell / reboot."""
        if args and args[0] == 'shell':
            return self.sh(' '.join(args[1:]), timeout)
        if args and args[0] == 'reboot':           # detached: the connection drops with the reboot
            return self.sh("(trap '' HUP; sleep 1; sync; reboot) >/dev/null 2>&1 &", 10)
        raise RuntimeError('not available over SSH: %s' % ' '.join(args))

    def close(self):
        try:
            self.client.close()
        except Exception:
            pass


# ----------------------------------------------------------------------------- encrypted envelope
# Same format as src/seal.c: the config.txt contents travel encrypted over WiFi.
#   master = PBKDF2-HMAC-SHA256(admin password, "lamax-x31-camcfg-v1", 20000), per message a random
#   16-byte salt -> ChaCha20 key and HMAC key; "LXE1" | salt | time (camera clock) | ciphertext | tag

def _chacha20_xor(key, nonce, counter, data):
    import struct
    def rotl(v, n):
        return ((v << n) | (v >> (32 - n))) & 0xffffffff
    const = (0x61707865, 0x3320646e, 0x79622d32, 0x6b206574)
    k = struct.unpack('<8I', key)
    nn = struct.unpack('<3I', nonce)
    out = bytearray(data)
    for blk in range(0, len(out), 64):
        st = list(const) + list(k) + [(counter + blk // 64) & 0xffffffff] + list(nn)
        x = st[:]
        for _ in range(10):
            for a, b, c, d in ((0, 4, 8, 12), (1, 5, 9, 13), (2, 6, 10, 14), (3, 7, 11, 15),
                               (0, 5, 10, 15), (1, 6, 11, 12), (2, 7, 8, 13), (3, 4, 9, 14)):
                x[a] = (x[a] + x[b]) & 0xffffffff; x[d] = rotl(x[d] ^ x[a], 16)
                x[c] = (x[c] + x[d]) & 0xffffffff; x[b] = rotl(x[b] ^ x[c], 12)
                x[a] = (x[a] + x[b]) & 0xffffffff; x[d] = rotl(x[d] ^ x[a], 8)
                x[c] = (x[c] + x[d]) & 0xffffffff; x[b] = rotl(x[b] ^ x[c], 7)
        ks = struct.pack('<16I', *((x[i] + st[i]) & 0xffffffff for i in range(16)))
        for i in range(min(64, len(out) - blk)):
            out[blk + i] ^= ks[i]
    return bytes(out)


def seal_master(password):
    return hashlib.pbkdf2_hmac('sha256', password.encode('utf-8'), b'lamax-x31-camcfg-v1', 20000, 32)


def _subkeys(master, salt):
    import hmac
    return (hmac.new(master, b'enc' + salt, hashlib.sha256).digest(),
            hmac.new(master, b'mac' + salt, hashlib.sha256).digest())


def seal(master, plain, now):
    import hmac, struct
    salt = os.urandom(16)
    enc, mac = _subkeys(master, salt)
    head = b'LXE1' + salt + struct.pack('>Q', int(now))
    body = head + _chacha20_xor(enc, bytes(12), 1, plain)
    return body + hmac.new(mac, body, hashlib.sha256).digest()


def seal_open(master, data, now):
    """plaintext, or RuntimeError (wrong password / tampered / too old)"""
    import hmac, struct
    if len(data) < 60 or data[:4] != b'LXE1':
        raise RuntimeError('The camera did not send encrypted data - update the camera programs '
                           '(write the SD card over USB once).')
    enc, mac = _subkeys(master, data[4:20])
    if not hmac.compare_digest(hmac.new(mac, data[:-32], hashlib.sha256).digest(), data[-32:]):
        raise RuntimeError('The encrypted answer of the camera does not check out (wrong admin password, '
                           'or it was altered on the way).')
    t = struct.unpack('>Q', data[20:28])[0]
    if abs(t - now) > 300:
        raise RuntimeError('The encrypted answer of the camera is too old (replayed?).')
    return _chacha20_xor(enc, bytes(12), 1, data[28:-32])


class Talker:
    """Intercom: the laptop microphone -> the camera's speaker. A one-time token from
    camcfg (GET /api/talk, sealed), then a raw connection carrying 16 kHz mono s16le
    in 20 ms chunks, ChaCha20-encrypted with HMAC(master, "talk" + token)."""
    RATE, BLOCK = 16000, 320            # 20 ms; one chunk = 640 bytes = 10 ChaCha20 blocks

    def __init__(self, net, on_level=None, gain_db=None):
        """gain_db: None = automatic, else a fixed microphone gain in dB."""
        self.net, self.on_level = net, on_level
        self.fixed = None if gain_db is None else 10 ** (gain_db / 20.0)
        self.running = False
        self.sock = self.stream = None

    def start(self):
        import hmac, queue as _q, socket
        try:
            import sounddevice as sd
        except Exception as e:          # ImportError, or OSError when PortAudio is missing (Linux)
            raise RuntimeError('No microphone support in this build (%s). On Linux install '
                               'libportaudio2 (sudo apt install libportaudio2).' % e)
        raw = self.net.request('GET', '/api/talk')
        if raw.startswith(b'someone'):
            raise RuntimeError('Someone else is talking to this camera.')
        token = seal_open(self.net._master(), raw, self.net._camera_now())
        key = hmac.new(self.net._master(), b'talk' + token, hashlib.sha256).digest()
        s = socket.create_connection((self.net.host, self.net.port), timeout=5)
        s.sendall(b'TALK /api/talk HTTP/1.1\r\nX-Talk-Token: ' + token.hex().encode() + b'\r\n\r\n')
        head = b''
        while b'\r\n\r\n' not in head:
            d = s.recv(1024)
            if not d:
                break
            head += d
        if not head.startswith(b'HTTP/1.1 200'):
            s.close()
            raise RuntimeError('The camera refused the talk connection: %s' % head.split(b'\r\n')[0].decode('latin-1'))
        s.settimeout(None)
        self.sock, self.key, self.q = s, key, _q.Queue(maxsize=50)
        self.running = True

        def callback(indata, frames, t, status):
            try:
                self.q.put_nowait(bytes(indata))
            except _q.Full:              # network stalled: drop rather than build up delay
                pass
        self.stream = sd.RawInputStream(samplerate=self.RATE, channels=1, dtype='int16',
                                        blocksize=self.BLOCK, callback=callback)
        self.stream.start()
        threading.Thread(target=self._send, daemon=True).start()

    # automatic gain: laptop microphones are often quiet (-35 dBFS speech). Aim at about
    # -13 dBFS RMS, up to +30 dB; quick to turn down, slow to turn up, and it does not turn
    # up during silence (no hiss pumping). A tanh limiter keeps peaks from clipping.
    # Fixed gain (gain_db): that gain, recovering to it after a howl cut.
    # Howl guard: if the camera can be heard by the laptop microphone the loop starts to
    # howl; speech has pauses, a howl does not - more than 1.2 s of uninterrupted loud
    # output halves the gain (again and again while it lasts).
    AGC_TARGET, AGC_MAX, AGC_SILENCE = 0.22, 31.6, 0.003
    HOWL_LOUD, HOWL_CHUNKS = 0.08, 60

    def _agc(self, data):
        import array, math
        a = array.array('h', data)
        rms = math.sqrt(sum(v * v for v in a) / len(a)) / 32768.0
        if self.fixed is not None:
            self.gain = min(self.fixed, self.gain * 1.01)              # back to the set gain in ~1-2 s
        elif rms > self.AGC_SILENCE:
            want = min(self.AGC_MAX, max(1.0, self.AGC_TARGET / rms))
            self.gain += (want - self.gain) * (0.5 if want < self.gain else 0.03)
        self.loud = self.loud + 1 if rms * self.gain > self.HOWL_LOUD else 0
        if self.loud > self.HOWL_CHUNKS:
            self.gain = max(0.25, self.gain * 0.5)
            self.loud = 0
            self.howls += 1
        g = self.gain
        out = array.array('h', (int(32000 * math.tanh(v * g / 32000.0)) for v in a))
        return out.tobytes(), rms

    def _send(self):
        chunk = self.BLOCK * 2
        pending, counter, n = b'', 1, 0
        self.gain, self.loud, self.howls = (self.fixed if self.fixed is not None else 4.0), 0, 0
        try:
            while self.running:
                try:
                    pending += self.q.get(timeout=0.5)
                except Exception:
                    continue
                while len(pending) >= chunk:
                    data, pending = pending[:chunk], pending[chunk:]
                    data, rms = self._agc(data)
                    self.sock.sendall(_chacha20_xor(self.key, bytes(12), counter, data))
                    counter += chunk // 64
                    n += 1
                    if self.on_level and n % 12 == 0:            # ~4 times a second
                        self.on_level(min(1.0, rms * self.gain))
        except OSError:
            pass
        finally:
            self.running = False

    def stop(self):
        self.running = False
        for f in (lambda: self.stream.stop(), lambda: self.stream.close(),
                  lambda: self.sock.shutdown(2), lambda: self.sock.close()):
            try:
                f()
            except Exception:
                pass


class CamNet:
    """The camera over WiFi: camcfg's HTTP API (Digest login, user "admin",
    password ADMIN_PASSWORD): settings, reboot, WiFi scan, status, intercom.
    Shell work (flash, SD card) goes through CamSsh."""

    class WrongPassword(Exception):
        pass

    def __init__(self, host, port, passwords):
        """passwords: tried in order (the form's, then the one that worked last:
        a changed ADMIN_PASSWORD only takes effect after the camera restarts)."""
        self.host, self.port = host, int(port)
        self.serial = '%s:%d' % (host, self.port)        # shown like an adb serial
        self.base = 'http://%s:%d' % (host, self.port)
        self.passwords = [p for i, p in enumerate(passwords) if p and p not in passwords[:i]]
        self.password = None
        self._use(self.passwords[0])

    class _Digest(urllib.request.HTTPDigestAuthHandler):
        """Give up at the first rejected login (urllib would retry 5 more times,
        each costing 1 s on the camera); only a stale nonce is retried."""
        def http_error_401(self, req, fp, code, msg, headers):
            if req.get_header('Authorization') and 'stale=true' not in headers.get('WWW-Authenticate', '').lower():
                raise urllib.error.HTTPError(req.full_url, 401, 'wrong password', headers, fp)
            return super().http_error_401(req, fp, code, msg, headers)

    def _use(self, password):
        pm = urllib.request.HTTPPasswordMgrWithDefaultRealm()
        pm.add_password(None, self.base, 'admin', password)
        self.opener = urllib.request.build_opener(CamNet._Digest(pm))
        self.password = password

    def request(self, method, path, body=None, timeout=10):
        for i, pw in enumerate(self.passwords):
            if pw != self.password:
                self._use(pw)
            try:
                return self._request(method, path, body, timeout)
            except CamNet.WrongPassword:
                if i == len(self.passwords) - 1:
                    raise RuntimeError('Wrong admin password for the camera at %s.\n'
                                       'It is the ADMIN_PASSWORD on the camera (Remote tab); a changed '
                                       'password takes effect after the camera restarts.' % self.serial)

    def _request(self, method, path, body, timeout):
        req = urllib.request.Request(self.base + path, data=body, method=method)
        if body is not None:
            req.add_header('Content-Type', 'text/plain; charset=utf-8')
        try:
            with self.opener.open(req, timeout=timeout) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code == 401:
                raise CamNet.WrongPassword()
            raise RuntimeError('Camera %s: %s %s' % (self.serial, e.code, e.read().decode('utf-8', 'replace').strip()))
        except (urllib.error.URLError, OSError) as e:
            raise RuntimeError('The camera does not answer at %s (%s).\nIs it on, on the same network, '
                               'and does its config.txt have an ADMIN_PASSWORD?'
                               % (self.serial, getattr(e, 'reason', e)))

    def get(self, path, timeout=10):
        return self.request('GET', path, timeout=timeout).decode('utf-8', 'replace')

    def post(self, path, timeout=10):
        return self.request('POST', path, body=b'', timeout=timeout).decode('utf-8', 'replace')

    # config.txt travels encrypted (seal / seal_open, the key comes from the admin password).
    # Time stamps use the camera's clock, so a wrong camera clock does not break it.
    def _camera_now(self):
        if getattr(self, '_clock_offset', None) is None:
            import calendar
            st = parse_config(self.get('/api/status'))
            cam = calendar.timegm(time.strptime(st['TIME_UTC'], '%Y-%m-%d %H:%M:%S'))
            self._clock_offset = cam - time.time()
        return time.time() + self._clock_offset

    def _master(self):
        cache = self.__dict__.setdefault('_masters', {})
        if self.password not in cache:
            cache[self.password] = seal_master(self.password)
        return cache[self.password]

    def read_config(self):
        data = self.request('GET', '/api/config')           # may switch to the other password
        return seal_open(self._master(), data, self._camera_now()).decode('utf-8', 'replace')

    def write_config(self, text):
        now = self._camera_now()
        self.request('GET', '/api/status')                   # settle on the password that works
        body = seal(self._master(), text.encode('utf-8'), now)
        return self.request('PUT', '/api/config', body=body, timeout=20).decode('utf-8', 'replace')


# ----------------------------------------------------------------------------- operations

def sha256(data):
    return hashlib.sha256(data).hexdigest()


def read_mtd_table(adb):
    parts = []
    for line in adb.sh('cat /proc/mtd').splitlines():
        m = re.match(r'mtd(\d+): ([0-9a-f]+) [0-9a-f]+ "([^"]+)"', line.strip())
        if m:
            parts.append((int(m.group(1)), int(m.group(2), 16), m.group(3)))
    return parts


def check_camera(adb):
    """Raise if this does not look like a LAMAX X3.1 (Allwinner V3 CamDroid)."""
    prod = adb.sh('getprop ro.product.name').strip()
    table = [(name, size) for _, size, name in read_mtd_table(adb)]
    if table != EXPECTED_MTD:
        raise RuntimeError('unexpected flash layout %s (product %r) - not a LAMAX X3.1?' % (table, prod))
    return prod


def backup(adb, serial, log):
    check_camera(adb)
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    folder = os.path.join(BACKUPS, '%s_%s' % (serial, stamp))
    os.makedirs(folder, exist_ok=True)
    tmp = tempfile.mkdtemp()
    lines = []
    try:
        for idx, size, name in read_mtd_table(adb):
            data = []
            for attempt in (1, 2):
                p = os.path.join(tmp, 'r%d' % attempt)
                adb.pull('/dev/block/mtdblock%d' % idx, p)
                data.append(open(p, 'rb').read())
            if data[0] != data[1] or len(data[0]) != size:
                raise RuntimeError('mtdblock%d: the two reads differ or have the wrong size' % idx)
            fn = 'mtdblock%d_%s.bin' % (idx, name)
            open(os.path.join(folder, fn), 'wb').write(data[0])
            lines.append('%s  %s  %d' % (sha256(data[0]), fn, size))
            log('  %-28s %8d bytes  read twice, identical' % (fn, size))
        info = adb.sh('cat /proc/mtd; getprop')
        open(os.path.join(folder, 'device_info.txt'), 'w', encoding='utf-8').write(info)
        open(os.path.join(folder, 'SHA256SUMS'), 'w').write('\n'.join(lines) + '\n')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    log('Backup saved to %s' % folder)
    return folder


def latest_backup(serial, original_only=True):
    """Newest backup folder of this camera; by default the newest one whose
    boot partition is still the original (taken before the hook was installed)."""
    if not os.path.isdir(BACKUPS):
        return None
    dirs = sorted((d for d in os.listdir(BACKUPS) if d.startswith(serial + '_')
                   and os.path.isfile(os.path.join(BACKUPS, d, 'mtdblock1_boot.bin'))), reverse=True)
    for d in dirs:
        path = os.path.join(BACKUPS, d)
        if not original_only or not make_boot.has_hook(open(os.path.join(path, 'mtdblock1_boot.bin'), 'rb').read()):
            return path
    return None


def read_sums(folder):
    """{file name: sha256} from a backup's SHA256SUMS (empty if missing)."""
    try:
        return {p[1]: p[0] for p in (l.split() for l in open(os.path.join(folder, 'SHA256SUMS'))) if len(p) >= 2}
    except OSError:
        return {}


def camera_system_hash(adb):
    """sha256 of the camera's system partition. We never change it, and it differs
    between firmware revisions - so it tells which backups fit this camera."""
    tmp = tempfile.mkdtemp()
    try:
        p = os.path.join(tmp, 'system.bin')
        adb.pull('/dev/block/mtdblock2', p)
        return sha256(open(p, 'rb').read())
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def find_original_boot(adb, serial, log):
    """(folder, boot image) of an original (hook-free) boot partition that belongs to this
    camera's firmware: its own backups first (newest first), then ORIGINAL_FW. A candidate
    must have the same system partition as the camera - the adb serial is not unique
    (several cameras report 20080411), and firmware revisions differ between units."""
    sys_hash = camera_system_hash(adb)
    names = sorted(os.listdir(BACKUPS), reverse=True) if os.path.isdir(BACKUPS) else []   # newest first
    names.sort(key=lambda d: not d.startswith(serial + '_'))                                # this serial first
    for folder in [os.path.join(BACKUPS, d) for d in names] + ORIGINAL_FW_DIRS:
        sums = read_sums(folder)
        bootf = os.path.join(folder, 'mtdblock1_boot.bin')
        if sums.get('mtdblock2_system.bin') != sys_hash or not os.path.isfile(bootf):
            continue
        boot = open(bootf, 'rb').read()
        if sums.get('mtdblock1_boot.bin') != sha256(boot) or make_boot.has_hook(boot):
            continue
        log('Original boot partition for this camera: %s' % folder)
        return folder, boot
    return None, None


FLASH_SCRIPT = '''
# keep going if the connection drops (WiFi/SSH): never stop halfway through a flash
trap '' HUP INT TERM PIPE
cmp /dev/lt_new.img /dev/block/mtdblock1 >/dev/null 2>&1 && { echo ALREADY; rm -f /dev/lt_*.img; exit 0; }
cat /dev/lt_new.img > /dev/block/mtdblock1
sync
echo 3 > /proc/sys/vm/drop_caches
if cmp /dev/lt_new.img /dev/block/mtdblock1 >/dev/null; then
    echo VERIFIED
else
    echo VERIFY_FAILED_RESTORING
    cat /dev/lt_orig.img > /dev/block/mtdblock1
    sync
    echo 3 > /proc/sys/vm/drop_caches
    cmp /dev/lt_orig.img /dev/block/mtdblock1 >/dev/null && echo RESTORED || echo RESTORE_FAILED
fi
rm -f /dev/lt_new.img /dev/lt_orig.img /dev/lt_flash.sh
'''


def flash_boot(adb, image, orig, log):
    """Write `image` to mtd1, verify by read-back, put `orig` back on mismatch."""
    if len(image) != PART_SIZE or len(orig) != PART_SIZE or image[:8] != b'ANDROID!':
        raise RuntimeError('refusing to flash: image must be a %d-byte Android boot image' % PART_SIZE)
    check_camera(adb)
    tmp = tempfile.mkdtemp()
    try:
        for name, data in (('lt_new.img', image), ('lt_orig.img', orig)):
            p = os.path.join(tmp, name)
            open(p, 'wb').write(data)
            adb.push(p, '/dev/' + name)
        p = os.path.join(tmp, 'lt_flash.sh')
        open(p, 'w', newline='\n').write(FLASH_SCRIPT)
        adb.push(p, '/dev/lt_flash.sh')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    log('Writing the boot partition (about 1-3 minutes, do not unplug)...')
    out = adb.sh('sh /dev/lt_flash.sh 2>&1', timeout=900)
    log(out.strip())
    if 'VERIFIED' in out or 'ALREADY' in out:
        return True
    raise RuntimeError('flash verification failed - DO NOT reboot; see the log above')


def install_hook(adb, serial, log):
    folder, orig = find_original_boot(adb, serial, log)
    if not folder:
        raise RuntimeError('No backup of this camera\'s original firmware (and ORIGINAL_FW is a different '
                           'firmware revision) - make a backup first, before installing the hook.')
    tmp = tempfile.mkdtemp()
    try:
        p = os.path.join(tmp, 'cur.bin')
        adb.pull('/dev/block/mtdblock1', p)
        current = open(p, 'rb').read()
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if make_boot.has_hook(current):
        log('The boot hook is already installed on this camera.')
        return
    if current != orig:
        raise RuntimeError('The boot partition on the camera differs from the backup in %s. '
                           'Make a new backup first.' % folder)
    log('Building the hooked boot image from this camera\'s own backup (%s)' % os.path.basename(folder))
    image = make_boot.build_hook_image(orig, log=lambda m: log('  ' + m))
    flash_boot(adb, image, orig, log)
    log('Boot hook installed. It takes effect after a reboot.')


def restore_boot(adb, serial, log):
    folder, orig = find_original_boot(adb, serial, log)
    if not folder:
        raise RuntimeError('No original boot partition for this camera: no backup of its own, and '
                           'ORIGINAL_FW is a different firmware revision (its system partition differs).')
    flash_boot(adb, orig, orig, log)
    log('Original boot partition restored. It takes effect after a reboot.')


# ----------------------------------------------------------------------------- settings
# (tab, key, label, kind, extra). Defaults come from the config.txt template.
# kinds: text, password, ssid, ip, int (min, max), choice (options), bool (on, off)
SETTINGS = [
    ('Network', 'CAMERA_NAME', 'Camera name', 'text', None),
    ('Network', 'WIFI_SSID', 'WiFi name (2.4 GHz)', 'ssid', None),
    ('Network', 'WIFI_PASSWORD', 'WiFi password', 'password', None),
    ('Network', 'IP_MODE', 'IP mode', 'choice', ['dhcp', 'static']),
    ('Network', 'IP_ADDRESS', 'Static IP', 'ip', None),
    ('Network', 'IP_NETMASK', 'Netmask', 'ip', None),
    ('Network', 'IP_GATEWAY', 'Gateway', 'ip', None),
    ('Network', 'IP_DNS', 'DNS', 'ip', None),
    ('Video', 'RESOLUTION', 'Resolution', 'choice', ['2688x1520', '1920x1080', '1280x720']),
    ('Video', 'VIDEO_FPS', 'Frames per second', 'int', (1, 30)),
    ('Video', 'VIDEO_BITRATE_KBPS', 'Bitrate (kbit/s)', 'int', (200, 12000)),
    ('Video', 'VIDEO_GOP_SECONDS', 'Key frame every (s)', 'int', (1, 10)),
    ('Video', 'VIDEO_PROFILE', 'H.264 profile', 'choice', ['baseline', 'main', 'high']),
    ('Video', 'VIDEO_FLIP', 'Rotate picture 180°', 'bool', ('1', '0')),
    ('Time', 'OSD_TIME', 'Date and time in the picture', 'bool', ('1', '0')),
    ('Time', 'OSD_NAME', 'Camera name before the time', 'bool', ('1', '0')),
    ('Time', 'OSD_BATTERY', 'Battery % when not on external power', 'bool', ('1', '0')),
    ('Time', 'OSD_POSITION', 'Position', 'choice', ['top-left', 'top-right', 'bottom-left', 'bottom-right']),
    ('Time', 'NTP_SERVER', 'Time server (NTP)', 'text', None),
    ('Time', 'TIMEZONE', 'Time zone', 'tz', None),
    ('RTSP', 'RTSP_PORT', 'RTSP port', 'int', (1, 65535)),
    ('RTSP', 'RTSP_PATH', 'Stream path', 'text', None),
    ('RTSP', 'RTSP_USER', 'User name (empty = no login)', 'text', None),
    ('RTSP', 'RTSP_PASSWORD', 'Password', 'password', None),
    ('RTSP', 'RTSP_MAX_CLIENTS', 'Max. clients', 'int', (1, 8)),
    ('RTSP', 'AUDIO', 'Microphone sound in the stream (G.711)', 'bool', ('1', '0')),
    ('RTSP', 'AUDIO_GAIN', 'Microphone gain (0-7)', 'int', (0, 7)),
    ('Remote', 'ADMIN_PASSWORD', 'Admin password (WiFi + SSH; empty = USB only)', 'password', None),
    ('Remote', 'ADMIN_PORT', 'Port', 'int', (1, 65535)),
    ('Device', 'MODE', 'Mode', 'choice', ['camera', 'stock']),
    ('Device', 'SCREEN_TIMEOUT', 'Screen off after (s, 0 = never)', 'int', (0, 3600)),
    ('Device', 'LED', 'Status LEDs', 'bool', ('on', 'off')),    ('Device', 'BATTERY_LONG_LIFE', 'Battery long life (charge to ~85-90 %, off at 10 %)', 'bool', ('1', '0')),
    ('Device', 'WATCHDOG', 'Reboot if the stream hangs', 'bool', ('1', '0')),
    ('Device', 'LOG', 'Log (sd / ram = not on the card / off)', 'choice', ['sd', 'ram', 'off']),
    ('Device', 'WIFI_DEBUG_LOG', 'Detailed WiFi log (on the card)', 'bool', ('1', '0')),]
# The camera has no time zone database: TIMEZONE is a POSIX TZ string.
TIMEZONES = [
    ('UTC', 'UTC0'),
    ('UK, Ireland, Portugal', 'GMT0BST,M3.5.0/1,M10.5.0'),
    ('Central Europe (Budapest, Berlin, Paris)', 'CET-1CEST,M3.5.0,M10.5.0/3'),
    ('Eastern Europe (Helsinki, Athens, Kyiv)', 'EET-2EEST,M3.5.0/3,M10.5.0/4'),
    ('Moscow, Istanbul', 'MSK-3'),
    ('US Eastern', 'EST5EDT,M3.2.0,M11.1.0'),
    ('US Central', 'CST6CDT,M3.2.0,M11.1.0'),
    ('US Mountain', 'MST7MDT,M3.2.0,M11.1.0'),
    ('US Pacific', 'PST8PDT,M3.2.0,M11.1.0'),
]
TZ_LABEL = {tz: '%s  [%s]' % (name, tz) for name, tz in TIMEZONES}

FALLBACK_DEFAULTS = {'CAMERA_NAME': 'camera1', 'IP_MODE': 'dhcp', 'IP_ADDRESS': '192.168.1.50',
                     'IP_NETMASK': '255.255.255.0', 'IP_GATEWAY': '192.168.1.1', 'IP_DNS': '192.168.1.1',
                     'VIDEO_WIDTH': '1920', 'VIDEO_HEIGHT': '1080', 'VIDEO_FPS': '15',
                     'VIDEO_BITRATE_KBPS': '3000', 'VIDEO_GOP_SECONDS': '2', 'VIDEO_PROFILE': 'main',
                     'VIDEO_FLIP': '0', 'RTSP_PORT': '554', 'RTSP_PATH': 'stream', 'RTSP_MAX_CLIENTS': '4',
                     'ADMIN_PASSWORD': '', 'ADMIN_PORT': '8080',
                     'AUDIO': '0', 'AUDIO_GAIN': '7',
                     'MODE': 'camera', 'SCREEN_TIMEOUT': '60', 'LED': 'on', 'BATTERY_LONG_LIFE': '1',
                     'WATCHDOG': '1', 'WIFI_DEBUG_LOG': '0', 'LOG': 'off', 'OSD_TIME': '1', 'OSD_NAME': '0', 'OSD_BATTERY': '1',
                     'OSD_POSITION': 'top-left', 'TIMEZONE': 'CET-1CEST,M3.5.0,M10.5.0/3', 'NTP_SERVER': 'pool.ntp.org'}


def parse_config(text):
    """KEY=VALUE lines -> dict (upper-case keys; comments, blanks, CRLF and quotes handled
    the same way as on the camera)."""
    out = {}
    for line in text.splitlines():
        line = line.strip()
        if not line or line[0] in '#;' or '=' not in line:
            continue
        k, v = line.split('=', 1)
        k, v = k.strip().upper(), v.strip()
        if len(v) >= 2 and v[0] == v[-1] and v[0] in '"\'':
            v = v[1:-1]
        out[k] = v
    return out


# Suggested H.264 bitrate (main profile) at 15 fps; more fps cost less than
# proportionally (P-frames are small): x (fps/15)^0.6. 1080p: 15 -> 3000, 30 -> 4500.
BITRATE_AT_15FPS = {'2688x1520': 5000, '1920x1080': 3000, '1280x720': 1500}


def suggested_bitrate(resolution, fps):
    try:
        fps = max(1, min(30, int(fps)))
    except (TypeError, ValueError):
        return None
    base = BITRATE_AT_15FPS.get(resolution)
    if base is None:
        return None
    return int(round(base * (fps / 15.0) ** 0.6 / 100.0)) * 100


def default_values():
    d = dict(FALLBACK_DEFAULTS)
    try:
        d.update(parse_config(open(os.path.join(SD_SRC, 'config.txt'), encoding='utf-8').read()))
    except OSError:
        pass
    return d


def is_ipv4(s):
    p = s.split('.')
    return len(p) == 4 and all(x.isdigit() and 0 <= int(x) <= 255 and str(int(x)) == x for x in p)


def validate(v):
    """Raise RuntimeError with a readable message if a value cannot work on the camera."""
    labels = {key: label for _, key, label, _, _ in SETTINGS}
    for _, key, label, kind, extra in SETTINGS:
        if kind == 'int' and key in v:
            try:
                n = int(v[key])
            except ValueError:
                raise RuntimeError('%s: "%s" is not a number' % (label, v[key]))
            if not extra[0] <= n <= extra[1]:
                raise RuntimeError('%s must be between %d and %d' % (label, extra[0], extra[1]))
    for key, val in v.items():
        if '\n' in val or '\r' in val:
            raise RuntimeError('%s must be a single line' % labels.get(key, key))
    if len(v.get('WIFI_SSID', '').encode('utf-8')) > 32:
        raise RuntimeError('The WiFi name can be at most 32 bytes long.')
    pw = v.get('WIFI_PASSWORD', '')
    if pw and not 8 <= len(pw) <= 63:
        raise RuntimeError('The WiFi password must be 8-63 characters (or empty for an open network).')
    if v.get('IP_MODE') == 'static':
        for key in ('IP_ADDRESS', 'IP_NETMASK', 'IP_GATEWAY'):
            if not is_ipv4(v.get(key, '')):
                raise RuntimeError('%s: "%s" is not a valid IPv4 address' % (labels[key], v.get(key, '')))
    if v.get('IP_DNS') and not is_ipv4(v['IP_DNS']):
        raise RuntimeError('DNS: "%s" is not a valid IPv4 address' % v['IP_DNS'])
    if not re.match(r'^[A-Za-z0-9._~-]+$', v.get('RTSP_PATH', 'stream')):
        raise RuntimeError('Stream path may only contain letters, digits and . _ ~ -')
    if not re.match(r'^[A-Za-z0-9.:-]+$', v.get('NTP_SERVER', 'pool.ntp.org')):
        raise RuntimeError('Time server: "%s" is not a host name or IP address' % v.get('NTP_SERVER', ''))
    if not re.match(r'^[A-Za-z<][A-Za-z0-9<>+,./:-]*$', v.get('TIMEZONE', 'UTC0')):
        raise RuntimeError('Time zone: "%s" is not a POSIX TZ string (e.g. CET-1CEST,M3.5.0,M10.5.0/3)'
                           % v.get('TIMEZONE', ''))
    if bool(v.get('RTSP_USER')) != bool(v.get('RTSP_PASSWORD')):
        raise RuntimeError('Set both the RTSP user name and password, or neither.')
    if any(ch in v.get('RTSP_USER', '') + v.get('RTSP_PASSWORD', '') for ch in '"\\'):
        raise RuntimeError('The RTSP user name and password cannot contain " or \\ (Digest login).')
    ap = v.get('ADMIN_PASSWORD', '')
    if ap and len(ap) < 8:
        raise RuntimeError('The admin password must be at least 8 characters (or empty: USB only).')
    if ap and ap == v.get('RTSP_PASSWORD'):
        raise RuntimeError('Use a different admin password than the RTSP password: '
                           'whoever can watch the stream should not be able to change the settings.')


def user_settings_dir():
    if WINDOWS:
        return os.path.join(os.environ.get('APPDATA') or os.path.expanduser('~'), 'LamaxTool')
    return os.path.join(os.environ.get('XDG_CONFIG_HOME') or os.path.expanduser('~/.config'), 'lamaxtool')


# Kept next to the program (like backups/), so a USB stick or folder carries it along.
# Falls back to the user's settings folder if the program folder is read-only.
LAST_SETTINGS = os.path.join(HOME, 'LamaxTool_settings.txt')
LAST_SETTINGS_USER = os.path.join(user_settings_dir(), 'last_config.txt')


def load_last_settings():
    """KEY -> value dict of the settings used last time, or None."""
    for path in (LAST_SETTINGS, LAST_SETTINGS_USER):
        try:
            v = parse_config(open(path, encoding='utf-8').read())
        except OSError:
            continue
        if v:
            return v
    return None


def _write_settings(path, values):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    text = ''.join('%s=%s\n' % kv for kv in sorted(values.items()))
    tmp = path + '.tmp'
    with open(tmp, 'w', encoding='utf-8') as f:
        f.write('# LamaxTool: settings used last time (also contains the WiFi / RTSP passwords)\n' + text)
    if not WINDOWS:
        os.chmod(tmp, 0o600)
    os.replace(tmp, path)


def save_last_settings(values):
    """Returns the file written."""
    try:
        _write_settings(LAST_SETTINGS, values)
        path = LAST_SETTINGS
    except OSError:
        _write_settings(LAST_SETTINGS_USER, values)
        return LAST_SETTINGS_USER
    try:                                    # one copy only: drop the one in the user folder
        os.remove(LAST_SETTINGS_USER)
    except OSError:
        pass
    return path


def make_config(values):
    """config.txt from the template with the given KEY -> value replacements."""
    text = open(os.path.join(SD_SRC, 'config.txt'), encoding='utf-8').read()
    for k, v in values.items():
        text, n = re.subn(r'(?m)^%s=.*$' % re.escape(k), lambda m: '%s=%s' % (k, v), text)
        if not n:
            text += '\n%s=%s\n' % (k, v)
    return text.replace('\r\n', '\n').replace('\n', '\r\n')    # Notepad-friendly


def sd_files():
    files = [('autorun.sh', os.path.join(SD_SRC, 'autorun.sh'))]
    bindir = os.path.join(SD_SRC, 'bin')
    for n in sorted(os.listdir(bindir)) if os.path.isdir(bindir) else []:
        files.append(('bin/' + n, os.path.join(bindir, n)))
    if len(files) < 6:
        raise RuntimeError('camera programs missing in %s (build them first, see BUILD.md)' % bindir)
    return files


def write_sd_adb(adb, config_text, log):
    mounts = adb.sh('cat /proc/mounts')
    if ' /mnt/extsd ' not in mounts:
        raise RuntimeError('The SD card is not mounted on the camera. If the camera shows the USB mode '
                           'question, the card is lent to the PC: use "Write to drive" instead, or insert a card.')
    adb.sh('mkdir -p /mnt/extsd/cam/bin')
    for rel, src in sd_files():
        adb.push(src, '/mnt/extsd/cam/' + rel)
        log('  cam/' + rel)
    tmp = tempfile.mkdtemp()                # config.txt always reflects the form
    try:
        p = os.path.join(tmp, 'config.txt')
        open(p, 'w', encoding='utf-8', newline='').write(config_text)
        adb.push(p, '/mnt/extsd/cam/config.txt')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    log('  cam/config.txt written')
    adb.sh('rm -f /mnt/extsd/cam/startup.wav; sync')     # own startup sound: no longer used
    log('SD card ready.')


def write_sd_folder(root, config_text, log):
    dst = os.path.join(root, 'cam')
    os.makedirs(os.path.join(dst, 'bin'), exist_ok=True)
    for rel, src in sd_files():
        shutil.copyfile(src, os.path.join(dst, rel.replace('/', os.sep)))
        log('  cam/' + rel)
    try:
        os.remove(os.path.join(dst, 'startup.wav'))       # own startup sound: no longer used
    except OSError:
        pass
    open(os.path.join(dst, 'config.txt'), 'w', encoding='utf-8', newline='').write(config_text)
    log('  cam/config.txt written')
    log('Files copied to %s - eject the drive safely before removing the card.' % dst)


def download_platform_tools(log):
    dst = os.path.join(HOME, 'platform-tools')
    log('Downloading Android platform-tools from Google...')
    data = urllib.request.urlopen(PLATFORM_TOOLS_URL, timeout=120).read()
    zipfile.ZipFile(io.BytesIO(data)).extractall(HOME)
    adb = os.path.join(dst, ADB_NAME)
    if not WINDOWS:
        os.chmod(adb, 0o755)            # zipfile does not keep the executable bit
    log('adb installed to %s' % dst)
    return adb


# ----------------------------------------------------------------------------- WiFi scan

def _band_from_mhz(mhz):
    return '2.4' if mhz < 3000 else '5' if mhz < 5925 else '6'


SCAN_ROUNDS = 3        # one scan takes 1-3 s on the camera but catches only part of the networks


def scan_wifi_camera(adb, on_update=None):
    """Networks the camera's own radio sees (needs our software running on it),
    one entry per name with the strongest signal, strongest first.
    First the camera's cached results (instant), then SCAN_ROUNDS fresh scans merged;
    on_update(nets, final) is called after each step."""
    if isinstance(adb, CamNet):
        def fetch(scan):
            if scan:
                adb.post('/api/wifi/scan')
                time.sleep(3)
            return adb.get('/api/wifi')
    else:
        w = '/dev/cam/wpa_cli -p /dev/cam/wpa'
        if 'yes' not in adb.sh('[ -S /dev/cam/wpa/wlan0 ] && echo yes || echo no', timeout=10):
            raise RuntimeError('The camera is not running the security camera software yet.\n'
                               'Install the boot hook, write the SD card, restart the camera and connect it '
                               'over USB again - or type the network name.')

        def fetch(scan):
            if scan:
                return adb.sh('%s scan >/dev/null; sleep 3; %s scan_results' % (w, w), timeout=20)
            return adb.sh('%s scan_results' % w, timeout=15)
    best = {}

    def merge(out):
        for line in out.splitlines():
            f = line.split('\t')
            if len(f) >= 5 and re.match(r'^[0-9a-f:]{17}$', f[0]) and f[4]:
                dbm = int(f[2])
                n = dict(ssid=f[4], signal=max(0, min(100, 2 * (dbm + 100))),
                         band=_band_from_mhz(int(f[1])), dbm=dbm)
                if n['ssid'] not in best or n['signal'] > best[n['ssid']]['signal']:
                    best[n['ssid']] = n
        return sorted(best.values(), key=lambda n: -n['signal'])

    nets = merge(fetch(False))
    for i in range(SCAN_ROUNDS):
        if on_update:
            on_update(nets, False)
        nets = merge(fetch(True))
    if on_update:
        on_update(nets, True)
    return nets


def rtsp_link(adb):
    """rtsp:// address of the running stream: live IP, port/path of the running
    server, user name and password from the camera's own config.txt (over WiFi
    the config comes encrypted)."""
    if isinstance(adb, CamNet):     # status has "ip=..." and the stream status line
        out = adb.get('/api/status').replace('ip=', 'wlan0: ', 1) + '\n' + adb.read_config()
    else:
        out = adb.sh('/dev/cam/camnet show wlan0; echo; cat /dev/cam/camd.status; echo; '
                     'cat /mnt/extsd/cam/config.txt', timeout=15)
    m = re.search(r'wlan0:\s+(\d+\.\d+\.\d+\.\d+)', out)
    if not m:
        raise RuntimeError('The camera has no IP address yet (WiFi not connected?).')
    st = dict(re.findall(r'\b(port|path)=(\S+)', out))
    cfg = parse_config(out)
    port = st.get('port') or cfg.get('RTSP_PORT') or '554'
    path = st.get('path') or cfg.get('RTSP_PATH') or 'stream'
    user, pw = cfg.get('RTSP_USER', ''), cfg.get('RTSP_PASSWORD', '')
    from urllib.parse import quote
    login = quote(user, safe='') + ':' + quote(pw, safe='') + '@' if user else ''
    return 'rtsp://%s%s%s/%s' % (login, m.group(1), '' if port == '554' else ':' + port, path.lstrip('/'))


# ----------------------------------------------------------------------------- GUI

MIC_GAINS = ['Auto', '+6 dB', '+12 dB', '+18 dB', '+24 dB', '+30 dB', '+36 dB']   # intercom microphone

TAB_ICONS = {'Network': '📶 ', 'Video': '🎥 ', 'Time': '🕒 ', 'RTSP': '📡 ',
             'Remote': '🌐 ', 'Device': '⚙ '}

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(APP)
        self.geometry('840x800')
        self.minsize(720, 620)
        self.q = queue.Queue()
        self.busy = False
        self.adb_exe = find_adb()
        self.serial = tk.StringVar()
        self.conn = tk.StringVar(value='usb')          # usb / wifi
        self.cam_ip = tk.StringVar()
        self.status = tk.StringVar(value='')
        self._build()
        self._pump_id = self.after(100, self._pump)
        self.after(300, self.refresh)

    # ---- layout
    def _build(self):
        pad = dict(padx=8, pady=4)
        st = ttk.Style(self)
        base = 'Segoe UI' if WINDOWS else 'TkDefaultFont'
        st.configure('Section.TLabelframe.Label', font=(base, 10, 'bold'), foreground='#1f4e8c')
        st.configure('Big.TButton', font=(base, 10, 'bold'), padding=(10, 6))
        st.configure('Tool.TButton', padding=(6, 3))
        st.configure('Hint.TLabel', foreground='#666')

        def button(parent, text, fn, style='Tool.TButton', **packopts):
            b = ttk.Button(parent, text=text, command=fn, style=style)
            b.pack(side='left', **packopts); self.btns.append(b)
            return b

        self.btns = []
        top = ttk.Frame(self); top.pack(fill='x', padx=8, pady=(8, 2))
        ttk.Label(top, text='📷  Camera:', font=(base, 10, 'bold')).pack(side='left')
        ttk.Radiobutton(top, text='🔌 USB', variable=self.conn, value='usb',
                        command=self.refresh).pack(side='left', padx=(6, 2))
        self.dev_box = ttk.Combobox(top, textvariable=self.serial, width=14, state='readonly')
        self.dev_box.pack(side='left', padx=(0, 8))
        ttk.Radiobutton(top, text='📶 WiFi  IP:', variable=self.conn, value='wifi',
                        command=self.refresh).pack(side='left', padx=(0, 2))
        ip_entry = ttk.Entry(top, textvariable=self.cam_ip, width=15)
        ip_entry.pack(side='left', padx=(0, 6))
        ip_entry.bind('<Return>', lambda _e: (self.conn.set('wifi'), self.refresh()))
        ttk.Button(top, text='⟳ Refresh', command=self.refresh, style='Tool.TButton').pack(side='left')
        self.talker = None
        self.talk_btn = ttk.Button(top, text='🎤 Talk', command=self.toggle_talk, style='Tool.TButton')
        self.talk_btn.pack(side='left', padx=(8, 2))
        self.mic_gain = tk.StringVar(value='Auto')
        ttk.Label(top, text='mic gain').pack(side='left')
        ttk.Combobox(top, textvariable=self.mic_gain, values=MIC_GAINS, state='readonly', width=7).pack(side='left', padx=(2, 0))
        top2 = ttk.Frame(self); top2.pack(fill='x', padx=8)
        ttk.Label(top2, textvariable=self.status, style='Hint.TLabel').pack(side='left')

        # ---- 1: flash (one time per camera)
        self.flash_frame = flash = ttk.LabelFrame(self, style='Section.TLabelframe', padding=(6, 4))
        flash.pack(fill='x', **pad)
        self.usb_only_btns = []                         # disabled while "📶 WiFi" is chosen
        for text, fn, usb in (('💾 Back up flash', self.do_backup, True), ('🔧 Install boot hook', self.do_install, True),
                              ('↩ Restore original boot', self.do_restore, True),
                              ('⏻ Reboot camera', self.do_reboot, False), ('📁 Backups folder', self.open_backups, False)):
            b = button(flash, text, fn, padx=(0, 6), pady=2)
            if usb:
                self.usb_only_btns.append(b)

        # ---- 2: settings (edited here, stored on this computer)
        sd = ttk.LabelFrame(self, text=' 2 │ Settings  (config.txt) ', style='Section.TLabelframe',
                            padding=(6, 4)); sd.pack(fill='x', **pad)
        bar = ttk.Frame(sd); bar.pack(fill='x', pady=(0, 2))
        for text, fn in (('📂 Open file...', self.do_load_file), ('📥 Read from camera', self.do_load_camera),
                         ('💾 Save as file...', self.do_save_file), ('↺ Defaults', self.do_defaults)):
            button(bar, text, fn, padx=(0, 4))
        self.cfg_source = tk.StringVar(value='defaults')
        ttk.Label(bar, textvariable=self.cfg_source, style='Hint.TLabel').pack(side='right', padx=4)
        ttk.Label(bar, text='Loaded:', style='Hint.TLabel').pack(side='right')

        nb = ttk.Notebook(sd); nb.pack(fill='x', pady=4)
        self.cfg, self.ssid_map, self.extra_cfg = {}, {}, {}
        tabs, counts = {}, {}
        for tab, key, label, kind, extra in SETTINGS:
            if tab not in tabs:
                tabs[tab] = ttk.Frame(nb, padding=6); nb.add(tabs[tab], text=TAB_ICONS.get(tab, '') + tab); counts[tab] = 0
            f, i = tabs[tab], counts[tab]; counts[tab] += 1
            if kind == 'tz':                   # wide field: a row of its own
                i += i % 2; counts[tab] = i + 2
            r, c = i // 2, (i % 2) * 2
            if kind == 'bool':
                v = tk.BooleanVar(); self.cfg[key] = v
                ttk.Checkbutton(f, text=label, variable=v).grid(row=r, column=c, columnspan=2, sticky='w', padx=4, pady=2)
                continue
            ttk.Label(f, text=label).grid(row=r, column=c, sticky='e', padx=4, pady=2)
            v = tk.StringVar(); self.cfg[key] = v
            cell = dict(row=r, column=c + 1, sticky='w', padx=4, pady=2)
            if kind == 'ssid':                 # pick from "Scan" results or type it
                fr = ttk.Frame(f); fr.grid(**cell)
                self.ssid_box = ttk.Combobox(fr, textvariable=v, width=26)
                self.ssid_box.pack(side='left')
                self.ssid_box.bind('<<ComboboxSelected>>', self.on_ssid_selected)
                button(fr, '🔍 Scan WiFi w. camera', self.do_wifi_scan, padx=(3, 0))
            elif kind == 'choice':
                ttk.Combobox(f, textvariable=v, values=extra, state='readonly', width=24).grid(**cell)
            elif kind == 'tz':                 # pick a common zone or type any POSIX TZ string
                ttk.Combobox(f, textvariable=v, values=[TZ_LABEL[tz] for _, tz in TIMEZONES], width=60).grid(
                    row=r, column=c + 1, columnspan=3, sticky='w', padx=4, pady=2)
            elif key == 'VIDEO_BITRATE_KBPS':  # spinbox + "use the suggested value" button
                fr = ttk.Frame(f); fr.grid(**cell)
                ttk.Spinbox(fr, textvariable=v, from_=extra[0], to=extra[1], increment=100, width=8).pack(side='left')
                self.br_hint = tk.StringVar()
                self.br_btn = button(fr, '', self.use_suggested_bitrate, padx=(3, 0))
                self.br_btn.configure(textvariable=self.br_hint)
            elif kind == 'int':
                ttk.Spinbox(f, textvariable=v, from_=extra[0], to=extra[1], width=24).grid(**cell)
            else:
                ttk.Entry(f, textvariable=v, width=26, show='*' if kind == 'password' else '').grid(**cell)
        # RTSP tab: the running camera's stream address, ready to copy (VLC, NVR ...)
        lf = ttk.Frame(tabs['RTSP']); lf.grid(row=50, column=0, columnspan=4, sticky='we', padx=4, pady=(8, 2))
        button(lf, '🔗 Get stream link from camera', self.do_rtsp_link)
        self.rtsp_link = tk.StringVar()
        ttk.Entry(lf, textvariable=self.rtsp_link, state='readonly', width=48).pack(side='left', padx=4)
        button(lf, '📋 Copy', self.copy_rtsp_link)
        ttk.Label(tabs['Remote'], style='Hint.TLabel', justify='left', text=
                  'With an admin password everything works over WiFi too: choose "📶 WiFi" at the top and enter\n'
                  'the camera IP. Settings, WiFi scan, stream link, reboot and talk go through the config service\n'
                  '(user admin); flash backup, boot hook and writing the SD card (programs too) through SSH\n'
                  '(user root, port 22, same password). The first SD write with SSH must be over USB.\n'
                  'A new password takes effect after a restart. Terminal: ssh root@<camera IP>'
                  ).grid(row=50, column=0, columnspan=4, sticky='w', padx=4, pady=(8, 2))
        self._br_suggest = None
        for k in ('RESOLUTION', 'VIDEO_FPS'):
            self.cfg[k].trace_add('write', lambda *_: self.on_video_change())
        ttk.Label(sd, text='The last used settings are remembered in LamaxTool_settings.txt next to the program (passwords included).',
                  style='Hint.TLabel').pack(anchor='w')
        last = load_last_settings()
        if last is not None:
            self.set_values(last)
            self.cfg_source.set('last used settings')
        else:
            self.set_values(default_values())

        # ---- 3: write the SD card
        self.write_frame = wr = ttk.LabelFrame(self, style='Section.TLabelframe', padding=(6, 6))
        wr.pack(fill='x', **pad)
        self.write_btn = button(wr, '', self.do_sd_adb, style='Big.TButton', padx=(0, 6))
        button(wr, '🖴  Write to drive / folder...', self.do_sd_folder, style='Big.TButton', padx=(0, 6))
        self.protocol('WM_DELETE_WINDOW', self.on_close)
        self.conn.trace_add('write', lambda *_: self.update_conn_ui())
        self.update_conn_ui()

        logf = ttk.LabelFrame(self, text=' 📝 Log ', style='Section.TLabelframe'); logf.pack(fill='both', expand=True, **pad)
        self.log_box = tk.Text(logf, height=14, wrap='word', font=('Consolas', 9) if WINDOWS else 'TkFixedFont')
        sb = ttk.Scrollbar(logf, command=self.log_box.yview); self.log_box['yscrollcommand'] = sb.set
        sb.pack(side='right', fill='y'); self.log_box.pack(fill='both', expand=True)
        self.log('%s - backups are stored in %s' % (APP, BACKUPS))

    # ---- helpers
    def log(self, msg):
        self.q.put(msg)

    def _pump(self):
        """Run queued GUI updates from worker threads. A failing callback must
        never stop the pump, or the buttons would stay disabled for good."""
        try:
            while True:
                try:
                    m = self.q.get_nowait()
                except queue.Empty:
                    break
                try:
                    if callable(m):
                        m()
                    else:
                        self.log_box.insert('end', m + '\n'); self.log_box.see('end')
                except Exception as e:
                    self.log_box.insert('end', 'internal error: %r\n' % e)
        finally:
            self._pump_id = self.after(100, self._pump)

    def set_busy(self, busy):
        self.busy = busy
        for b in self.btns:
            b.state(['disabled'] if busy else ['!disabled'])
        if not busy:
            self.update_conn_ui()

    def update_conn_ui(self):
        """Titles, button texts and USB-only buttons follow the 🔌 USB / 📶 WiFi choice."""
        wifi = self.wifi()
        self.flash_frame.configure(text=' 1 │ Camera flash  (once per camera%s) '
                                   % (' – over WiFi via SSH' if wifi else ''))
        self.write_frame.configure(text=' 3 │ Write to camera SD card  (programs + config.txt, over WiFi via SSH) '
                                   if wifi else ' 3 │ Write to SD card  (programs + config.txt) ')
        self.write_btn.configure(text='⬆  Write to camera SD card (WiFi)' if wifi
                                 else '⬆  Write to camera SD card (USB)')
        if not self.busy:                     # everything works over USB and over WiFi (SSH) now
            for b in self.usb_only_btns:
                b.state(['!disabled'])

    def wifi(self):
        return self.conn.get() == 'wifi'

    def camnet(self):
        """CamNet for the IP at the top and the admin password of the form, or None (message shown)."""
        ip = self.cam_ip.get().strip()
        pw = self.cfg['ADMIN_PASSWORD'].get()
        if not is_ipv4(ip):
            messagebox.showwarning(APP, 'Enter the camera\'s IP address next to "📶 WiFi" at the top.\n'
                                        '(With the camera on USB, "Get stream link" on the RTSP tab shows it.)')
            return None
        if not pw:
            messagebox.showwarning(APP, 'Enter the camera\'s admin password on the 🌐 Remote tab '
                                        '(the ADMIN_PASSWORD in its config.txt).')
            return None
        port = self.cfg['ADMIN_PORT'].get().strip() or '8080'
        return CamNet(ip, port if port.isdigit() else '8080', [pw, getattr(self, '_net_pw_ok', '')])

    def run_task(self, title, fn, need_device=True, usb_only=False):
        """fn(conn) in a worker thread. conn: an Adb (USB); over WiFi a CamNet (the config
        service), or for shell tasks (usb_only) a CamSsh, which behaves like Adb."""
        if self.busy:
            return
        net = None
        if need_device and self.wifi():
            net = self.camnet()
            if not net:
                return
        elif need_device and not self.serial.get():
            messagebox.showwarning(APP, 'No camera selected. Connect it over USB and press Refresh '
                                        '(or choose "📶 WiFi" and enter its IP).')
            return
        self.set_busy(True)
        shell = bool(net and usb_only)
        self.log('--- %s%s' % (title, (' (WiFi %s, SSH)' if shell else ' (WiFi %s)') % net.host if net else ''))
        serial = self.serial.get() or None      # Tk variables are read in the GUI thread only

        def worker():
            conn = None
            try:
                if shell:
                    conn = CamSsh(net.host, net.passwords)
                    self._net_pw_ok = conn.password
                else:
                    conn = net or (Adb(self.adb_exe, serial) if self.adb_exe else None)
                fn(conn)
                if net and not shell:
                    self._net_pw_ok = net.password      # remembered for this session only
                self.log('--- done')
            except Exception as e:      # shown to the user, never fatal
                msg = str(e)            # `e` is deleted when the except block ends
                self.log('ERROR: %s' % msg)
                self.q.put(lambda: messagebox.showerror(APP, msg))
            finally:
                if isinstance(conn, CamSsh):
                    conn.close()
                self.q.put(lambda: self.set_busy(False))
                self.q.put(self.refresh)
        threading.Thread(target=worker, daemon=True).start()

    def on_video_change(self):
        """Resolution or fps changed: update the suggestion; a bitrate that was
        the previous suggestion follows it (a hand-set value is kept)."""
        new = suggested_bitrate(self.cfg['RESOLUTION'].get(), self.cfg['VIDEO_FPS'].get())
        if new is None:
            self.br_hint.set('💡 suggested: ?')
            return
        br = self.cfg['VIDEO_BITRATE_KBPS']
        if self._br_suggest is not None and br.get().strip() == str(self._br_suggest):
            br.set(str(new))
        self._br_suggest = new
        self.br_hint.set('💡 suggested: %d' % new)

    def use_suggested_bitrate(self):
        if self._br_suggest is not None:
            self.cfg['VIDEO_BITRATE_KBPS'].set(str(self._br_suggest))

    def set_values(self, values):
        """Fill the form from a KEY -> value dict (missing keys: defaults)."""
        d = default_values(); d.update(values)
        if 'LED' not in values and values.get('LED_MODE', '').lower() == 'off':   # older config files
            d['LED'] = 'off'
        for _, key, _, kind, extra in SETTINGS:
            if key == 'RESOLUTION':
                self.cfg[key].set('%sx%s' % (d.get('VIDEO_WIDTH', '1920'), d.get('VIDEO_HEIGHT', '1080')))
            elif kind == 'bool':
                self.cfg[key].set(d.get(key, extra[1]).lower() not in (extra[1], '0', 'off', 'no', 'false', ''))
            elif kind == 'tz':
                self.cfg[key].set(TZ_LABEL.get(d.get(key, ''), d.get(key, '')))
            else:
                self.cfg[key].set(d.get(key, ''))
        known = {k for _, k, _, _, _ in SETTINGS} | {'VIDEO_WIDTH', 'VIDEO_HEIGHT', 'LED_MODE', 'LED_STREAM', 'BATTERY_CHARGE_LIMIT',
                                                     'LED_STATUS', 'LED_POWER', 'LED_INVERT',
                                                     'WIFI_POWER_SAVE', 'STARTUP_SOUND'}   # removed settings: dropped
        self.extra_cfg = {k: v for k, v in values.items()       # kept when writing
                          if k not in known and not k.startswith('LAMAXTOOL_')}
        if values.get('LAMAXTOOL_CONNECTION') in ('usb', 'wifi'):
            self.conn.set(values['LAMAXTOOL_CONNECTION'])
        if values.get('LAMAXTOOL_CAMERA_IP'):
            self.cam_ip.set(values['LAMAXTOOL_CAMERA_IP'])
        if values.get('LAMAXTOOL_MIC_GAIN') in MIC_GAINS:
            self.mic_gain.set(values['LAMAXTOOL_MIC_GAIN'])

    def form_values(self):
        """Current form content as KEY -> value (not validated)."""
        v = dict(self.extra_cfg)
        for _, key, _, kind, extra in SETTINGS:
            var = self.cfg[key]
            if key == 'RESOLUTION':
                w, _, h = var.get().partition('x')
                v['VIDEO_WIDTH'], v['VIDEO_HEIGHT'] = w, h
            elif kind == 'bool':
                v[key] = extra[0] if var.get() else extra[1]
            elif kind == 'tz':          # "Name  [POSIX]" from the list, or a typed POSIX string
                m = re.search(r'\[(.+)\]\s*$', var.get())
                v[key] = (m.group(1) if m else var.get()).strip()
            else:
                v[key] = var.get().strip()
        return v

    def config_values(self):
        v = self.form_values()
        validate(v)
        self.remember_now(v)
        return v

    def remember_now(self, values=None):
        """Store the form for the next start."""
        v = dict(values if values is not None else self.form_values())
        v['LAMAXTOOL_CONNECTION'] = self.conn.get()
        v['LAMAXTOOL_CAMERA_IP'] = self.cam_ip.get().strip()
        v['LAMAXTOOL_MIC_GAIN'] = self.mic_gain.get()
        try:
            save_last_settings(v)
        except OSError as e:
            self.log('Could not store the settings: %s' % e)

    def on_close(self):
        if self.talker:
            self.talker.stop()
        self.remember_now()
        self.after_cancel(self._pump_id)
        self.destroy()

    def do_defaults(self):
        self.set_values({})
        self.cfg_source.set('defaults')

    def load_text(self, text, source):
        vals = parse_config(text)
        if not vals:
            raise RuntimeError('No KEY=VALUE settings found in %s' % source)
        self.set_values(vals)
        self.cfg_source.set('loaded: %s' % source)
        self.log('Settings loaded from %s (%d keys)' % (source, len(vals)))

    def do_load_file(self):
        path = filedialog.askopenfilename(title='Load config.txt',
                                          filetypes=[('config files', '*.txt'), ('all files', '*')])
        if path:
            try:
                self.load_text(open(path, encoding='utf-8', errors='replace').read(), os.path.basename(path))
            except Exception as e:
                messagebox.showerror(APP, str(e))

    def do_load_camera(self):
        def task(adb):
            if isinstance(adb, CamNet):
                text = adb.read_config()
                self.q.put(lambda: self._load_or_error(text, 'camera %s (WiFi)' % adb.serial))
                return
            if ' /mnt/extsd ' not in adb.sh('cat /proc/mounts'):
                raise RuntimeError('The SD card is not mounted on the camera (in USB mass storage mode the '
                                   'card belongs to the PC: use "Load from file..." with the drive instead).')
            text = adb.sh('cat /mnt/extsd/cam/config.txt 2>/dev/null')
            self.q.put(lambda: self._load_or_error(text, 'camera %s' % adb.serial))
        self.run_task('Load settings from the camera', task)

    def _load_or_error(self, text, source):
        try:
            self.load_text(text, source)
        except Exception as e:
            messagebox.showerror(APP, str(e))

    def do_save_file(self):
        try:
            text = make_config(self.config_values())
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        path = filedialog.asksaveasfilename(title='Save config.txt', initialfile='config.txt',
                                            defaultextension='.txt', filetypes=[('config files', '*.txt')])
        if path:
            open(path, 'w', encoding='utf-8', newline='').write(text)
            self.log('Settings saved to %s' % path)

    # ---- actions
    def refresh(self):
        if self.busy:
            return
        if self.wifi():
            ip = self.cam_ip.get().strip()
            if not is_ipv4(ip) or not self.cfg['ADMIN_PASSWORD'].get():
                self.status.set('WiFi: enter the camera IP above and the admin password on the 🌐 Remote tab')
                return
            net = self.camnet()
            self.status.set('WiFi: connecting to %s ...' % net.serial)

            def worker():
                try:
                    st = parse_config(net.get('/api/status', timeout=5))
                    self._net_pw_ok = net.password
                    res = re.search(r'res=(\S+)', st.get('STREAM', ''))
                    on_ext = st.get('USB_POWER') == '1' or st.get('AC_POWER') == '1'
                    up = float((st.get('UPTIME') or '0').split()[0])
                    msg = 'WiFi: %s  %s   stream %s   battery %s%%%s   up %d min' % (
                        st.get('NAME', ''), net.host, res.group(1) if res else '-',
                        st.get('BATTERY_PCT', '?'), ' (charger)' if on_ext else '', up // 60)
                except Exception as e:
                    msg = 'WiFi: %s' % str(e).splitlines()[0]
                self.q.put(lambda: self.status.set(msg))
            threading.Thread(target=worker, daemon=True).start()
            return
        if not self.adb_exe:
            if messagebox.askyesno(APP, 'adb (Android platform-tools) was not found.\n\n'
                                        'Download it from Google now (about 7 MB) next to this program?'):
                self.run_task('Download adb', lambda _: setattr(self, 'adb_exe', download_platform_tools(self.log)),
                              need_device=False)
            else:
                self.status.set('adb not found')
            return
        try:
            devs = Adb(self.adb_exe).devices()
        except Exception as e:
            self.status.set('adb error: %s' % e); return
        self.dev_box['values'] = devs
        if devs and self.serial.get() not in devs:
            self.serial.set(devs[0])
        if not devs:
            self.serial.set('')
            self.status.set('no camera - connect it over USB (mass storage mode)')
            return
        try:
            a = Adb(self.adb_exe, self.serial.get())
            hook = 'yes' in a.sh('[ -f /init.camsd.sh ] && echo yes || echo no', timeout=10)
            sd = ' /mnt/extsd ' in a.sh('cat /proc/mounts', timeout=10)
            bk = latest_backup(self.serial.get(), original_only=False)
            self.status.set('hook: %s   SD mounted: %s   backup: %s' % (
                'installed' if hook else 'not installed', 'yes' if sd else 'no', 'yes' if bk else 'none'))
        except Exception as e:
            self.status.set('camera not responding: %s' % e)

    def do_backup(self):
        self.run_task('Back up flash', lambda adb: backup(adb, adb.serial, self.log), usb_only=True)

    def target_name(self):
        return ('%s (WiFi)' % self.cam_ip.get().strip()) if self.wifi() else self.serial.get()

    def do_install(self):
        if messagebox.askokcancel(APP, 'Install the SD-card boot hook on camera %s?\n\n'
                                       'The boot partition is rewritten (verified by read-back, the original is '
                                       'restored automatically if verification fails). Do not unplug or switch off '
                                       'the camera while it runs; over WiFi a dropped connection does not stop it.'
                                       % self.target_name()):
            self.run_task('Install boot hook', lambda adb: install_hook(adb, adb.serial, self.log), usb_only=True)

    def do_restore(self):
        if messagebox.askokcancel(APP, 'Write the original boot partition back to camera %s?\n\n'
                                       'Taken from this camera\'s own backup, or from ORIGINAL_FW - only if its '
                                       'system partition is identical to the camera\'s (checked first).'
                                  % self.target_name()):
            self.run_task('Restore original boot', lambda adb: restore_boot(adb, adb.serial, self.log), usb_only=True)

    def do_reboot(self):
        def task(adb):
            if isinstance(adb, CamNet):
                adb.post('/api/reboot')
                self.log('Camera is rebooting (back on WiFi in about 30 s).')
            else:
                adb.run('reboot', check=False)
                self.log('Camera is rebooting.')
        self.run_task('Reboot', task)

    def toggle_talk(self):
        """Intercom: laptop microphone -> camera speaker (over WiFi, admin password)."""
        if self.talker:
            self.talker.stop()
            self.talker = None
            self.talk_btn.configure(text='🎤 Talk')
            self.status.set('Talking stopped')
            self.log('Intercom: stopped')
            return
        if not self.wifi():
            messagebox.showinfo(APP, 'Talking to the camera works over WiFi: choose "📶 WiFi" at the top, enter '
                                     'the camera IP and its admin password (🌐 Remote tab).')
            return
        net = self.camnet()
        if not net:
            return
        self.talk_btn.configure(text='… connecting')

        def level(v):
            bars = int(min(1.0, v * 6) * 10)
            self.q.put(lambda: self.talker and self.status.set('🎤 Talking  ' + '▮' * bars + '▯' * (10 - bars)))

        g = self.mic_gain.get()
        gain_db = None if g == 'Auto' else float(g.split()[0])

        def worker():
            t = Talker(net, on_level=level, gain_db=gain_db)
            try:
                t.start()
            except Exception as e:
                msg = str(e)
                self.log('Intercom: %s' % msg)
                self.q.put(lambda: (self.talk_btn.configure(text='🎤 Talk'), messagebox.showerror(APP, msg)))
                return
            self._net_pw_ok = net.password
            self.log('Intercom: talking to %s (the microphone of this computer -> camera speaker)' % net.host)

            def started():
                self.talker = t
                self.talk_btn.configure(text='⏹ Stop talking')
                self.after(500, watch)
            self.q.put(started)

        def watch():                     # connection lost on the way
            if self.talker and not self.talker.running:
                self.toggle_talk()
            elif self.talker:
                self.after(500, watch)
        threading.Thread(target=worker, daemon=True).start()

    def open_backups(self):
        """Show the backups folder. On Windows explorer.exe directly: os.startfile (the
        "open" verb of folders) silently does nothing on some PCs."""
        try:
            os.makedirs(BACKUPS, exist_ok=True)
            if WINDOWS:
                subprocess.Popen(['explorer.exe', os.path.normpath(BACKUPS)])
            else:
                subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', BACKUPS])
            self.log('Backups folder: %s' % BACKUPS)
        except OSError as e:
            messagebox.showerror(APP, 'Could not open the backups folder:\n%s\n\n%s' % (BACKUPS, e))

    def do_wifi_scan(self):
        shown = [False]

        def update(nets, final):
            items, mapping = [], {}
            for n in nets:
                note = '' if n['band'] in ('2.4', '?') else '   (%s GHz - not usable)' % n['band']
                text = '%s   %d dBm%s' % (n['ssid'], n['dbm'], note)
                items.append(text)
                mapping[text] = n['ssid']
            if final:
                self.log('%d networks seen by the camera (2.4 GHz only):' % len(nets))
                for t in items:
                    self.log('  ' + t)

            def show():
                self.ssid_map = mapping
                self.ssid_box['values'] = items
                self.status.set('' if final else 'Scanning WiFi... %d networks so far' % len(items))
                try:        # refresh an open drop-down list in place
                    self.ssid_box.tk.call('ttk::combobox::ConfigureListbox', str(self.ssid_box))
                except tk.TclError:
                    pass
                if items and not shown[0]:
                    shown[0] = True
                    self.ssid_box.focus_set()
                    self.ssid_box.event_generate('<Down>')     # drop the list down
            self.q.put(show)
        self.run_task('Scan WiFi with the camera', lambda adb: scan_wifi_camera(adb, update))

    def do_rtsp_link(self):
        def task(adb):
            link = rtsp_link(adb)
            self.log('Stream: %s' % re.sub(r'(//[^:@/]+:)[^@]+@', r'\1***@', link))   # no password in the log
            ip = re.search(r'(\d+\.\d+\.\d+\.\d+)', link).group(1)
            self.q.put(lambda: (self.rtsp_link.set(link), self.cam_ip.set(ip)))   # also the WiFi address
        self.run_task('Get the stream link from the camera', task)

    def copy_rtsp_link(self):
        link = self.rtsp_link.get()
        if link:
            self.clipboard_clear()
            self.clipboard_append(link)
            self.status.set('Stream link copied to the clipboard')

    def on_ssid_selected(self, _event=None):
        text = self.ssid_box.get()
        self.cfg['WIFI_SSID'].set(self.ssid_map.get(text, text))

    def do_sd_adb(self):
        try:
            text = make_config(self.config_values())
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        if not self.wifi():
            self.run_task('Write SD card over adb', lambda adb: write_sd_adb(adb, text, self.log))
            return

        def task(net):
            # over SSH: programs + config, exactly like USB. A camera with older
            # programs has no SSH yet: then only config.txt, through the config service.
            try:
                ssh = CamSsh(net.host, net.passwords)
            except RuntimeError as e:
                ssh = None
                self.log('SSH not available (%s) - writing only config.txt.' % str(e).splitlines()[0])
            if ssh:
                try:
                    write_sd_adb(ssh, text, self.log)
                finally:
                    ssh.close()
            else:
                net.write_config(text)
                self.log('config.txt written over WiFi, encrypted (the programs: over USB, '
                         'or over WiFi once the camera has the programs with SSH).')

            def ask():
                if messagebox.askyesno(APP, 'Settings saved on the camera. They take effect after a restart.\n\n'
                                            'Restart the camera now?\n\n(If the WiFi settings are wrong, it will not '
                                            'come back on WiFi - then fix them over USB.)'):
                    self.after(300, self.do_reboot)
            self.q.put(ask)
        self.run_task('Write the camera SD card over WiFi', task)

    def do_sd_folder(self):
        try:
            text = make_config(self.config_values())
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        root = filedialog.askdirectory(title='Select the SD card drive (its root folder)')
        if root:
            self.run_task('Write SD card to %s' % root, lambda _: write_sd_folder(root, text, self.log),
                          need_device=False)


if __name__ == '__main__':
    App().mainloop()
