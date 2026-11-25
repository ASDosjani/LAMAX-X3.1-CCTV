"""Build the boot image with the SD-card hook.

usage: python make_boot.py <original mtdblock1_boot.bin> <out.img>

Takes the camera's original boot partition (Android boot image: kernel +
gzip'd newc cpio ramdisk), adds /init.camsd.sh and a `camsd` service to
init.rc, repacks everything with the original header values and checks that
the result still fits the 2.5 MB partition. The kernel is copied unchanged.

Also importable: build_hook_image(original_bytes) and has_hook(image_bytes)
are used by the Windows tool (tools/lamax_tool.py).
"""
import gzip, hashlib, io, os, struct, sys

PART_SIZE = 0x280000          # mtd1 "boot"
HERE = os.path.dirname(os.path.abspath(__file__))
INIT_RC_ADD = b"""
# --- LAMAX security camera: run cam/autorun.sh from the SD card if present ---
service camsd /system/bin/sh /init.camsd.sh
    class main
    user root
    group root
    oneshot
"""


def pad(n, page):
    return (page - n % page) % page


def parse_boot(d):
    if d[:8] != b'ANDROID!':
        raise ValueError('not an Android boot image')
    ksz, kaddr, rsz, raddr, ssz, saddr, tags, page = struct.unpack('<8I', d[8:40])
    o = page
    kernel = d[o:o + ksz]; o += ksz + pad(ksz, page)
    ramdisk = d[o:o + rsz]; o += rsz + pad(rsz, page)
    second = d[o:o + ssz]
    return dict(hdr=bytearray(d[:page]), page=page, kernel=kernel, ramdisk=ramdisk, second=second)


def boot_id(kernel, ramdisk, second):
    h = hashlib.sha1()
    for blob in (kernel, ramdisk, second):
        h.update(blob); h.update(struct.pack('<I', len(blob)))
    return h.digest()


def cpio_read(data):
    entries, o = [], 0
    while True:
        if data[o:o + 6] != b'070701':
            raise ValueError('bad cpio magic at %d' % o)
        f = [int(data[o + 6 + i * 8:o + 14 + i * 8], 16) for i in range(13)]
        namesize, filesize = f[11], f[6]
        name = data[o + 110:o + 110 + namesize - 1].decode()
        o += 110 + namesize; o += (4 - o % 4) % 4
        body = data[o:o + filesize]; o += filesize; o += (4 - o % 4) % 4
        if name == 'TRAILER!!!':
            return entries
        entries.append(dict(name=name, fields=f, body=body))


def cpio_write(entries):
    out = io.BytesIO()

    def put(name, f, body):
        nb = name.encode() + b'\0'
        f = list(f); f[6] = len(body); f[11] = len(nb)
        out.write(b'070701' + b''.join(b'%08X' % v for v in f) + nb)
        out.write(b'\0' * ((4 - out.tell() % 4) % 4))
        out.write(body)
        out.write(b'\0' * ((4 - out.tell() % 4) % 4))
    for i, e in enumerate(entries):
        f = list(e['fields']); f[0] = 300000 + i   # unique inode numbers
        put(e['name'], f, e['body'])
    put('TRAILER!!!', [0] * 13, b'')
    return out.getvalue()


def ramdisk_files(img):
    """name -> entry of the boot image's ramdisk"""
    b = parse_boot(img)
    return {e['name']: e for e in cpio_read(gzip.decompress(b['ramdisk']))}


def has_hook(img):
    """True if this boot image already contains the SD-card hook."""
    try:
        files = ramdisk_files(img)
    except Exception:
        return False
    return 'init.camsd.sh' in files and b'service camsd' in files['init.rc']['body']


def hook_script():
    path = os.path.join(HERE, 'init.camsd.sh')
    return open(path, 'rb').read().replace(b'\r\n', b'\n')


def build_hook_image(orig, log=print):
    """Return the hooked boot partition (exactly PART_SIZE bytes) built from
    the camera's original boot partition `orig`."""
    b = parse_boot(orig)
    stored_id = bytes(b['hdr'][576:596])
    log('original: kernel %d B, ramdisk %d B, page %d, id %s' % (
        len(b['kernel']), len(b['ramdisk']), b['page'],
        'OK' if stored_id == boot_id(b['kernel'], b['ramdisk'], b['second']) else 'differs (not checked by u-boot)'))

    entries = cpio_read(gzip.decompress(b['ramdisk']))
    names = [e['name'] for e in entries]
    rc = next(e for e in entries if e['name'] == 'init.rc')
    if b'service camsd' not in rc['body']:
        rc['body'] = rc['body'].rstrip(b'\n') + b'\n' + INIT_RC_ADD
    hook = hook_script()
    tmpl = list(rc['fields'])
    tmpl[1] = 0o100750                         # regular file rwxr-x---
    if 'init.camsd.sh' in names:
        next(e for e in entries if e['name'] == 'init.camsd.sh')['body'] = hook
    else:
        entries.append(dict(name='init.camsd.sh', fields=tmpl, body=hook))

    raw = cpio_write(entries)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode='wb', compresslevel=9, mtime=0) as g:
        g.write(raw)
    ramdisk = buf.getvalue()

    hdr = b['hdr']
    struct.pack_into('<I', hdr, 16, len(ramdisk))
    hdr[576:608] = boot_id(b['kernel'], ramdisk, b['second']) + b'\0' * 12
    page = b['page']
    img = bytes(hdr)
    for blob in (b['kernel'], ramdisk, b['second']):
        if blob:
            img += blob + b'\0' * pad(len(blob), page)
    log('new:      ramdisk %d B (+%d), image %d B of %d B partition (%d B free)' % (
        len(ramdisk), len(ramdisk) - len(b['ramdisk']), len(img), PART_SIZE, PART_SIZE - len(img)))
    if len(img) > PART_SIZE:
        raise ValueError('image does not fit the boot partition')

    # self-check: parse again, kernel identical, hook present, nothing lost
    c = parse_boot(img)
    assert c['kernel'] == b['kernel']
    chk = {e['name']: e for e in cpio_read(gzip.decompress(c['ramdisk']))}
    assert b'service camsd' in chk['init.rc']['body'] and 'init.camsd.sh' in chk
    assert set(chk) == set(names) | {'init.camsd.sh'}
    # pad to the full partition with the original trailing bytes, so a
    # whole-partition write leaves nothing undefined
    return img + orig[len(img):PART_SIZE] if len(orig) >= PART_SIZE else img


def main(src, dst):
    full = build_hook_image(open(src, 'rb').read())
    open(dst, 'wb').write(full)
    print('wrote %s (%d B), sha256 %s' % (dst, len(full), hashlib.sha256(full).hexdigest()))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
