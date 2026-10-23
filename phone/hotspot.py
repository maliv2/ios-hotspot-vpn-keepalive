# Shared helpers: find the Personal Hotspot bridge interface and its IPv4 network.
# The interface name is bridge100 on every iOS version seen so far; override with HOTSPOT_IF.
import fcntl, ipaddress, os, socket, struct

IFACE = os.environ.get('HOTSPOT_IF', 'bridge100')
SIOCGIFADDR = 0xc0206921     # Darwin: _IOWR('i', 33, struct ifreq)
SIOCGIFNETMASK = 0xc0206925  # Darwin: _IOWR('i', 37, struct ifreq)


def exists():
    try:
        return any(n == IFACE for _, n in socket.if_nameindex())
    except Exception:
        return False


def _ioctl_addr(req):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        r = fcntl.ioctl(s.fileno(), req, struct.pack('16s16x', IFACE.encode()))
    finally:
        s.close()
    return socket.inet_ntoa(r[20:24])


def address():
    """IPv4 address of the hotspot interface (the phone's gateway address), or None."""
    try:
        return _ioctl_addr(SIOCGIFADDR)
    except Exception:
        return None


def network():
    """ipaddress.IPv4Network of the hotspot interface, or None while the hotspot is off."""
    try:
        return ipaddress.ip_network('%s/%s' % (_ioctl_addr(SIOCGIFADDR), _ioctl_addr(SIOCGIFNETMASK)), strict=False)
    except Exception:
        return None
