#!/var/jb/usr/bin/python3
# Prevents CPU/Wi-Fi sleep (IOPMAssertion) while on AC power or while the Personal Hotspot interface
# exists. Also supervises the hotspot helper daemons (socksd.py, hshold.py) as child processes.
import ctypes, os, subprocess, time

import hotspot

CF = ctypes.CDLL('/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation')
IOK = ctypes.CDLL('/System/Library/Frameworks/IOKit.framework/IOKit')
CF.CFStringCreateWithCString.restype = ctypes.c_void_p
CF.CFStringCreateWithCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32]
CF.CFStringGetCString.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_long, ctypes.c_uint32]
CF.CFRelease.argtypes = [ctypes.c_void_p]
IOK.IOPMAssertionCreateWithName.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32)]
IOK.IOPMAssertionRelease.argtypes = [ctypes.c_uint32]
IOK.IOPSCopyPowerSourcesInfo.restype = ctypes.c_void_p
IOK.IOPSGetProvidingPowerSourceType.restype = ctypes.c_void_p
IOK.IOPSGetProvidingPowerSourceType.argtypes = [ctypes.c_void_p]
LIBC = ctypes.CDLL(None)
LIBC.memorystatus_control.argtypes = [ctypes.c_uint32, ctypes.c_int32, ctypes.c_uint32, ctypes.c_void_p, ctypes.c_size_t]

# launchd gives /var/jb daemons a 6 MB jetsam limit (JetsamProperties in the plist is ignored);
# python3 + ctypes does not fit and was SIGKILLed every few minutes, taking the children (socksd, hshold) with it.
LIMIT_MB = 32

def raise_limit():
    lim = (ctypes.c_int32 * 4)()
    if LIBC.memorystatus_control(8, os.getpid(), 0, lim, ctypes.sizeof(lim)) == 0 and lim[0] >= LIMIT_MB:
        return
    new = (ctypes.c_int32 * 4)(LIMIT_MB, 1, LIMIT_MB, 1)  # GET/SET_MEMLIMIT_PROPERTIES, fatal
    r = LIBC.memorystatus_control(7, os.getpid(), 0, new, ctypes.sizeof(new))
    log('memory limit %s -> %s MB r=%s' % (lim[0], LIMIT_MB, r))

def cfs(s):
    return CF.CFStringCreateWithCString(None, s.encode(), 0x08000100)

def log(m):
    print(time.strftime('%F %T'), m, flush=True)

def on_ac():
    try:
        info = IOK.IOPSCopyPowerSourcesInfo()
        try:
            t = IOK.IOPSGetProvidingPowerSourceType(info)
            buf = ctypes.create_string_buffer(64)
            CF.CFStringGetCString(t, buf, 64, 0x08000100)
            return buf.value.decode() == 'AC Power'
        finally:
            CF.CFRelease(info)
    except Exception as e:
        log('power source check failed: %r' % e)
        return True

TYPES = ('NoIdleSleepAssertion', 'PreventUserIdleSystemSleep', 'PreventSystemSleep')
held = []

def acquire():
    for t in TYPES:
        aid = ctypes.c_uint32()
        r = IOK.IOPMAssertionCreateWithName(cfs(t), 255, cfs('keepawake'), ctypes.byref(aid))
        log('assertion %s -> %s id=%s' % (t, r, aid.value))
        if r == 0:
            held.append(aid.value)

def release():
    while held:
        IOK.IOPMAssertionRelease(held.pop())
    log('released')

# KEEPAWAKE_ON_AC=0: hold the assertions only while the hotspot is up
NO_AC = os.environ.get('KEEPAWAKE_ON_AC', '1') == '0'
# KEEPAWAKE_CHILDREN: comma-separated helper scripts to supervise (e.g. "hshold" to skip the proxy)
CHILDREN = tuple(c for c in os.environ.get('KEEPAWAKE_CHILDREN', 'socksd,hshold').split(',') if c)
children = {}

def supervise():
    # launchd's low memory limit kills the proxy when it runs as its own daemon, so it runs as a child of this process.
    for n in CHILDREN:
        c = children.get(n)
        if c is None or c.poll() is not None:
            if c is not None:
                log('%s exited rc=%s, respawning' % (n, c.returncode))
            children[n] = subprocess.Popen(['/var/jb/usr/bin/python3', '/var/jb/usr/local/bin/%s.py' % n],
                                           stdout=open('/var/jb/var/log/%s.log' % n, 'a'), stderr=subprocess.STDOUT)

log('start')
while True:
    raise_limit()
    supervise()
    ac = on_ac() and not NO_AC
    hs = hotspot.exists()
    want = ac or hs
    if want and not held:
        log('acquiring (ac=%s hotspot=%s)' % (ac, hs))
        acquire()
    elif not want and held:
        log('releasing (battery, no hotspot)')
        release()
    time.sleep(20)
