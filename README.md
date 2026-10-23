# ios-hotspot-vpn-keepalive

Personal Hotspot tools for a rootless-jailbroken iPhone:

- **VPN sharing without proxies.** Everything that connects to the phone's Personal Hotspot goes through the VPN that runs on the phone. No proxy settings, no app on the clients.
- **Clients stay connected.** A locked iPhone/iPad client no longer drops off the hotspot a second after its screen locks, and the hotspot itself no longer sleeps with the screen off.

Everything runs on the phone as small launch daemons. Nothing is needed on the clients or on a server.

> **Status:** works for the author on one setup (see [Tested on](#tested-on)). Other devices, iOS versions and VPN apps are untested. It uses private APIs and a packet-filter hack; read [Known limitations](#known-limitations) before installing. Personal use on your own devices only.

## What is in the box

| Part | File | What it does |
| --- | --- | --- |
| keepawake | `phone/keepawake.py` | Holds power assertions (no idle/system sleep) while on AC power or while the hotspot interface exists. Supervises the two helpers below. |
| hshold | `phone/hshold.py` | Keeps a TCP connection open to every hotspot client's lockdownd (port 62078) and sends a query every 30 s, so a locked client stays on the hotspot. |
| pftun | `phone/pftun/pftun.c` | Routes hotspot client traffic into the VPN tunnel via `pf`, restarts the VPN if iOS kills it for using too much memory, raises its memory limit and throttles uploads near it. |
| pftun-vpnid | `phone/pftun/pftun-vpnid` | Finds the configuration UUID of the running VPN (used by pftun to restart it). |
| socksd | `phone/socksd.py` | Optional SOCKS5 + HTTP proxy for hotspot clients (egress from the phone, so through its VPN). Only needed if you do not use pftun. |
| hotspot | `phone/hotspot.py` | Shared helper: hotspot interface and its subnet. |
| tools | `phone/tools/ulog.c`, `phone/pftun/pfdump.c` | Diagnostics: unified log streamer for iOS 17, pf rule dumper. |

## Architecture

```
                        +-------------------------------- phone (rootless jailbreak) --------------------------------+
 client (Wi-Fi) ------> | bridge100 (hotspot)                                                                         |
                        |    |  pf: pass in quick on bridge100 route-to <pftun utun>                                  |
                        |    v                                                                                       |
                        |  pftun's own classic utun (198.18.0.1)  --read-->  pftun (userspace)                        |
                        |                                                       |  raw IP_HDRINCL socket,            |
                        |                                                       |  IP_BOUND_IF <vpn utun>            |
                        |                                                       v                                    |
                        |  pf: nat on <vpn utun>  <---------------------  VPN utun (any NEPacketTunnel VPN app)       |
                        +----------------------------------------------------|---------------------------------------+
                                                                             v
                                                                       VPN server / internet
```

- **Re-sending from userspace (pftun).** pf `route-to` hands client packets to pftun's own *classic* utun; pftun reads them and re-sends each one through a raw socket bound to the VPN's utun, where a pf `nat` rule rewrites the source address. Replies come back through the VPN, pf undoes the NAT and the kernel forwards them to the hotspot bridge. The roundabout path exists because `route-to <vpn utun>` panics the kernel (see [Known limitations](#known-limitations)).
- **Automatic on/off.** pftun checks once a second. If the hotspot or the VPN is gone it removes its rules and the hotspot falls back to the normal cellular path, with no reconfiguration. When both are up again it installs the rules and clears old pf states so existing connections move to the VPN.
- **Detection, not configuration.** The hotspot interface is `bridge100` (iOS); its subnet is read from the interface. The VPN tunnel is the `utun` that has a private IPv4 address (link-local `169.254/16` and `198.18/15` are ignored). The VPN's packet-tunnel extension is the `.appex` process that holds a utun control socket; the VPN configuration UUID is looked up in `/var/preferences/com.apple.networkextension.plist`.
- **VPN extension protection.** iOS kills a packet-tunnel extension that exceeds its memory limit (50 MB on the tested device) and does not reconnect On Demand VPNs while the screen is locked. pftun raises that process's jetsam limit (default 128 MB), watches its memory, throttles client *uploads* above 80% of the limit (full drop at 86%) so client TCP backs off, and, if the extension is killed by jetsam anyway, restarts the VPN through `nesessionmanager` within ~10 s. A VPN you stopped yourself is not restarted.
- **Locked clients (hshold).** An iOS client leaves the hotspot about a second after its screen locks unless something keeps an active Wi-Fi connection to it. hshold finds clients in the hotspot's ARP table, opens a TCP connection to each client's lockdownd and sends a `QueryType` request every 30 s.

## Requirements

- A rootless-jailbroken iPhone (`/var/jb` layout) with root SSH (OpenSSH from your package manager), Python 3 (installed by `install.sh` from the Procursus repo if missing), and a working Personal Hotspot.
- For VPN sharing: a VPN app that uses a Network Extension packet tunnel (NEPacketTunnelProvider) and gives the tunnel an IPv4 address.
- A computer with `bash`, `ssh`, `git`, `curl` and `tar` to build pftun (it is compiled on the phone; the Apple headers and SDK are downloaded at build time and are not part of this repository).
- For clients that must not drop when locked: the client must be Wi-Fi-sync paired with a computer, otherwise it does not open port 62078 over Wi-Fi (see [Known limitations](#known-limitations)).

## Install

All commands marked *phone* run as root on the phone over SSH; *computer* commands run on your computer. `<phone>` is the phone's address or SSH host name.

1. **Copy the `phone` folder to the phone** (*computer*):
   ```sh
   scp -r phone root@<phone>:/var/mobile/hotspot-vpn
   ```
2. **Hotspot keep-awake + locked-client hold only** (*phone*):
   ```sh
   cd /var/mobile/hotspot-vpn && ./install.sh
   ```
   This installs Python 3 if needed, copies the scripts to `/var/jb/usr/local/bin`, installs the launch daemon `com.mali.keepawake` and starts it. Logs: `/var/jb/var/log/keepawake.log`, `socksd.log`, `hshold.log`.
3. **VPN sharing (optional)**:
   - Build pftun (*computer*; needs SSH key access to the phone, `KEY=<private key>` selects a key):
     ```sh
     PHONE=<phone> phone/pftun/build.sh
     ```
     It installs `clang` and `ldid` on the phone if they are missing, uploads the headers and SDK, and builds `/var/mobile/pfb/pftun` and `pfdump` on the phone.
   - Install it (*phone*):
     ```sh
     cd /var/mobile/hotspot-vpn && ./install.sh --pftun
     ```
   - Start your VPN and the hotspot. `/var/jb/var/log/pftun.log` should show `own utun utunN`, `detected VPN extension <name>`, `VPN configuration <uuid>`, `VPN extension memory limit 50 -> 128 MB` and `install utunM <addr> ok, hotspot <subnet>`.
   - Check from a client: its public IP should be the VPN's.
4. **Keep socksd?** If you use pftun you do not need the proxy. Set `KEEPAWAKE_CHILDREN=hshold` in the keepawake plist (see below) to not start it.

## Configuration

Defaults work without any configuration. Overrides:

**pftun** (arguments after `run` in `/var/jb/Library/LaunchDaemons/com.mali.pftun.plist`; each flag and its value are separate `<string>` entries; reload with `launchctl unload` / `load -w`):

| Flag | Meaning | Default |
| --- | --- | --- |
| `-b <if>` | hotspot bridge interface | `bridge100` |
| `-i <if>` | VPN interface to use, e.g. `utun5` | detected private-IPv4 `utun` |
| `-u <uuid>` | VPN configuration UUID for automatic restarts | looked up by `pftun-vpnid` |
| `-n <name>` | VPN extension process name | detected (`.appex` with a utun control socket) |
| `-m <MB>` | jetsam limit for the VPN extension, `0` = leave it alone | `128` |

`/var/jb/usr/local/bin/pftun-vpnid --list` prints the VPN configurations it can see (UUID and name); `pftun-vpnid <path of the extension executable>` prints the one it would pick.

**keepawake / helpers** (environment variables, add an `EnvironmentVariables` dict to the plist):

| Variable | Used by | Meaning | Default |
| --- | --- | --- | --- |
| `HOTSPOT_IF` | all scripts | hotspot interface | `bridge100` |
| `KEEPAWAKE_ON_AC` | keepawake | `0` = hold the assertions only while the hotspot is up (not on AC power) | `1` |
| `KEEPAWAKE_CHILDREN` | keepawake | helper scripts to supervise | `socksd,hshold` |
| `SOCKSD_PORT` | socksd | proxy port | `1080` |
| `SOCKSD_ALLOW` | socksd | comma-separated CIDRs allowed to connect | the hotspot's current subnet |

## Uninstall / rollback

On the phone:
```sh
cd /var/mobile/hotspot-vpn && ./uninstall.sh
```
This unloads and removes both daemons and the scripts and runs `pftun del` (removes the pf rules and clears client states). To disable only VPN sharing: `launchctl unload -w /var/jb/Library/LaunchDaemons/com.mali.pftun.plist; /var/jb/usr/local/bin/pftun del`. The hotspot then uses the cellular connection directly, as before. If the daemon was killed uncleanly, rebooting also clears the pf rules (they live only in the kernel).

## Troubleshooting

- **Hotspot clients have no internet after installing pftun.** Check `pftun.log`. No `install ... ok` line means the VPN utun or the bridge was not found: run `ifconfig` (from `network-cmds`) and look for a `utun` with a private IPv4 address; pass it with `-i`. `pftun del` restores normal behavior at once.
- **`VPN configuration UUID not found`.** Automatic restart is disabled, nothing else is affected. Run `pftun-vpnid --list`; pass the right UUID with `-u`.
- **The VPN dies under heavy upload and does not come back.** Check for `VPN extension killed by jetsam` in `pftun.log`. Make sure the VPN app's own memory-saving options are on, and, for sing-box based apps, avoid the `gvisor` TUN stack (it terminates TCP inside the extension and buffers a lot; `mixed` is what was tested).
- **A locked client still leaves the hotspot.** `hshold.log` shows `<ip> holding` for each client. If a client is retried every 30 seconds without ever holding, it is refusing port 62078: pair it with a computer for Wi-Fi sync (Finder / Apple Devices, "Show this iPhone when on Wi-Fi").
- **keepawake restarts every few minutes (and the proxy / hshold with it).** launchd gives daemons under `/var/jb` a 6 MB jetsam limit and ignores `JetsamProperties` in the plist; python3 with ctypes needs about that much. keepawake raises its own limit to 32 MB at start (`memory limit 6 -> 32 MB` in `keepawake.log`). If it still gets `SIGKILL`ed, check that line and the log for restarts.
- **Phone unreachable over SSH.** A locked/sleeping phone that is neither charging nor serving the hotspot drops off Wi-Fi; unlock it.
- **Diagnostics.** `phone/tools/ulog.c` streams the iOS 17 unified log (`clang -arch arm64 -isysroot <sdk> -fblocks ulog.c -o ulog && ldid -Sulog.xml ulog`, then `./ulog 'process == "hostapd"'`); `pfdump` dumps the pf anchors and rules.

## Known limitations

- **Kernel panics with `route-to <vpn utun>`.** The obvious design, pf `route-to` straight into the VPN's utun, panicked the test phone three times under upload load (`nx_netif_host_output -> m_copydata: invalid mbuf chain`). The VPN utun is a skywalk netif that advertises TSO; an oversized hotspot mbuf passes pf's checks and crashes while it is copied into a skywalk packet. pftun avoids this by never letting the kernel forward an mbuf into that interface: the userspace detour through a classic utun and a raw socket builds every packet from scratch within the MTU. Do not "simplify" it back to `route-to <vpn utun>`. Details in [`phone/pftun/README.md`](phone/pftun/README.md).
- **VPN extension memory limit.** iOS kills packet-tunnel extensions at a per-process limit (50 MB on the tested device). Raising the limit, throttling uploads and restarting the VPN after a kill reduce the problem; they do not remove it. Uploads from many clients at once are the worst case.
- **Locked-client hold needs Wi-Fi-sync pairing.** hshold depends on the client answering on lockdownd's port 62078 over Wi-Fi, which iOS only opens when the client is paired with a computer for Wi-Fi sync. If that pairing is removed the hold probably stops working (not tested). A locked client in power saving answers pings only now and then; that is normal.
- **IPv4 only.** IPv6 traffic from clients is not routed through the VPN.
- **Private APIs.** pftun uses `pf` ioctls, `memorystatus_control` and the private `ne_session_*` functions of libsystem_networkextension. A future iOS update can break any of them.
- **Hotspot interface name.** `bridge100` is assumed (overridable); other values are untested.
- **Reinstalling the VPN app** changes its configuration UUID; with automatic lookup this needs no action, with `-u` it does.

## Tested on

Everything here was developed and run on this one setup only:

- Hotspot / server phone: iPhone SE (2nd generation, iPhone12,8), iOS 17.6.1 (21G93), Dopamine rootless jailbreak.
- VPN: Hiddify (sing-box TUN, `mixed` stack).
- Hotspot client: iPhone 14 Pro Max on iOS 27 (screen-lock hold verified for 6 minutes on battery; incoming Continuity calls arrive instantly while locked).

The generalizations below were written afterwards **and have not been run on a device**, so treat them as untested: detecting the hotspot subnet from the interface (the tested subnet was `172.20.10.0/28`), detecting the VPN extension and configuration UUID automatically (the tested setup passed them by hand), the command-line flags, `install.sh` / `uninstall.sh` and the `KEEPAWAKE_*` / `SOCKSD_*` / `HOTSPOT_IF` settings. The keepawake memory-limit raise (`memorystatus_control`) and the 30 s hshold retry were run on the device in their original form; the ported versions here were not. The C changes were also not compiled before publishing (the compiler lives on the phone). Other devices, iOS versions, jailbreaks and VPN apps are untested.

## License

MIT, see [LICENSE](LICENSE).
