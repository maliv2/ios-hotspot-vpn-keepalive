#!/var/jb/bin/sh
# Run as root ON THE PHONE. Removes the daemons, scripts and pf rules; the hotspot goes back to normal.
J=/var/jb
BIN=$J/usr/local/bin
LD=$J/Library/LaunchDaemons
export PATH=$J/usr/bin:$J/usr/sbin:$J/bin:$J/sbin:$PATH
for n in pftun keepawake; do
    launchctl unload -w $LD/org.hotspotvpn.$n.plist 2>/dev/null
    rm -f $LD/org.hotspotvpn.$n.plist
done
[ -x $BIN/pftun ] && $BIN/pftun del
rm -f $BIN/pftun $BIN/pftun-vpnid $BIN/hotspot.py $BIN/keepawake.py $BIN/socksd.py $BIN/hshold.py
echo "removed (logs stay in $J/var/log)"
