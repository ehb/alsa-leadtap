#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Selftest of the LibreELEC add-on: runs bin/alsa-leadtap against a fake add-on root with
# native (x86_64) builds, without root and without touching the real system, and checks
# build_addon.py with the aarch64 builds. Needs Linux, alsa-utils (aplay), libasound with
# the system alsa.conf, python3, unzip. Prints "ALL OK" only if every check passes.
#
#   LT_SO=<native plugin .so> LT_WLED=<native wled-audiosync> \
#   LT_SO_A64=<aarch64 plugin .so> LT_WLED_A64=<aarch64 wled-audiosync> \
#   LT_LICENSE_W=<LICENSE of wled-audiosync> \
#   LT_FAKESLAVE=<native libasound_module_pcm_fakeslave.so from make test> \
#       sh libreelec/test_addon.sh
set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
SCRIPT=$HERE/addon/bin/alsa-leadtap
: "${LT_FAKESLAVE:?set LT_FAKESLAVE}" "${LT_SO:?set LT_SO}" "${LT_WLED:?set LT_WLED}" "${LT_SO_A64:?set LT_SO_A64}" "${LT_WLED_A64:?set LT_WLED_A64}" "${LT_LICENSE_W:?set LT_LICENSE_W}"
LT_SO=$(readlink -f "$LT_SO"); LT_WLED=$(readlink -f "$LT_WLED")
LT_SO_A64=$(readlink -f "$LT_SO_A64"); LT_WLED_A64=$(readlink -f "$LT_WLED_A64")

T=$(mktemp -d /tmp/lt-addon.XXXXXX) || exit 1
PIDS=
cleanup() { for p in $PIDS; do kill "$p" 2> /dev/null; done; rm -rf "$T"; }
trap cleanup EXIT

FAILS=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; FAILS=$((FAILS + 1)); }
check() { # check DESCRIPTION COMMAND... (runs the command, passes when it succeeds)
  d=$1; shift
  if "$@" > /dev/null 2>&1; then ok "$d"; else bad "$d"; fi
}
nocheck() { # passes when the command fails
  d=$1; shift
  if "$@" > /dev/null 2>&1; then bad "$d"; else ok "$d"; fi
}

# ---- fake add-on root and path overrides ------------------------------------------------
A=$T/addon; D=$T/data; CONFD=$T/var/lib/alsa/conf.d; RUN=$T/run; FD=$T/fifo
mkdir -p "$A/bin" "$A/lib" "$D" "$RUN" "$FD"
cp "$SCRIPT" "$A/bin/alsa-leadtap"
cp "$LT_SO" "$A/lib/libasound_module_pcm_leadtap.so"
cp "$LT_WLED" "$A/bin/wled-audiosync"; chmod +x "$A/bin/wled-audiosync"
export LEADTAP_ADDON=$A LEADTAP_SETTINGS=$D/settings.xml LEADTAP_CONFD=$CONFD \
       LEADTAP_RUN=$RUN LEADTAP_FIFODIR=$FD LEADTAP_RESTART_DELAY=1
S="sh $A/bin/alsa-leadtap"
CONF=$CONFD/60-alsa-leadtap.conf

# the same load hook as LibreELEC's alsa.conf (the line "/var/lib/alsa/conf.d" of the stock file)
sed "s|\"/var/lib/alsa/conf.d\"|\"$CONFD\"|" /usr/share/alsa/alsa.conf > "$T/alsa.conf"
grep -q "$CONFD" "$T/alsa.conf" || bad "system alsa.conf has no /var/lib/alsa/conf.d entry"
export ALSA_CONFIG_PATH=$T/alsa.conf HOME=$T/home
mkdir -p "$HOME"

has() { grep -q -- "$1" "$2"; }
settings() { # settings 'xml lines'
  printf '<settings version="2">\n%s\n</settings>\n' "$1" > "$D/settings.xml"
}

# ---- static checks ----------------------------------------------------------------------
check "sh -n bin/alsa-leadtap" sh -n "$SCRIPT"
check "sh -n test_addon.sh" sh -n "$HERE/test_addon.sh"
check "default.py compiles" python3 -c "import sys; compile(open(sys.argv[1]).read(), 'default.py', 'exec')" "$HERE/addon/default.py"
for x in addon/addon.xml.in addon/resources/settings.xml; do
  check "xml well-formed: $x" python3 -c "import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])" "$HERE/$x"
