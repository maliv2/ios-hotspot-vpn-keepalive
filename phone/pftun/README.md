# pftun: share the hotspot through the VPN (L3, no proxy, no client settings)

Developed on an iPhone SE (2nd gen), iOS 17.6.1 (21G93, xnu-10063.142.1), Dopamine rootless, with Hiddify (sing-box TUN) as the VPN. All IPv4 traffic (TCP, UDP, ICMP) of Personal Hotspot clients leaves through the VPN running on the phone. The VPN's TUN answers ping and DNS locally (ping ~5 ms to the first hop). With the VPN off the rules are removed and the hotspot goes back to the normal cellular path.

The numbers below (utun names, addresses, memory figures) are from that setup and only illustrate the mechanism.

## How it works

```
client -> bridge100 -> pf: pass in quick on bridge100 route-to <own utun>   (<hotspot subnet> -> !<hotspot subnet>)
       -> own utun (pftun's classic utun, 198.18.0.1 -> 198.18.0.2, no skywalk netif)
       -> pftun reads and validates the packet, re-sends it through an IP_HDRINCL raw socket (IP_BOUND_IF <vpn utun>)
       -> ip_output -> pf: nat on <vpn utun> from <hotspot subnet> -> <vpn utun address> -> VPN
return: vpn utun -> pf reverses the nat -> the kernel forwards to bridge100 (iOS sets net.inet.ip.forwarding=1 while the hotspot is on)
```

- `pftun run` (daemon): once a second it finds the hotspot bridge (`bridge100`, subnet from the interface address and netmask) and the VPN utun (a `utun*` with an IPv4 address that is neither link-local `169.254/16` nor in `198.18/15`; `-i` overrides). If the rules are missing it adds them, then deletes old pf states from the hotspot subnet (`DIOCKILLSTATES`) so open connections move to the new path. iOS removes the rules from time to time, so they are re-added. When the VPN or hotspot goes away the rules are removed. Every 60 s it logs `pkts= bytes= bad= big= senderr=`. If the hotspot subnet changes the rules are reinstalled.
- `pftun del`: removes the rules and states.
- The rules are added to the anchor `com.apple.internet-sharing` (root -> `com.apple.internet-sharing` -> `base_v4`). The nat port range is 50001-65535, which does not overlap the 32768-49151 range used by `base_v4`.
- The daemon is `/var/jb/usr/local/bin/pftun run` with the launchd plist `org.hotspotvpn.pftun`; log `/var/jb/var/log/pftun.log`. launchd's jetsam limit for it is 6 MB; it uses about 1.4 MB.

## Why so roundabout: kernel panic

