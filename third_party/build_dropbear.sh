#!/bin/bash
# Build the Dropbear SSH server for the camera (Android 4.2 bionic, armv7):
# one binary, dropbearmulti = dropbear (server) + dropbearkey + scp.
# Password login only, against the camera's ADMIN_PASSWORD: bionic has no crypt() and
# no /etc/shadow, so the password check is patched to compare with /dev/cam/ssh.pw
# (written by autorun.sh, readable by root only). Output: sdcard/cam/bin/dropbearmulti
set -e
cd "$(dirname "$0")"
ROOT=$(cd .. && pwd)
NDK=${NDK:-$HOME/lamax/toolchain/android-ndk-r21e}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
VER=2026.94
SHA=e098034a843699200c8c977a991fff73159735bf795d5f72ef672c41a6b1ae81
WORK=$HOME/lamax/build/dropbear
mkdir -p "$WORK" && cd "$WORK"
[ -f dropbear-$VER.tar.bz2 ] || curl -sSfLO https://matt.ucc.asn.au/dropbear/releases/dropbear-$VER.tar.bz2
echo "$SHA  dropbear-$VER.tar.bz2" | sha256sum -c -
rm -rf dropbear-$VER && tar xjf dropbear-$VER.tar.bz2
cd dropbear-$VER

# --- options --------------------------------------------------------------------
cat > localoptions.h <<'EOF'
/* LAMAX camera build */
#define DSS_PRIV_FILENAME "/mnt/extsd/cam/ssh/dss_host_key"
#define RSA_PRIV_FILENAME "/mnt/extsd/cam/ssh/rsa_host_key"
#define ECDSA_PRIV_FILENAME "/mnt/extsd/cam/ssh/ecdsa_host_key"
#define ED25519_PRIV_FILENAME "/mnt/extsd/cam/ssh/ed25519_host_key"
#define DEFAULT_PATH "/dev/cam:/system/bin:/system/xbin"
#define DEFAULT_ROOT_PATH "/dev/cam:/system/bin:/system/xbin"
#define COMPAT_USER_SHELLS "/system/bin/sh"
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 0
#define DROPBEAR_SK_KEYS 0
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_X11FWD 0
#define INETD_MODE 0
#define DO_MOTD 0
#define DROPBEAR_DSS 0
#define LAMAX_PASSWORD_FILE "/dev/cam/ssh.pw"
/* terminals: the camera has /dev/ptmx + devpts, which configure cannot see when cross
 * compiling (grantpt() for API < 21 comes from the NDK's legacy inlines) */
#define USE_DEV_PTMX 1
EOF

# --- password check: compare with LAMAX_PASSWORD_FILE instead of crypt() ----------
python3 - <<'EOF'
import re
p = 'src/svr-authpasswd.c'
s = open(p).read()
helper = r'''
/* LAMAX camera: no crypt()/shadow on Android - the password is the camera's admin
 * password, kept by autorun.sh in LAMAX_PASSWORD_FILE (root only). Constant-time
 * compare; a wrong password costs one more second. */
#include <fcntl.h>
static int lamax_password_ok(const char *pw, unsigned int len) {
	char want[256];
	int fd = open(LAMAX_PASSWORD_FILE, O_RDONLY);
	if (fd < 0) return 0;
	ssize_t n = read(fd, want, sizeof(want) - 1);
	close(fd);
	if (n <= 0) return 0;
	while (n > 0 && (want[n - 1] == '\n' || want[n - 1] == '\r')) n--;
	want[n] = 0;
	int ok = n > 0 && (unsigned int)n == len && constant_time_memcmp(pw, want, len) == 0;
	m_burn(want, sizeof(want));
	if (!ok) sleep(1);
	return ok;
}

void svr_auth_password(int valid_user) {'''
assert 'void svr_auth_password(int valid_user) {' in s
s = s.replace('void svr_auth_password(int valid_user) {', helper, 1)
old = 'testcrypt = crypt(password, passwdcrypt);'
assert old in s
s = s.replace(old, 'passwdcrypt = "ok";\n\t\ttestcrypt = lamax_password_ok(password, passwordlen) ? "ok" : "no";')
open(p, 'w').write(s)

# --- keep Android's environment in sessions: dropbear clears it, but getprop needs
#     ANDROID_PROPERTY_WORKSPACE (the property area fd is inherited) and Android tools
#     the other ANDROID_* / class path variables
p = 'src/svr-chansession.c'
s = open(p).read()
keep = ('"ANDROID_PROPERTY_WORKSPACE", "ANDROID_ROOT", "ANDROID_DATA", "ANDROID_ASSETS", '
        '"BOOTCLASSPATH", "LD_LIBRARY_PATH", "EXTERNAL_STORAGE"')
a = '\tchar *envcp = getenv("LANG");\n'
assert a in s
s = s.replace(a, a + '\tstatic const char *lamax_keep[] = { %s };\n'
                     '\tchar *lamax_val[sizeof(lamax_keep) / sizeof(lamax_keep[0])];\n'
                     '\tfor (unsigned int k = 0; k < sizeof(lamax_keep) / sizeof(lamax_keep[0]); k++) {\n'
                     '\t\tconst char *v = getenv(lamax_keep[k]);\n'
                     '\t\tlamax_val[k] = v ? m_strdup(v) : NULL;\n'
                     '\t}\n' % keep, 1)
b = '\taddnewvar("SHELL", get_user_shell());\n'
assert b in s
s = s.replace(b, b + '\tfor (unsigned int k = 0; k < sizeof(lamax_keep) / sizeof(lamax_keep[0]); k++)\n'
                     '\t\tif (lamax_val[k]) addnewvar(lamax_keep[k], lamax_val[k]);\n', 1)
open(p, 'w').write(s)

# ...and do not close the property area fd ("fd,size" in ANDROID_PROPERTY_WORKSPACE)
# when dropbear closes the inherited descriptors before starting the shell
p = 'src/dbutil.c'
s = open(p).read()
c = '\tfor (i = 3; i <= maxfd; i++) {\n\t\tm_close(i);'
assert c in s
s = s.replace(c, '\tconst char *lamax_pw = getenv("ANDROID_PROPERTY_WORKSPACE");\n'
                 '\tint lamax_keepfd = lamax_pw ? atoi(lamax_pw) : -1;\n'
                 '\tfor (i = 3; i <= maxfd; i++) {\n'
                 '\t\tif ((int)i == lamax_keepfd) continue;\n'
                 '\t\tm_close(i);', 1)
open(p, 'w').write(s)
EOF

# --- configure + build -------------------------------------------------------------
export CC="$TC/armv7a-linux-androideabi17-clang"
export CFLAGS="-O2 -march=armv7-a -mfpu=neon -mfloat-abi=softfp -fPIE -DHAVE_CRYPT=1"
export LDFLAGS="-pie"
./configure --host=arm-linux-androideabi --disable-zlib --disable-lastlog --disable-utmp \
    --disable-utmpx --disable-wtmp --disable-wtmpx --disable-pututline --disable-pututxline \
    --disable-shadow --disable-pam --enable-bundled-libtom >/dev/null
make -j4 PROGRAMS="dropbear dropbearkey scp" MULTI=1 >/dev/null
"$TC/llvm-strip" dropbearmulti
mkdir -p "$ROOT/sdcard/cam/bin"
cp dropbearmulti "$ROOT/sdcard/cam/bin/"
ls -l "$ROOT/sdcard/cam/bin/dropbearmulti"