done
check "unit: Before=kodi.service" grep -q '^Before=kodi.service' "$HERE/addon/system.d/service.alsa-leadtap.service"
check "unit named after the add-on id" test -f "$HERE/addon/system.d/service.alsa-leadtap.service"
check "settings ids match the script" python3 -c "
import sys, re, xml.etree.ElementTree as E
ids = sorted(s.get('id') for s in E.parse(sys.argv[1]).iter('setting'))
sys.exit(0 if ids == ['gain', 'lead', 'slave', 'target', 'wled'] else 1)" "$HERE/addon/resources/settings.xml"
check "settings labels exist in strings.po" python3 -c "
import sys, re
po = open(sys.argv[2]).read()
xml = open(sys.argv[1]).read()
need = set(re.findall(r'(?:label|help)=\"(\d+)\"', xml)) | set(re.findall(r'<heading>(\d+)</heading>', xml))
have = set(re.findall(r'msgctxt \"#(\d+)\"', po))
sys.exit(0 if need <= have else 1)" "$HERE/addon/resources/settings.xml" "$HERE/addon/resources/language/resource.language.en_gb/strings.po"

# ---- defaults (no settings file) --------------------------------------------------------
$S start > "$T/out" 2>&1; rc=$?
[ $rc = 0 ] && ok "start with no settings file" || { bad "start with no settings file (rc $rc)"; cat "$T/out"; }
check "conf.d file written" test -f "$CONF"
check "default slave" has 'slave.pcm "hdmi_bmc"' "$CONF"
check "default lead 180" has '^  lead 180$' "$CONF"
check "default: both pipes" has "fifo \[ \"$FD/leadtap.fifo\" \"$FD/leadtap-sync.fifo\" \]" "$CONF"
check "name begins with hdmi" has '^pcm.hdmi_leadtap {' "$CONF"
check "plugin path points into the add-on" has "lib \"$A/lib/libasound_module_pcm_leadtap.so\"" "$CONF"
check "default env" sh -c "grep -qx WLED_ON=true '$RUN/service.alsa-leadtap.env' && grep -qx TARGET=239.0.0.1 '$RUN/service.alsa-leadtap.env' && grep -qx GAIN=1.0 '$RUN/service.alsa-leadtap.env' && grep -qx LEAD=180 '$RUN/service.alsa-leadtap.env'"

# ---- the configuration parses and the device shows up -----------------------------------
check "aplay -L lists hdmi_leadtap" sh -c "aplay -L 2>&1 | grep -qx hdmi_leadtap"
check "aplay -L shows the hint text" sh -c "aplay -L 2>&1 | grep -q 'Audio with early copy'"
# conf.d rules: only *.conf is read, other files in the directory are ignored
printf 'pcm.junk_tmp {\n type null\n}\n' > "$CONFD/99-junk.conf.tmp"
printf 'pcm.junk_conf {\n type null\n}\n' > "$CONFD/99-junk.conf"
check "conf.d: a *.conf file is read" sh -c "aplay -L 2>&1 | grep -qx junk_conf"
nocheck "conf.d: the .tmp file is not read" sh -c "aplay -L 2>&1 | grep -qx junk_tmp"
rm -f "$CONFD/99-junk.conf" "$CONFD/99-junk.conf.tmp"

# ---- settings mapping -------------------------------------------------------------------
settings '    <setting id="slave">hdmi:CARD=vc4hdmi0,DEV=0</setting>
    <setting id="lead" default="false">250</setting>
    <setting id="wled">false</setting>
    <setting id="target">192.0.2.7</setting>
    <setting id="gain">2.5</setting>'
$S start > "$T/out" 2>&1
check "slave setting" has 'slave.pcm "hdmi:CARD=vc4hdmi0,DEV=0"' "$CONF"
check "lead setting" has '^  lead 250$' "$CONF"
check "wled off: only the snapserver pipe" has "fifo \[ \"$FD/leadtap.fifo\" \]" "$CONF"
check "env: wled off, target, gain" sh -c "grep -qx WLED_ON=false '$RUN/service.alsa-leadtap.env' && grep -qx TARGET=192.0.2.7 '$RUN/service.alsa-leadtap.env' && grep -qx GAIN=2.5 '$RUN/service.alsa-leadtap.env' && grep -qx LEAD=250 '$RUN/service.alsa-leadtap.env'"

