#!/var/jb/usr/bin/python3
# Offers the phone's own egress path (including an active VPN) to Personal Hotspot clients as a
# SOCKS5 + HTTP proxy. Uses threads + select instead of asyncio because of launchd's low memory limit.
import ipaddress, os, resource, select, socket, struct, threading, time

import hotspot

PORT = int(os.environ.get('SOCKSD_PORT', '1080'))
# Only hotspot clients may connect: by default the current network of the hotspot interface.
# SOCKSD_ALLOW can hold comma-separated CIDRs instead (e.g. "172.20.10.0/28").
ALLOW = [ipaddress.ip_network(n.strip()) for n in os.environ.get('SOCKSD_ALLOW', '').split(',') if n.strip()]

def allowed(ip):
    nets = ALLOW or [hotspot.network()]
    a = ipaddress.ip_address(ip)
    return any(n is not None and a in n for n in nets)

def log(m):
    print(time.strftime('%F %T'), m, flush=True)

def recvn(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c:
            raise EOFError
        b += c
    return b

def pump(a, b):
    socks = [a, b]
    try:
        while True:
            r, _, _ = select.select(socks, [], [], 120)
            if not r:
                return
            for s in r:
                d = s.recv(16384)
                if not d:
                    return
                (b if s is a else a).sendall(d)
    except Exception:
        pass

def connect(host, port):
    return socket.create_connection((host, port), 15)

def socks5(c):
    n = recvn(c, 1)[0]
    recvn(c, n)
    c.sendall(b'\x05\x00')
    ver, cmd, _, atyp = recvn(c, 4)
    if atyp == 1:
        host = socket.inet_ntoa(recvn(c, 4))
    elif atyp == 3:
        host = recvn(c, recvn(c, 1)[0]).decode()
    elif atyp == 4:
        host = socket.inet_ntop(socket.AF_INET6, recvn(c, 16))
    else:
        c.sendall(b'\x05\x08\x00\x01' + b'\0' * 6)
        return
    port = struct.unpack('>H', recvn(c, 2))[0]
    if cmd != 1:
        c.sendall(b'\x05\x07\x00\x01' + b'\0' * 6)
        return
    try:
        r = connect(host, port)
    except Exception as e:
        log('connect fail %s:%s %r' % (host, port, e))
        c.sendall(b'\x05\x05\x00\x01' + b'\0' * 6)
        return
    c.sendall(b'\x05\x00\x00\x01' + b'\0' * 6)
    pump(c, r)
    r.close()

def http(c, first):
    head = first
    while b'\r\n\r\n' not in head:
        d = c.recv(4096)
        if not d:
            return
        head += d
        if len(head) > 65536:
            return
    hdr, _, body = head.partition(b'\r\n\r\n')
    line, _, rest = hdr.partition(b'\r\n')
    method, target, ver = line.decode().split(' ', 2)
    if method == 'CONNECT':
        host, _, port = target.rpartition(':')
        try:
            r = connect(host.strip('[]'), int(port))
        except Exception as e:
            log('connect fail %s %r' % (target, e))
            c.sendall(b'HTTP/1.1 502 Bad Gateway\r\n\r\n')
            return
        c.sendall(b'HTTP/1.1 200 Connection established\r\n\r\n')
        if body:
            r.sendall(body)
        pump(c, r)
        r.close()
        return
    if not target.startswith('http://'):
        c.sendall(b'HTTP/1.1 400 Bad Request\r\n\r\n')
        return
    hostport, _, path = target[7:].partition('/')
    host, _, port = hostport.partition(':')
    keep = [l for l in rest.split(b'\r\n') if not l.lower().startswith((b'proxy-connection', b'connection'))]
    req = ('%s /%s %s\r\n' % (method, path, ver)).encode() + b'\r\n'.join(keep) + b'\r\nConnection: close\r\n\r\n' + body
    try:
        r = connect(host, int(port or 80))
    except Exception as e:
        log('connect fail %s %r' % (hostport, e))
        c.sendall(b'HTTP/1.1 502 Bad Gateway\r\n\r\n')
        return
    r.sendall(req)
    pump(c, r)
    r.close()

def handle(c, addr):
    try:
        log('conn %s rss=%dKB' % (addr[0], resource.getrusage(resource.RUSAGE_SELF).ru_maxrss // 1024))
        if not allowed(addr[0]):
            return
        c.settimeout(30)
        b = recvn(c, 1)
        c.settimeout(None)
        if b == b'\x05':
            socks5(c)
        else:
            http(c, b)
    except Exception as e:
        log('handler %r' % (e,))
    finally:
        try:
            c.close()
        except Exception:
            pass

def main():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('0.0.0.0', PORT))
    srv.listen(64)
    log('listening on %s' % PORT)
    while True:
        c, addr = srv.accept()
        threading.Thread(target=handle, args=(c, addr), daemon=True).start()

threading.stack_size(256 * 1024)
main()
