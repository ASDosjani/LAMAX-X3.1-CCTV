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


class CamNet:
    """The camera over WiFi: camcfg's HTTP API (Digest login, user "admin",
    password ADMIN_PASSWORD). Only settings, reboot, WiFi scan and status -
    flashing needs USB."""

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
    ('Remote', 'ADMIN_PASSWORD', 'Admin password (empty = USB only)', 'password', None),
    ('Remote', 'ADMIN_PORT', 'Port', 'int', (1, 65535)),
    ('Device', 'MODE', 'Mode', 'choice', ['camera', 'stock']),
    ('Device', 'SCREEN_TIMEOUT', 'Screen off after (s, 0 = never)', 'int', (0, 3600)),
    ('Device', 'LED', 'Status LEDs', 'bool', ('on', 'off')),
    ('Device', 'STARTUP_SOUND', 'Startup sound when on the network', 'bool', ('1', '0')),
    ('Device', 'BATTERY_LONG_LIFE', 'Battery long life (charge to ~85-90 %, off at 10 %)', 'bool', ('1', '0')),
    ('Device', 'WATCHDOG', 'Reboot if the stream hangs', 'bool', ('1', '0')),
    ('Device', 'LOG', 'Log (sd / ram = not on the card / off)', 'choice', ['sd', 'ram', 'off']),
    ('Device', 'WIFI_DEBUG_LOG', 'Detailed WiFi log (on the card)', 'bool', ('1', '0')),
]
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
                     'ADMIN_PASSWORD': '', 'ADMIN_PORT': '8080', 'STARTUP_SOUND': '1',
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


SOUND_RATE, SOUND_MAX_S = 22050, 10


def convert_sound(path):
    """Any PCM WAV -> 16-bit mono 22050 Hz WAV bytes (at most 10 s), which the
    camera's startup_music player handles."""
    import array, wave
    try:
        w = wave.open(path, 'rb')
    except (wave.Error, EOFError) as e:
        raise RuntimeError('%s is not a PCM WAV file (%s). Convert it to WAV first, '
                           'e.g. with Audacity.' % (os.path.basename(path), e))
    with w:
        ch, sw, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        n = min(n, rate * SOUND_MAX_S)
        raw = w.readframes(n)
    n = len(raw) // (sw * ch)
    if sw == 1:
        s = [(b - 128) / 128.0 for b in raw]
    elif sw == 2:
        s = [v / 32768.0 for v in array.array('h', raw[:n * ch * 2])]
    elif sw in (3, 4):
        s = [int.from_bytes(raw[i:i + sw], 'little', signed=True) / float(1 << (8 * sw - 1))
             for i in range(0, n * ch * sw, sw)]
    else:
        raise RuntimeError('Unsupported WAV sample size: %d bytes' % sw)
    mono = [sum(s[i * ch:(i + 1) * ch]) / ch for i in range(n)]
    # as loud as the small speaker allows: normalise, then a soft limiter (tanh, about +9 dB)
    peak = max((abs(v) for v in mono), default=0) or 1.0
    import math
    drive = 3.0
    mono = [math.tanh(drive * v / peak) / math.tanh(drive) * 0.98 for v in mono]
    m = int(n * SOUND_RATE / rate)
    out = array.array('h')
    for i in range(m):
        x = i * rate / SOUND_RATE
        j = int(x)
        a = mono[j]
        b = mono[j + 1] if j + 1 < n else a
        v = a + (b - a) * (x - j)
        out.append(max(-32768, min(32767, int(round(v * 32767)))))
    buf = io.BytesIO()
    with wave.open(buf, 'wb') as o:
        o.setnchannels(1); o.setsampwidth(2); o.setframerate(SOUND_RATE)
        o.writeframes(out.tobytes())
    return buf.getvalue()


def sd_files():
    files = [('autorun.sh', os.path.join(SD_SRC, 'autorun.sh'))]
    bindir = os.path.join(SD_SRC, 'bin')
    for n in sorted(os.listdir(bindir)) if os.path.isdir(bindir) else []:
        files.append(('bin/' + n, os.path.join(bindir, n)))
    if len(files) < 6:
        raise RuntimeError('camera programs missing in %s (build them first, see BUILD.md)' % bindir)
    return files


def write_sd_adb(adb, config_text, log, sound=None):
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
        if sound:
            p = os.path.join(tmp, 'startup.wav')
            open(p, 'wb').write(sound)
            adb.push(p, '/mnt/extsd/cam/startup.wav')
            log('  cam/startup.wav')
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    log('  cam/config.txt written')
    adb.sh('sync')
    log('SD card ready.')