settings '    <setting id="slave" default="true">  sp_dif:x  </setting>
    <setting id="lead" default="true">0</setting>
    <setting id="wled" default="true">true</setting>
    <setting id="target" default="true">10.1.2.3</setting>
    <setting id="gain" default="true">12.25</setting>'
$S start > "$T/out" 2>&1
check "whitespace trimmed" has 'slave.pcm "sp_dif:x"' "$CONF"
check "lead 0 is valid" has '^  lead 0$' "$CONF"
check "gain 12.25 valid" grep -qx GAIN=12.25 "$RUN/service.alsa-leadtap.env"

# bad values fall back to the defaults, each one separately
settings '    <setting id="slave">a"b;reboot</setting>
    <setting id="lead">abc</setting>
    <setting id="wled">maybe</setting>
    <setting id="target">999.1.1.1</setting>
    <setting id="gain">0</setting>'
$S start > "$T/out" 2>&1
check "bad slave -> default" has 'slave.pcm "hdmi_bmc"' "$CONF"
check "bad lead -> default" has '^  lead 180$' "$CONF"
check "bad wled -> default (on)" has "leadtap-sync.fifo" "$CONF"
check "bad target -> default" grep -qx TARGET=239.0.0.1 "$RUN/service.alsa-leadtap.env"
check "gain 0 -> default" grep -qx GAIN=1.0 "$RUN/service.alsa-leadtap.env"
check "bad values are said in the log" sh -c "grep -c 'using' '$T/out' | grep -qx 5"
for bad_lead in 2001 -5 1.5 99999 "1 2"; do
  settings "    <setting id=\"lead\">$bad_lead</setting>"
  $S start > "$T/out" 2>&1
  check "lead '$bad_lead' -> default" has '^  lead 180$' "$CONF"
done
for bad_ip in 1.2.3 1.2.3.4.5 1.2.3.256 host.example "239.0.0.1;x" "-1.2.3.4" 010.1.2.3 1.2.3.04 01.2.3.4; do
  settings "    <setting id=\"target\">$bad_ip</setting>"
  $S start > "$T/out" 2>&1
  check "target '$bad_ip' -> default" grep -qx TARGET=239.0.0.1 "$RUN/service.alsa-leadtap.env"
done
for bad_gain in -1 1e9 100 0.0 abc 1,5 .5; do
  settings "    <setting id=\"gain\">$bad_gain</setting>"
  $S start > "$T/out" 2>&1
  check "gain '$bad_gain' -> default" grep -qx GAIN=1.0 "$RUN/service.alsa-leadtap.env"
done
settings '    <setting id="slave" />
    <setting id="lead"></setting>
    <setting id="slave2">x</setting>'
$S start > "$T/out" 2>&1
check "empty entries -> defaults" sh -c "grep -q 'slave.pcm \"hdmi_bmc\"' '$CONF' && grep -q '^  lead 180\$' '$CONF'"
settings '    <setting id="slave">a&amp;b</setting>'
$S start > "$T/out" 2>&1
check "XML entity decoded then rejected" has 'slave.pcm "hdmi_bmc"' "$CONF"
printf 'garbage, not xml' > "$D/settings.xml"
check "garbage settings file -> defaults, start succeeds" $S start

# ---- failure leaves a reason ------------------------------------------------------------
mv "$A/lib/libasound_module_pcm_leadtap.so" "$T/so.keep"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start fails without the plugin" || bad "start fails without the plugin"
check "reason in the status file" grep -q 'plugin missing' "$RUN/service.alsa-leadtap.status"
mv "$T/so.keep" "$A/lib/libasound_module_pcm_leadtap.so"
$S start > "$T/out" 2>&1
nocheck "status removed by a good start" test -e "$RUN/service.alsa-leadtap.status"

