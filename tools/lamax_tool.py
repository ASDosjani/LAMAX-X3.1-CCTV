"""LAMAX X3.1 camera tool (GUI for Windows and Linux).

Per camera, over USB/adb:
  1. Back up the whole NOR flash (all 8 MTD partitions, each read twice and compared)
  2. Install the SD-card boot hook (built from THIS camera's own backup), verified by read-back
  3. Restore the original boot partition from the backup
  4. Write the SD card files (programs + config.txt) to the camera or to a drive / folder
The WiFi name can be picked from the networks the camera itself sees (once our software runs on it).
Run from source:  python tools/lamax_tool.py   (Linux: python3, needs python3-tk and adb)
Build: tools/build_exe.ps1 (Windows exe), tools/build_linux.sh (Linux binary)
"""
import datetime, hashlib, io, os, queue, re, shutil, subprocess, sys, tempfile, threading, urllib.request, zipfile
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP = 'LAMAX X3.1 camera tool'
# resource dir (PyInstaller unpacks data here) and the folder next to the exe / repo root
RES = getattr(sys, '_MEIPASS', os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HOME = os.path.dirname(sys.executable) if getattr(sys, 'frozen', False) else RES
sys.path.insert(0, os.path.join(RES, 'boot'))
import make_boot  # noqa: E402

SD_SRC = os.path.join(RES, 'sdcard', 'cam')
BACKUPS = os.path.join(HOME, 'backups')
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
    folder = latest_backup(serial)
    if not folder:
        raise RuntimeError('No backup of the original firmware of this camera (%s) - make a backup '
                           'first (before installing the hook).' % serial)
    orig = open(os.path.join(folder, 'mtdblock1_boot.bin'), 'rb').read()
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
    folder = latest_backup(serial)
    if not folder:
        raise RuntimeError('No backup of the original firmware of this camera (%s).' % serial)
    orig = open(os.path.join(folder, 'mtdblock1_boot.bin'), 'rb').read()
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
    ('Video', 'RESOLUTION', 'Resolution', 'choice', ['1920x1080', '1280x720']),
    ('Video', 'VIDEO_FPS', 'Frames per second', 'int', (1, 30)),
    ('Video', 'VIDEO_BITRATE_KBPS', 'Bitrate (kbit/s)', 'int', (200, 12000)),
    ('Video', 'VIDEO_GOP_SECONDS', 'Key frame every (s)', 'int', (1, 10)),
    ('Video', 'VIDEO_PROFILE', 'H.264 profile', 'choice', ['baseline', 'main', 'high']),
    ('Video', 'VIDEO_FLIP', 'Rotate picture 180°', 'bool', ('1', '0')),
    ('Time', 'OSD_TIME', 'Date and time in the picture', 'bool', ('1', '0')),
    ('Time', 'OSD_NAME', 'Camera name before the time', 'bool', ('1', '0')),
    ('Time', 'OSD_POSITION', 'Position', 'choice', ['top-left', 'top-right', 'bottom-left', 'bottom-right']),
    ('Time', 'NTP_SERVER', 'Time server (NTP)', 'text', None),
    ('Time', 'TIMEZONE', 'Time zone', 'tz', None),
    ('RTSP', 'RTSP_PORT', 'RTSP port', 'int', (1, 65535)),
    ('RTSP', 'RTSP_PATH', 'Stream path', 'text', None),
    ('RTSP', 'RTSP_USER', 'User name (empty = no auth)', 'text', None),
    ('RTSP', 'RTSP_PASSWORD', 'Password', 'password', None),
    ('RTSP', 'RTSP_MAX_CLIENTS', 'Max. clients', 'int', (1, 8)),
    ('Device', 'MODE', 'Mode', 'choice', ['camera', 'stock']),
    ('Device', 'SCREEN_TIMEOUT', 'Screen off after (s, 0 = never)', 'int', (0, 3600)),
    ('Device', 'LED', 'Status LEDs', 'bool', ('on', 'off')),
    ('Device', 'BATTERY_LONG_LIFE', 'Battery long life (charge to ~85-90 %)', 'bool', ('1', '0')),
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
                     'VIDEO_WIDTH': '1920', 'VIDEO_HEIGHT': '1080', 'VIDEO_FPS': '25',
                     'VIDEO_BITRATE_KBPS': '4000', 'VIDEO_GOP_SECONDS': '2', 'VIDEO_PROFILE': 'main',
                     'VIDEO_FLIP': '0', 'RTSP_PORT': '554', 'RTSP_PATH': 'stream', 'RTSP_MAX_CLIENTS': '4',
                     'MODE': 'camera', 'SCREEN_TIMEOUT': '60', 'LED': 'on', 'BATTERY_LONG_LIFE': '1',
                     'WATCHDOG': '1', 'WIFI_DEBUG_LOG': '0', 'LOG': 'sd', 'OSD_TIME': '1', 'OSD_NAME': '0',
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


def write_sd_adb(adb, config_text, overwrite_config, log):
    mounts = adb.sh('cat /proc/mounts')
    if ' /mnt/extsd ' not in mounts:
        raise RuntimeError('The SD card is not mounted on the camera. If the camera shows the USB mode '
                           'question, the card is lent to the PC: use "Write to drive" instead, or insert a card.')
    adb.sh('mkdir -p /mnt/extsd/cam/bin')
    for rel, src in sd_files():
        adb.push(src, '/mnt/extsd/cam/' + rel)
        log('  cam/' + rel)
    exists = 'yes' in adb.sh('[ -f /mnt/extsd/cam/config.txt ] && echo yes || echo no')
    if exists and not overwrite_config:
        log('  cam/config.txt exists - kept')
    else:
        tmp = tempfile.mkdtemp()
        try:
            p = os.path.join(tmp, 'config.txt')
            open(p, 'w', encoding='utf-8', newline='').write(config_text)
            adb.push(p, '/mnt/extsd/cam/config.txt')
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        log('  cam/config.txt written')
    adb.sh('sync')
    log('SD card ready.')


def write_sd_folder(root, config_text, overwrite_config, log):
    dst = os.path.join(root, 'cam')
    os.makedirs(os.path.join(dst, 'bin'), exist_ok=True)
    for rel, src in sd_files():
        shutil.copyfile(src, os.path.join(dst, rel.replace('/', os.sep)))
        log('  cam/' + rel)
    cfg = os.path.join(dst, 'config.txt')
    if os.path.exists(cfg) and not overwrite_config:
        log('  cam/config.txt exists - kept')
    else:
        open(cfg, 'w', encoding='utf-8', newline='').write(config_text)
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


def scan_wifi_camera(adb):
    """Networks the camera's own radio sees (needs our software running on it),
    one entry per name with the strongest signal, strongest first."""
    w = '/dev/cam/wpa_cli -p /dev/cam/wpa'
    if 'yes' not in adb.sh('[ -S /dev/cam/wpa/wlan0 ] && echo yes || echo no', timeout=10):
        raise RuntimeError('The camera is not running the security camera software yet.\n'
                           'Install the boot hook, write the SD card, restart the camera and connect it '
                           'over USB again - or type the network name.')
    out = adb.sh('%s scan >/dev/null; sleep 8; %s scan_results' % (w, w), timeout=30)
    nets = []
    for line in out.splitlines():
        f = line.split('\t')
        if len(f) >= 4 and re.match(r'^[0-9a-f:]{17}$', f[0]):
            dbm = int(f[2])
            nets.append(dict(ssid=f[4] if len(f) > 4 else '', signal=max(0, min(100, 2 * (dbm + 100))),
                             band=_band_from_mhz(int(f[1])), dbm=dbm))
    best = {}
    for n in nets:
        if n['ssid'] and (n['ssid'] not in best or n['signal'] > best[n['ssid']]['signal']):
            best[n['ssid']] = n
    return sorted(best.values(), key=lambda n: -n['signal'])


# ----------------------------------------------------------------------------- GUI

TAB_ICONS = {'Network': '📶 ', 'Video': '🎥 ', 'Time': '🕒 ', 'RTSP': '📡 ',
             'Device': '⚙ '}

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
        self.dev_box = ttk.Combobox(top, textvariable=self.serial, width=22, state='readonly')
        self.dev_box.pack(side='left', padx=6)
        ttk.Button(top, text='⟳ Refresh', command=self.refresh, style='Tool.TButton').pack(side='left')
        ttk.Label(top, textvariable=self.status, style='Hint.TLabel').pack(side='left', padx=10)

        # ---- 1: flash (one time per camera)
        flash = ttk.LabelFrame(self, text=' 1 │ Camera flash  (once per camera) ', style='Section.TLabelframe',
                               padding=(6, 4)); flash.pack(fill='x', **pad)
        for text, fn in (('💾 Back up flash', self.do_backup), ('🔧 Install boot hook', self.do_install),
                         ('↩ Restore original boot', self.do_restore), ('⏻ Reboot camera', self.do_reboot),
                         ('📁 Backups folder', self.open_backups)):
            button(flash, text, fn, padx=(0, 6), pady=2)

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
            elif kind == 'int':
                ttk.Spinbox(f, textvariable=v, from_=extra[0], to=extra[1], width=24).grid(**cell)
            else:
                ttk.Entry(f, textvariable=v, width=26, show='*' if kind == 'password' else '').grid(**cell)
        ttk.Label(sd, text='The last used settings are remembered in LamaxTool_settings.txt next to the program (passwords included).',
                  style='Hint.TLabel').pack(anchor='w')
        last = load_last_settings()
        if last is not None:
            self.set_values(last)
            self.cfg_source.set('last used settings')
        else:
            self.set_values(default_values())

        # ---- 3: write the SD card
        wr = ttk.LabelFrame(self, text=' 3 │ Write to SD card  (programs + config.txt) ', style='Section.TLabelframe',
                            padding=(6, 6)); wr.pack(fill='x', **pad)
        button(wr, '⬆  Write to camera SD card (USB)', self.do_sd_adb, style='Big.TButton', padx=(0, 6))
        button(wr, '🖴  Write to drive / folder...', self.do_sd_folder, style='Big.TButton', padx=(0, 6))
        self.overwrite = tk.BooleanVar(value=False)
        ttk.Checkbutton(wr, text='Overwrite existing config.txt', variable=self.overwrite).pack(side='left', padx=8)
        self.protocol('WM_DELETE_WINDOW', self.on_close)

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

    def run_task(self, title, fn, need_device=True):
        if self.busy:
            return
        if need_device and not self.serial.get():
            messagebox.showwarning(APP, 'No camera selected. Connect it over USB and press Refresh.')
            return
        self.set_busy(True)
        self.log('--- %s' % title)
        serial = self.serial.get() or None      # Tk variables are read in the GUI thread only

        def worker():
            try:
                adb = Adb(self.adb_exe, serial) if self.adb_exe else None
                fn(adb)
                self.log('--- done')
            except Exception as e:      # shown to the user, never fatal
                msg = str(e)            # `e` is deleted when the except block ends
                self.log('ERROR: %s' % msg)
                self.q.put(lambda: messagebox.showerror(APP, msg))
            finally:
                self.q.put(lambda: self.set_busy(False))
                self.q.put(self.refresh)
        threading.Thread(target=worker, daemon=True).start()

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
        self.extra_cfg = {k: v for k, v in values.items() if k not in known}   # kept when writing

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
        try:
            save_last_settings(values if values is not None else self.form_values())
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
        self.run_task('Back up flash', lambda adb: backup(adb, adb.serial, self.log))

    def do_install(self):
        if messagebox.askokcancel(APP, 'Install the SD-card boot hook on camera %s?\n\n'
                                       'The boot partition is rewritten (verified by read-back, the original is '
                                       'restored automatically if verification fails). Do not unplug the camera '
                                       'while it runs.' % self.serial.get()):
            self.run_task('Install boot hook', lambda adb: install_hook(adb, adb.serial, self.log))

    def do_restore(self):
        if messagebox.askokcancel(APP, 'Write the original boot partition back from the backup of camera %s?'
                                  % self.serial.get()):
            self.run_task('Restore original boot', lambda adb: restore_boot(adb, adb.serial, self.log))

    def do_reboot(self):
        self.run_task('Reboot', lambda adb: (adb.run('reboot', check=False), self.log('Camera is rebooting.')))

    def open_backups(self):
        os.makedirs(BACKUPS, exist_ok=True)
        if WINDOWS:
            os.startfile(BACKUPS)
        else:
            subprocess.Popen(['open' if sys.platform == 'darwin' else 'xdg-open', BACKUPS])

    def do_wifi_scan(self):
        def task(adb):
            nets = scan_wifi_camera(adb)
            items, mapping = [], {}
            for n in nets:
                note = '' if n['band'] in ('2.4', '?') else '   (%s GHz - not usable)' % n['band']
                sig = '%d dBm' % n['dbm'] if 'dbm' in n else '%d%%' % n['signal']
                text = '%s   %s%s' % (n['ssid'], sig, note)
                items.append(text)
                mapping[text] = n['ssid']
            self.log('%d networks seen by the camera (2.4 GHz only):' % len(nets))
            for t in items:
                self.log('  ' + t)

            def show():
                self.ssid_map = mapping
                self.ssid_box['values'] = items
                if items:
                    self.ssid_box.focus_set()
                    self.ssid_box.event_generate('<Down>')     # drop the list down
            self.q.put(show)
        self.run_task('Scan WiFi with the camera', task)

    def on_ssid_selected(self, _event=None):
        text = self.ssid_box.get()
        self.cfg['WIFI_SSID'].set(self.ssid_map.get(text, text))

    def do_sd_adb(self):
        try:
            text = make_config(self.config_values())
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        ow = self.overwrite.get()
        self.run_task('Write SD card over adb', lambda adb: write_sd_adb(adb, text, ow, self.log))

    def do_sd_folder(self):
        try:
            text = make_config(self.config_values())
        except Exception as e:
            messagebox.showerror(APP, str(e)); return
        root = filedialog.askdirectory(title='Select the SD card drive (its root folder)')
        if root:
            self.run_task('Write SD card to %s' % root,
                          lambda _, ow=self.overwrite.get(): write_sd_folder(root, text, ow, self.log), need_device=False)


if __name__ == '__main__':
    App().mainloop()