def write_sd_folder(root, config_text, log, sound=None):
    dst = os.path.join(root, 'cam')
    os.makedirs(os.path.join(dst, 'bin'), exist_ok=True)
    for rel, src in sd_files():
        shutil.copyfile(src, os.path.join(dst, rel.replace('/', os.sep)))
        log('  cam/' + rel)
    if sound:
        open(os.path.join(dst, 'startup.wav'), 'wb').write(sound)
        log('  cam/startup.wav')
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
        self.sound_path = tk.StringVar()               # WAV for cam/startup.wav (on this PC)
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
        # Device tab: own startup sound (converted and copied to cam/startup.wav when writing the SD card)
        sf = ttk.Frame(tabs['Device']); sf.grid(row=50, column=0, columnspan=4, sticky='we', padx=4, pady=(8, 2))
        ttk.Label(sf, text='Startup sound (WAV):').pack(side='left')
        ttk.Entry(sf, textvariable=self.sound_path, state='readonly', width=36).pack(side='left', padx=4)
        button(sf, '📂 Choose...', self.choose_sound)
        button(sf, '✖', lambda: self.sound_path.set(''), padx=(2, 0))
        self.usb_only_btns.append(button(sf, '▶ Play on camera (USB)', self.do_play_sound, padx=(6, 0)))
        ttk.Label(tabs['Device'], style='Hint.TLabel', text=
                  'Written to cam/startup.wav with the SD card (USB or card reader), played when the camera is on '
                  'the network.\nNo file chosen = the sound on the card stays. The original power-on sound stays too.'
                  ).grid(row=51, column=0, columnspan=4, sticky='w', padx=4)
        ttk.Label(tabs['Remote'], style='Hint.TLabel', justify='left', text=
                  'With an admin password the camera can be configured over WiFi: choose "📶 WiFi" at the top\n'
                  'and enter its IP. Read / write settings, WiFi scan, stream link and reboot then work without\n'
                  'a cable (user name: admin). Over WiFi only config.txt is written, not the programs;\n'
                  'flash backup and boot hook need USB. A new password takes effect after a restart.'
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
                                   % (' – needs 🔌 USB' if wifi else ''))
        self.write_frame.configure(text=' 3 │ Write to camera  (WiFi: only config.txt, encrypted) ' if wifi
                                   else ' 3 │ Write to SD card  (programs + config.txt) ')
        self.write_btn.configure(text='⬆  Write settings to camera (WiFi)' if wifi
                                 else '⬆  Write to camera SD card (USB)')
        if not self.busy:
            for b in self.usb_only_btns:
                b.state(['disabled'] if wifi else ['!disabled'])

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
        """fn(conn) in a worker thread; conn is an Adb (USB) or a CamNet (WiFi)."""
        if self.busy:
            return
        net = None
        if need_device and self.wifi():
            if usb_only:
                messagebox.showwarning(APP, '"%s" needs the camera on USB (choose "🔌 USB" at the top).' % title)
                return
            net = self.camnet()
            if not net:
                return
        elif need_device and not self.serial.get():
            messagebox.showwarning(APP, 'No camera selected. Connect it over USB and press Refresh '
                                        '(or choose "📶 WiFi" and enter its IP).')
            return
        self.set_busy(True)
        self.log('--- %s%s' % (title, ' (WiFi %s)' % net.serial if net else ''))
        serial = self.serial.get() or None      # Tk variables are read in the GUI thread only

        def worker():
            try:
                adb = net or (Adb(self.adb_exe, serial) if self.adb_exe else None)
                fn(adb)
                if net:
                    self._net_pw_ok = net.password      # remembered for this session only
                self.log('--- done')
            except Exception as e:      # shown to the user, never fatal
                msg = str(e)            # `e` is deleted when the except block ends
                self.log('ERROR: %s' % msg)
                self.q.put(lambda: messagebox.showerror(APP, msg))
            finally:
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
                                                     'LED_STATUS', 'LED_POWER', 'LED_INVERT'}
        self.extra_cfg = {k: v for k, v in values.items()       # kept when writing
                          if k not in known and not k.startswith('LAMAXTOOL_')}
        if values.get('LAMAXTOOL_CONNECTION') in ('usb', 'wifi'):
            self.conn.set(values['LAMAXTOOL_CONNECTION'])
        if values.get('LAMAXTOOL_CAMERA_IP'):
            self.cam_ip.set(values['LAMAXTOOL_CAMERA_IP'])
        if 'LAMAXTOOL_STARTUP_SOUND' in values:
            self.sound_path.set(values['LAMAXTOOL_STARTUP_SOUND'])

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
        v['LAMAXTOOL_STARTUP_SOUND'] = self.sound_path.get()
        try:
            save_last_settings(v)
        except OSError as e:
            self.log('Could not store the settings: %s' % e)

    def on_close(self):
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

    def do_install(self):
        if self.wifi():
            return self.run_task('Install boot hook', None, usb_only=True)
        if messagebox.askokcancel(APP, 'Install the SD-card boot hook on camera %s?\n\n'
                                       'The boot partition is rewritten (verified by read-back, the original is '
                                       'restored automatically if verification fails). Do not unplug the camera '
                                       'while it runs.' % self.serial.get()):
            self.run_task('Install boot hook', lambda adb: install_hook(adb, adb.serial, self.log))

    def do_restore(self):
        if self.wifi():
            return self.run_task('Restore original boot', None, usb_only=True)
        if messagebox.askokcancel(APP, 'Write the original boot partition back to camera %s?\n\n'
                                       'Taken from this camera\'s own backup, or from ORIGINAL_FW - only if its '
                                       'system partition is identical to the camera\'s (checked first).'
                                  % self.serial.get()):
            self.run_task('Restore original boot', lambda adb: restore_boot(adb, adb.serial, self.log))

    def do_reboot(self):
        def task(adb):
            if isinstance(adb, CamNet):
                adb.post('/api/reboot')
                self.log('Camera is rebooting (back on WiFi in about 30 s).')
            else:
                adb.run('reboot', check=False)
                self.log('Camera is rebooting.')
        self.run_task('Reboot', task)

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
        snd = self.sound_path.get()
        if not self.wifi():
            self.run_task('Write SD card over adb', lambda adb: write_sd_adb(
                adb, text, self.log, convert_sound(snd) if snd else None))
            return

        def task(net):
            net.write_config(text)
            self.log('config.txt written over WiFi, encrypted (the programs are only updated over USB '
                     'or a card reader).')
            if snd:
                self.log('Note: the startup sound is not sent over WiFi - write it over USB or a card reader.')

            def ask():
                if messagebox.askyesno(APP, 'Settings saved on the camera. They take effect after a restart.\n\n'
                                            'Restart the camera now?\n\n(If the WiFi settings are wrong, it will not '
                                            'come back on WiFi - then fix them over USB.)'):
                    self.after(300, self.do_reboot)
            self.q.put(ask)
        self.run_task('Write config.txt over WiFi', task)

    def do_sd_folder(self):
        try:
            text = make_config(self.config_values())
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        root = filedialog.askdirectory(title='Select the SD card drive (its root folder)')
        if root:
            snd = self.sound_path.get()
            self.run_task('Write SD card to %s' % root, lambda _: write_sd_folder(
                root, text, self.log, convert_sound(snd) if snd else None), need_device=False)

    def choose_sound(self):
        path = filedialog.askopenfilename(title='Startup sound', filetypes=[('WAV sound', '*.wav'), ('all files', '*')])
        if not path:
            return
        try:
            data = convert_sound(path)
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        self.sound_path.set(path)
        self.log('Startup sound: %s (%.1f s on the camera)' % (path, (len(data) - 44) / 2.0 / SOUND_RATE))

    def do_play_sound(self):
        """Play the chosen sound (or the one on the card) with the camera's own player."""
        snd = self.sound_path.get()

        def task(adb):
            if snd:
                tmp = tempfile.mkdtemp()
                try:
                    p = os.path.join(tmp, 'startup.wav')
                    open(p, 'wb').write(convert_sound(snd))
                    adb.push(p, '/dev/cam/sound_test.wav')
                finally:
                    shutil.rmtree(tmp, ignore_errors=True)
                f = '/dev/cam/sound_test.wav'
            else:
                f = '/mnt/extsd/cam/startup.wav'
            out = adb.sh('[ -f %s ] && { /system/bin/startup_music %s 2>&1 & i=0; while [ $i -lt 15 ]; do '
                         'tinymix -D 0 1 31 >/dev/null; i=$((i+1)); done; wait; }; rm -f /dev/cam/sound_test.wav'
                         % (f, f))     # the player sets Line Volume 29: back to the maximum 31
            if 'Error' in out:
                raise RuntimeError('The camera could not play the sound:\n' + out.strip())
            if 'Playing' not in out:
                raise RuntimeError('No sound file: choose one, or write the SD card with one first.')
            self.log('Played on the camera.')
        self.run_task('Play the startup sound on the camera', task, usb_only=True)


if __name__ == '__main__':
    App().mainloop()