# start refuses a file under our name that it did not write (stop leaves such a file, too)
rm -f "$CONF" "$RUN/service.alsa-leadtap.status"
printf '# mine, not yours\n' > "$CONF"
$S start > "$T/out" 2>&1; rc=$?
[ $rc != 0 ] && ok "start refuses to overwrite a foreign file" || bad "start refuses to overwrite a foreign file"
check "the foreign file is untouched" sh -c "[ \"\$(cat '$CONF')\" = '# mine, not yours' ]"
check "reason in the status file (foreign file)" grep -q 'refusing to overwrite' "$RUN/service.alsa-leadtap.status"
# ExecStopPost after the failed start: the reason has to survive it
SERVICE_RESULT=exit-code $S stop
check "failed start: stop keeps the status file" test -f "$RUN/service.alsa-leadtap.status"
check "failed start: stop leaves the foreign file" test -f "$CONF"
SERVICE_RESULT=success $S stop
nocheck "clean stop removes the status file" test -e "$RUN/service.alsa-leadtap.status"
rm -f "$CONF"

# wled-audiosync switched on but missing: sound works, the status says why, no sync pipe
rm -f "$D/settings.xml"
mv "$A/bin/wled-audiosync" "$T/wled.keep"
$S start > "$T/out" 2>&1; rc=$?
[ $rc = 0 ] && ok "start succeeds without wled-audiosync" || bad "start succeeds without wled-audiosync"
check "missing wled-audiosync is said in the status file" grep -q 'wled-audiosync is missing' "$RUN/service.alsa-leadtap.status"
check "missing wled-audiosync: WLED sync off in env" grep -qx WLED_ON=false "$RUN/service.alsa-leadtap.env"
nocheck "missing wled-audiosync: no sync pipe configured" has "leadtap-sync.fifo" "$CONF"
check "applied settings still say wled=true (what the user asked for)" grep -q 'wled=true' "$RUN/service.alsa-leadtap.applied"
mv "$T/wled.keep" "$A/bin/wled-audiosync"; chmod +x "$A/bin/wled-audiosync"

# rejected arguments (exit 2) are surfaced, the unit stays up
cp "$A/bin/wled-audiosync" "$T/wled.real"
printf '#!/bin/sh\nexit 2\n' > "$A/bin/wled-audiosync"; chmod +x "$A/bin/wled-audiosync"
$S start > "$T/out" 2>&1
$S run > "$T/run.out" 2>&1 &
RP=$!; PIDS="$PIDS $RP"
i=0; while [ $i -lt 20 ] && [ ! -s "$RUN/service.alsa-leadtap.status" ]; do sleep 0.2; i=$((i + 1)); done
check "rejected arguments are said in the status file" grep -q 'cannot run (exit 2' "$RUN/service.alsa-leadtap.status"
check "rejected arguments: run stays up" kill -0 "$RP"
kill -TERM "$RP"
i=0; while [ $i -lt 30 ] && kill -0 "$RP" 2> /dev/null; do sleep 0.2; i=$((i + 1)); done
nocheck "rejected arguments: run ends on TERM" kill -0 "$RP"
cp "$T/wled.real" "$A/bin/wled-audiosync"; chmod +x "$A/bin/wled-audiosync"
rm -f "$RUN/service.alsa-leadtap.status"

# applied vs current: default.py restarts the unit only for a real change
settings '    <setting id="lead">200</setting>'
$S start > "$T/out" 2>&1
check "current equals applied after start" sh -c "[ \"\$(sh '$A/bin/alsa-leadtap' current)\" = \"\$(cat '$RUN/service.alsa-leadtap.applied')\" ]"
settings '    <setting id="lead">200</setting>
    <setting id="unrelated">x</setting>'
check "an irrelevant settings change is no change" sh -c "[ \"\$(sh '$A/bin/alsa-leadtap' current)\" = \"\$(cat '$RUN/service.alsa-leadtap.applied')\" ]"
settings '    <setting id="lead">300</setting>'
nocheck "a changed lead differs from the applied settings" sh -c "[ \"\$(sh '$A/bin/alsa-leadtap' current)\" = \"\$(cat '$RUN/service.alsa-leadtap.applied')\" ]"
$S stop
nocheck "stop removes the applied file" test -e "$RUN/service.alsa-leadtap.applied"