The first attempt, `route-to <vpn utun>` (directly into the VPN's utun), panicked the phone three times under load: `m_copydata: invalid mbuf chain [off 0, len 44392|47208] @uipc_mbuf.c:6433`. The panic logs were symbolicated against the kernelcache (blacktop `ipsw` plus symbolicator signatures); all three had the same stack:

`bridge_iff_input -> dlil_input -> ip_input -> pf_af_hook -> pf_test -> pf_route+0x460 -> dlil_output -> nx_netif_host_output -> _pkt_copypkt_sum -> m_copydata`

- Direction: client to internet (uplink). An mbuf arrives on the hotspot input with a pkthdr length of about 47 KB and a chain that is shorter than that, and reaches `pf_route`.
- The VPN utun is `CHANNEL_DRV` (a skywalk netif) and advertises `hwassist TSO_V4`. pf_route's TSO check passes, so the packet goes to the netif's host path unsplit; there it is copied into a skywalk packet and `m_copydata` crashes. `rx_agg_tcp=0` and `net.inet.tcp.tso=0` did not prevent it.
- Fix: never let the kernel forward an mbuf into the VPN utun. A classic utun (`ctl_enqueuembuf`, no netif copy) hands packets to userspace, and the raw socket builds every packet from scratch as a new mbuf within the MTU. With raw `IP_HDRINCL`, `ip_len` and `ip_off` must be in host byte order, and a foreign source address is allowed.
- Result: 400k+ packets / 450+ MB with `bad=0 big=0 senderr=0` and no panic. Ookla: 51.4/22.7 Mbps down/up, ping 81 ms.

## The VPN shutting itself down (solved on the test setup: TUN stack = `mixed`)

Under a many-connection upload (12 x 5 MB in parallel) the VPN turned itself off within about a second, with no panic. Cause: iOS limits the VPN's Network Extension to 50 MB and kills it above that (memorystatus `attr 1`, no jetsam report is written). sing-box with `stack: gvisor` terminates TCP inside the process; because the client RTT is low and the egress is slow, upload data piles up in gVisor's buffers. Downloads do not pile up (24 connections: 26 MB at most).

Switching the VPN app's TUN stack to `mixed` (TCP in the kernel, UDP in gVisor) fixed it for that test: 24 parallel uploads plus 2 downloads kept the extension at 24.7 MB at most. Going back to `gvisor` brings the problem back. Even with `mixed`, a heavy load (Ookla, reproduced with the screen locked) could push the extension to 48.7 MB and get it killed; system memory was free (31%), so it is the per-process limit. The VPN app's own "memory limit" option should stay on: it tightens sing-box's GC, and turning it off makes things worse.

## Raising the extension's memory limit

iOS sets the 50 MB limit, but root may change it. When pftun finds the extension it calls `memorystatus_control(SET_MEMLIMIT_PROPERTIES)` to set the active and inactive limit to 128 MB (`-m`; still fatal, so a runaway extension still dies). It re-reads the limit every second and re-applies it if iOS takes it back (log: `VPN extension memory limit 50 -> 128 MB`). The limit is per process; when the extension restarts pftun raises it again. After 3 rounds of (48 parallel uploads + 6 downloads) the limit stayed at 128 MB and the extension peaked at 40.5 MB (with the old limit the protection below would have kicked in), no kills.

## Memory protection (upload throttling)

pftun reads the extension's `phys_footprint` every 100 ms (`proc_pid_rusage`). Above 80% of the limit (about 102 MB of 128 MB, 40 MB of 50 MB) it drops client packets of 256 bytes or more (data, not ACKs or DNS) with 20% probability, rising to 100% at 86%. The client's TCP slows down and the extension's buffers stop growing. The 60-second log line has `throttled=` (dropped packets) and `ne_peak=` (highest memory of the period).
- Tested with the thresholds lowered to 25/31 MB: 7,500 packets dropped during 32 parallel uploads + 4 downloads, the extension peaked at 34 MB (about 3 MB above the full-drop point, because of the 100 ms sampling interval). The 86% ceiling leaves room for this overshoot.
- At the real thresholds a freshly started extension stays at 24-33 MB under heavy load (the protection does not trigger). The kills seen earlier happened when the extension's memory grew past 30 MB over long use.

## Reconnecting the VPN after it was killed

The test VPN's configuration had iOS On Demand (Connect rule) enabled, but it does not fire while the screen is locked (tried a BSD socket, CFNetwork and NSURLSession requests). The VPN only came back when the phone was unlocked.

pftun watches the extension process with kqueue `EVFILT_PROC NOTE_EXIT | NOTE_EXIT_DETAIL`. If the exit is `NOTE_EXIT_MEMORY` (jetsam) it restarts the VPN with the private `ne_session_create` / `ne_session_start` (libsystem_networkextension). The old session needs about 6 s to tear down, so it retries every 4 s, at most 8 times. With the screen locked the VPN was back about 10 s after the kill. If you stop the VPN yourself the process exits normally (`VPN extension exited`) and pftun leaves it alone (verified).

- Finding the extension: an `.appex` process that holds a `com.apple.net.utun_control` socket (`proc_pidinfo` / `proc_pidfdinfo`), or the process named with `-n`.
- Finding the configuration UUID: `pftun-vpnid <path of the extension executable>` reads the bundle identifier from the `Info.plist` next to the executable and matches it against the VPN configurations in `/var/preferences/com.apple.networkextension.plist` (an NSKeyedArchiver file; the UUID is the configuration's `Identifier`). If nothing matches and there is exactly one packet-tunnel configuration, that one is used. `pftun-vpnid --list` shows what it sees; `-u <uuid>` skips the lookup. The configuration UUID changes if the VPN app is reinstalled.
- To test: `memorystatus_control(MEMORYSTATUS_CMD_SET_JETSAM_TASK_LIMIT, pid, 1)` kills the extension like a real jetsam kill.

Measuring tools (not in this repository): `memorystatus_control(GET_MEMLIMIT_PROPERTIES)` plus `proc_pid_rusage(RUSAGE_INFO_V4).ri_phys_footprint`.

## Build and install

`PHONE=<phone> ./build.sh` (on your computer). It downloads the XNU headers `bsd/net/{pfvar.h,radix.h,pf_pbuf.h,if_pflog.h}`, `bsd/sys/{kern_control.h,sys_domain.h}` and `libkern/tree.h` (xnu-10063.141.1) and the theos iPhoneOS16.5 SDK, uploads them to the phone and builds in `/var/mobile/pfb`. The Apple headers are not part of this repository. `pfdump` dumps the anchors and rules.

Install: `phone/install.sh --pftun` from the top-level instructions. By hand (on the phone, in `/var/mobile/pfb`):
```
cp pftun /var/jb/usr/local/bin/pftun
P=/var/jb/Library/LaunchDaemons/org.hotspotvpn.pftun.plist; cp org.hotspotvpn.pftun.plist $P; chown root:wheel $P; chmod 644 $P
launchctl load -w $P
```
Rollback: `launchctl unload -w $P; rm $P; /var/jb/usr/local/bin/pftun del`.
