#!/var/jb/bin/sh
# Run as root ON THE PHONE, from the directory the "phone" folder was copied to.
#   ./install.sh            keepawake + socksd + hshold (hotspot stays up, locked clients stay on it)
#   ./install.sh --pftun    additionally pftun (hotspot clients share the phone's VPN); needs a built pftun binary
set -e
D=$(cd "$(dirname "$0")" && pwd)
J=/var/jb
BIN=$J/usr/local/bin
LD=$J/Library/LaunchDaemons
[ -d $J ] || { echo "$J not found: this installer is for rootless jailbreaks (Dopamine, palera1n rootless, ...)"; exit 1; }
export PATH=$J/usr/bin:$J/usr/sbin:$J/bin:$J/sbin:$PATH

command -v python3 >/dev/null || apt-get install -y -o Acquire::Retries=5 python3
mkdir -p $BIN $J/var/log
for f in hotspot keepawake socksd hshold; do cp $D/$f.py $BIN/$f.py; chmod 755 $BIN/$f.py; done
daemons="keepawake"

if [ "$1" = "--pftun" ]; then
    P=$D/pftun/pftun
    [ -x "$P" ] || P=/var/mobile/pfb/pftun
    [ -x "$P" ] || { echo "no pftun binary: build it first with pftun/build.sh (run on your computer)"; exit 1; }
    cp $P $BIN/pftun; chmod 755 $BIN/pftun
    cp $D/pftun/pftun-vpnid $BIN/pftun-vpnid; chmod 755 $BIN/pftun-vpnid
    daemons="$daemons pftun"
fi

for n in $daemons; do
    L=com.mali.$n
    cp $D/launchd/$L.plist $LD/$L.plist
    chown root:wheel $LD/$L.plist; chmod 644 $LD/$L.plist
    launchctl unload $LD/$L.plist 2>/dev/null || true
    launchctl load -w $LD/$L.plist
done
sleep 3
echo "---- keepawake.log ----"; tail -5 $J/var/log/keepawake.log
[ -f $J/var/log/pftun.log ] && { echo "---- pftun.log ----"; tail -5 $J/var/log/pftun.log; }
exit 0