# ---- run: wled-audiosync supervised, TERM stops it ---------------------------------------
rm -f "$D/settings.xml"
settings '    <setting id="target">127.0.0.1</setting>'
$S start > "$T/out" 2>&1
$S run > "$T/run.out" 2>&1 &
RP=$!; PIDS="$PIDS $RP"
wled_pid() { pgrep -f "$A/bin/wled-audiosync --pipe $FD/leadtap-sync.fifo" | head -n 1; }
w=""; i=0; while [ $i -lt 30 ] && [ -z "$w" ]; do w=$(wled_pid); [ -n "$w" ] || sleep 0.2; i=$((i + 1)); done
[ -n "$w" ] && ok "run starts wled-audiosync" || { bad "run starts wled-audiosync"; cat "$T/run.out"; }
check "wled-audiosync got lead and target" sh -c "tr '\\0' ' ' < /proc/$w/cmdline | grep -q -- '--lead-ms 180 --target 127.0.0.1 --gain 1.0'"
kill -9 "$w" 2> /dev/null
w2=""; i=0; while [ $i -lt 40 ] && [ -z "$w2" ]; do sleep 0.3; w2=$(wled_pid); i=$((i + 1)); done
{ [ -n "$w2" ] && [ "$w2" != "$w" ]; } && ok "wled-audiosync restarted after it died" || bad "wled-audiosync restarted after it died"
kill -TERM "$RP"
i=0; while [ $i -lt 30 ] && kill -0 "$RP" 2> /dev/null; do sleep 0.2; i=$((i + 1)); done
nocheck "run ends on TERM" kill -0 "$RP"
sleep 0.3
[ -z "$(wled_pid)" ] && ok "TERM stops wled-audiosync" || bad "TERM stops wled-audiosync"

settings '    <setting id="wled">false</setting>'
$S start > "$T/out" 2>&1
$S run > "$T/run.out" 2>&1 &
RP=$!; PIDS="$PIDS $RP"
sleep 1
[ -z "$(wled_pid)" ] && ok "wled off: no wled-audiosync" || bad "wled off: no wled-audiosync"
check "wled off: run stays up" kill -0 "$RP"
kill -TERM "$RP"
i=0; while [ $i -lt 30 ] && kill -0 "$RP" 2> /dev/null; do sleep 0.2; i=$((i + 1)); done
nocheck "wled off: run ends on TERM" kill -0 "$RP"

# ---- the whole chain: aplay -> plugin -> pipe -------------------------------------------
# the slave is the simulated real-time sound card of the plugin's own selftest (a null
# device consumes everything at once, which leaves no copy that is early)
printf 'pcm_type.fakeslave {\n lib "%s"\n}\npcm.lt_slave {\n type fakeslave\n}\n' "$LT_FAKESLAVE" > "$CONFD/50-test-slave.conf"
settings '    <setting id="slave">lt_slave</setting>
    <setting id="wled">false</setting>'
$S start > "$T/out" 2>&1
mkfifo "$FD/leadtap.fifo"
timeout 8 cat "$FD/leadtap.fifo" > "$T/copy.raw" &
CP=$!; PIDS="$PIDS $CP"
sleep 0.3
timeout 10 aplay -q -D hdmi_leadtap -t raw -f S16_LE -r 48000 -c 2 -d 2 /dev/zero > "$T/aplay.out" 2>&1; rc=$?
[ $rc = 0 ] && ok "aplay plays through hdmi_leadtap" || { bad "aplay plays through hdmi_leadtap (rc $rc)"; cat "$T/aplay.out"; }
sleep 0.5; kill "$CP" 2> /dev/null
n=$(wc -c < "$T/copy.raw")
[ "$n" -gt 100000 ] && ok "the pipe got the copy ($n bytes)" || { bad "the pipe got the copy ($n bytes)"; cat "$T/aplay.out"; }
rm -f "$CONFD/50-test-slave.conf"

