#!/var/jb/usr/bin/python3
# Holds an open connection to the lockdownd (TCP 62078) of every Personal Hotspot client (iPhone/iPad).
# When its screen locks, an iOS client leaves the hotspot unless something keeps a Wi-Fi connection
# active; with this connection open it stays associated. Runs as a child of keepawake.py.
import plistlib, re, socket, struct, subprocess, threading, time

import hotspot

PORT = 62078
INTERVAL = 30      # seconds between QueryType requests
TIMEOUT = 60       # a locked client in power saving can answer late
REFUSED_BACKOFF = 30  # a freshly joined iPhone can open 62078 a few seconds late; a long backoff delayed the hold by minutes
REQ = plistlib.dumps({'Label': 'hshold', 'Request': 'QueryType'})
ARP = re.compile(r'\((\d+\.\d+\.\d+\.\d+)\) at ([0-9a-f:]+) on ' + re.escape(hotspot.IFACE))

active = set()
backoff = {}

def log(m):
    print(time.strftime('%F %T'), m, flush=True)

def clients():
    try:
        out = subprocess.run(['/var/jb/usr/sbin/arp', '-an', '-i', hotspot.IFACE], capture_output=True, text=True, timeout=10).stdout
    except Exception:
        return []
    own = hotspot.address()
    return [ip for ip, mac in ARP.findall(out) if ip != own]

def query(s):
    s.sendall(struct.pack('>I', len(REQ)) + REQ)
    n = struct.unpack('>I', s.recv(4, socket.MSG_WAITALL))[0]
    return plistlib.loads(s.recv(n, socket.MSG_WAITALL))

def hold(ip):
    try:
        s = socket.create_connection((ip, PORT), timeout=5)
    except ConnectionRefusedError:
        backoff[ip] = time.time() + REFUSED_BACKOFF
        active.discard(ip)
        return
    except Exception:
        active.discard(ip)
        return
    start = time.time()
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
        s.settimeout(TIMEOUT)
        if query(s).get('Type') != 'com.apple.mobile.lockdown':
            backoff[ip] = time.time() + REFUSED_BACKOFF
            return
        log('%s holding' % ip)
        while True:
            time.sleep(INTERVAL)
            query(s)
    except Exception as e:
        log('%s dropped after %ds: %r' % (ip, time.time() - start, e))
    finally:
        s.close()
        active.discard(ip)

log('start')
while True:
    now = time.time()
    for ip in clients():
        if ip in active or backoff.get(ip, 0) > now:
            continue
        active.add(ip)
        threading.Thread(target=hold, args=(ip,), daemon=True).start()
    time.sleep(5)