# ---- stop removes everything ------------------------------------------------------------
$S start > "$T/out" 2>&1
printf 'x' > "$RUN/service.alsa-leadtap.status"
$S stop
nocheck "stop removes the conf.d file" test -e "$CONF"
nocheck "stop removes the env file" test -e "$RUN/service.alsa-leadtap.env"
nocheck "stop removes the status file" test -e "$RUN/service.alsa-leadtap.status"
nocheck "stop removes the empty conf.d directory" test -d "$CONFD"
nocheck "device is gone" sh -c "aplay -L 2>&1 | grep -qx hdmi_leadtap"
check "stop twice is fine" $S stop
# another add-on's file in the directory, and a foreign file under our name
mkdir -p "$CONFD"; printf '# other\n' > "$CONFD/50-rpi-gpio-spdif.conf"
$S start > "$T/out" 2>&1
$S stop
check "stop leaves other add-ons' files" test -f "$CONFD/50-rpi-gpio-spdif.conf"
check "stop leaves the directory when it is not empty" test -d "$CONFD"
printf '# mine, not yours\n' > "$CONF"
$S stop
check "stop leaves a file that is not ours" test -f "$CONF"
rm -f "$CONF"

# ---- build_addon.py ---------------------------------------------------------------------
B="python3 $HERE/build_addon.py --audiosync-license $LT_LICENSE_W --out $T/out-zip"
$B --so "$LT_SO" --audiosync "$LT_WLED_A64" > "$T/b.out" 2>&1; rc=$?
[ $rc != 0 ] && grep -q AArch64 "$T/b.out" && ok "wrong architecture (plugin) refused" || bad "wrong architecture (plugin) refused"
$B --so "$LT_SO_A64" --audiosync "$LT_WLED" > "$T/b.out" 2>&1; rc=$?
[ $rc != 0 ] && grep -q AArch64 "$T/b.out" && ok "wrong architecture (wled-audiosync) refused" || bad "wrong architecture (wled-audiosync) refused"
printf 'not an elf' > "$T/junk.bin"
$B --so "$T/junk.bin" --audiosync "$LT_WLED_A64" > "$T/b.out" 2>&1; rc=$?
[ $rc != 0 ] && grep -q 'not an ELF' "$T/b.out" && ok "non-ELF refused" || bad "non-ELF refused"
nocheck "no zip after refusals" test -e "$T/out-zip"
$B --so "$LT_SO_A64" --audiosync "$LT_WLED_A64" --version 9.8.7 > "$T/b.out" 2>&1; rc=$?
[ $rc = 0 ] && ok "aarch64 build packs" || { bad "aarch64 build packs"; cat "$T/b.out"; }
Z=$T/out-zip/service.alsa-leadtap-9.8.7.zip
check "zip named after id and version" test -f "$Z"
mkdir -p "$T/unz"; unzip -q -o "$Z" -d "$T/unz" 2> /dev/null
P=$T/unz/service.alsa-leadtap
for f in addon.xml default.py LICENSE.txt LICENSE-wled-audiosync.txt bin/alsa-leadtap bin/wled-audiosync \
         lib/libasound_module_pcm_leadtap.so system.d/service.alsa-leadtap.service resources/settings.xml \
         resources/language/resource.language.en_gb/strings.po; do
  check "zip has $f" test -f "$P/$f"
done
check "zip addon.xml: id and version" sh -c "grep -q 'id=\"service.alsa-leadtap\"' '$P/addon.xml' && grep -q 'version=\"9.8.7\"' '$P/addon.xml' && ! grep -q @VERSION@ '$P/addon.xml'"
check "zip addon.xml well-formed" python3 -c "import sys, xml.etree.ElementTree as E; E.parse(sys.argv[1])" "$P/addon.xml"
check "zip: scripts are executable" python3 -c "
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
m = {i.filename.split('/', 1)[1]: (i.external_attr >> 16) & 0o111 for i in z.infolist()}
sys.exit(0 if m['bin/alsa-leadtap'] and m['bin/wled-audiosync'] and not m['default.py'] else 1)" "$Z"
check "zip: plugin is the aarch64 one" cmp "$P/lib/libasound_module_pcm_leadtap.so" "$LT_SO_A64"
$B --so "$LT_SO_A64" --audiosync "$LT_WLED_A64" > "$T/b.out" 2>&1
want=$(sed -n 's/^## \([0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*\).*/\1/p' "$ROOT/CHANGELOG.md" | head -n 1)
check "default version from CHANGELOG ($want)" test -f "$T/out-zip/service.alsa-leadtap-$want.zip"

echo
if [ "$FAILS" = 0 ]; then echo "ALL OK"; else echo "$FAILS check(s) FAILED"; exit 1; fi
