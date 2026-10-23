/* pftun: send hotspot (bridge100) traffic out through the VPN tun.
 * pf route-to hands client packets to our own legacy utun (no skywalk netif), we read them
 * and re-send them with a raw socket bound to the VPN utun; pf nat on the VPN utun.
 * Every packet the VPN utun sees is a fresh mbuf built from userspace: route-to straight
 * into the VPN utun panicked (nx_netif_host_output -> m_copydata, invalid mbuf chain).
 * The hotspot subnet is read from the bridge interface; the VPN utun is the one with a private IPv4
 * address. The VPN's NEPacketTunnelProvider extension is found at runtime (an .appex process holding
 * a utun control socket) unless named with -n. A jetsam kill of that extension (NOTE_EXIT_MEMORY)
 * restarts the VPN through nesessionmanager, using the configuration UUID from -u or from the
 * pftun-vpnid helper; iOS On Demand does not reconnect it while the screen is locked. A user stop
 * exits normally and is left alone.
 * The extension's default jetsam limit (50 MB) is raised to the -m value (default 128; re-applied if
 * iOS resets it), and near the limit client uploads are throttled.
 * usage: pftun run [-b bridge-if] [-i vpn-if] [-u vpn-config-uuid] [-n extension-process-name] [-m limit-mb]
 *        pftun del [-b bridge-if] */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/select.h>
#include <sys/event.h>
#include <dlfcn.h>
#include <uuid/uuid.h>
#include <sys/kern_control.h>
#include <sys/sys_domain.h>
#include <net/if.h>
#include <net/pfvar.h>
#include <ifaddrs.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/ip.h>
#include <sys/proc_info.h>
#define ANCHOR "com.apple.internet-sharing"
#define LABEL "tunshare"
#define UTUN_CONTROL_NAME "com.apple.net.utun_control"
#define UTUN_OPT_IFNAME 2
#define OWN_ADDR "198.18.0.1"   /* our utun; 198.18.0.0/15 is skipped when looking for the VPN */
#define OWN_PEER "198.18.0.2"
#define VPNID_HELPER "/var/jb/usr/local/bin/pftun-vpnid"
static int fd;
static char hs_if[IFNAMSIZ] = "bridge100";   /* Personal Hotspot bridge */
static char hs_net[32] = "";                 /* its IPv4 network address, last seen */
static int hs_bits;                          /* and prefix length */
static char vpn_if[IFNAMSIZ] = "";           /* -i: use this interface as the VPN instead of detecting it */
static char vpnid[40] = "";                  /* VPN configuration UUID (-u or helper) */
static char ne_name[64] = "";                /* VPN extension process name (-n or detected) */
static int vpn_auto = 1, ne_auto = 1;        /* the two values above are detected, not given on the command line */
static int ne_limit_mb = 128;

static void cidr(struct pf_addr_wrap *w, const char *ip, int bits) {
	w->type = PF_ADDR_ADDRMASK;
	inet_pton(AF_INET, ip, &w->v.a.addr.v4addr);
	w->v.a.mask.addr32[0] = bits ? htonl(0xffffffffu << (32 - bits)) : 0;
}

static int newticket(int act, unsigned *t) {
	struct pfioc_rule pr; memset(&pr, 0, sizeof pr);
	strlcpy(pr.anchor, ANCHOR, sizeof pr.anchor);
	pr.rule.action = act;
	if (ioctl(fd, DIOCGETRULES, &pr) < 0) return -1;
	*t = pr.ticket; return 0;
}

static int add(int act, struct pf_rule *tmpl, const char *poolif, int haspool, const char *paddr) {
	struct pfioc_pooladdr pp; memset(&pp, 0, sizeof pp);
	strlcpy(pp.anchor, ANCHOR, sizeof pp.anchor);
	if (ioctl(fd, DIOCBEGINADDRS, &pp) < 0) { perror("beginaddrs"); return -1; }
	unsigned pt = pp.ticket;
	if (haspool) {
		memset(&pp.addr, 0, sizeof pp.addr);
		pp.ticket = pt; pp.af = AF_INET;
		strlcpy(pp.addr.ifname, poolif, IFNAMSIZ);
		if (paddr) cidr(&pp.addr.addr, paddr, 32); else cidr(&pp.addr.addr, "0.0.0.0", 0);
		if (ioctl(fd, DIOCADDADDR, &pp) < 0) { perror("addaddr"); return -1; }
	}
	struct pfioc_rule pr; memset(&pr, 0, sizeof pr);
	strlcpy(pr.anchor, ANCHOR, sizeof pr.anchor);
	pr.action = PF_CHANGE_ADD_HEAD;
	pr.pool_ticket = pt;
	if (newticket(act, &pr.ticket) < 0) { perror("ticket"); return -1; }
	pr.rule = *tmpl;
	if (ioctl(fd, DIOCCHANGERULE, &pr) < 0) { perror("changerule"); return -1; }
	return 0;
}

static int del(int act) {
	int removed = 0;
	for (;;) {
		struct pfioc_rule pr; memset(&pr, 0, sizeof pr);
		strlcpy(pr.anchor, ANCHOR, sizeof pr.anchor); pr.rule.action = act;
		if (ioctl(fd, DIOCGETRULES, &pr) < 0) return removed;
		unsigned n = pr.nr, t = pr.ticket; int found = -1;
		for (unsigned i = 0; i < n; i++) {
			struct pfioc_rule r; memset(&r, 0, sizeof r);
			strlcpy(r.anchor, ANCHOR, sizeof r.anchor); r.ticket = t; r.nr = i; r.rule.action = act;
			if (ioctl(fd, DIOCGETRULE, &r) < 0) continue;
			if (!strcmp(r.rule.label, LABEL)) { found = i; break; }
		}
		if (found < 0) return removed;
		struct pfioc_rule d; memset(&d, 0, sizeof d);
		strlcpy(d.anchor, ANCHOR, sizeof d.anchor);
		d.action = PF_CHANGE_REMOVE; d.nr = found; d.ticket = t; d.rule.action = act;
		if (ioctl(fd, DIOCCHANGERULE, &d) < 0) { perror("remove"); return removed; }
		removed++;
	}
}


/* Replies from the VPN utun are forwarded to bridge100. iOS turns this on with the hotspot;
 * make sure. Returns 1 if forwarding is on. */
static int forwarding_on(void) {
	int v = 0, one = 1; size_t sz = sizeof v;
	if (sysctlbyname("net.inet.ip.forwarding", &v, &sz, NULL, 0) == 0 && v == 1) return 1;
	sysctlbyname("net.inet.ip.forwarding", NULL, NULL, &one, sizeof one);
	v = 0; sz = sizeof v;
	return sysctlbyname("net.inet.ip.forwarding", &v, &sz, NULL, 0) == 0 && v == 1;
}

static int count(int act) {
	struct pfioc_rule pr; memset(&pr, 0, sizeof pr);
	strlcpy(pr.anchor, ANCHOR, sizeof pr.anchor); pr.rule.action = act;
	if (ioctl(fd, DIOCGETRULES, &pr) < 0) return 0;
	int c = 0; unsigned n = pr.nr, t = pr.ticket;
	for (unsigned i = 0; i < n; i++) {
		struct pfioc_rule r; memset(&r, 0, sizeof r);
		strlcpy(r.anchor, ANCHOR, sizeof r.anchor); r.ticket = t; r.nr = i; r.rule.action = act;
		if (ioctl(fd, DIOCGETRULE, &r) == 0 && !strcmp(r.rule.label, LABEL)) c++;
	}
	return c;
}

/* nat on the VPN utun <tif> to <taddr>; pass in on bridge100 route-to our utun <rif> */
static int install(const char *tif, const char *taddr, const char *rif) {
	if (!hs_net[0]) return -1;
	del(PF_PASS); del(PF_NAT);
	struct pf_rule nat; memset(&nat, 0, sizeof nat);
	nat.action = PF_NAT; nat.af = AF_INET; nat.direction = PF_OUT;
	strlcpy(nat.ifname, tif, IFNAMSIZ); strlcpy(nat.label, LABEL, sizeof nat.label);
	cidr(&nat.src.addr, hs_net, hs_bits); cidr(&nat.dst.addr, "0.0.0.0", 0);
	nat.rpool.proxy_port[0] = 50001; nat.rpool.proxy_port[1] = 65535;
	nat.rpool.af = AF_INET;
	if (add(PF_NAT, &nat, tif, 1, taddr) < 0) return -1;
	struct pf_rule f; memset(&f, 0, sizeof f);
	f.action = PF_PASS; f.af = AF_INET; f.direction = PF_IN; f.quick = 1;
	f.keep_state = PF_STATE_NORMAL; f.rt = PF_ROUTETO;
	strlcpy(f.ifname, hs_if, IFNAMSIZ); strlcpy(f.label, LABEL, sizeof f.label);
	cidr(&f.src.addr, hs_net, hs_bits);
	cidr(&f.dst.addr, hs_net, hs_bits); f.dst.neg = 1;
	f.rpool.af = AF_INET;
	if (add(PF_PASS, &f, rif, 1, NULL) < 0) return -1;
	return 0;
}

/* Reads the hotspot bridge's IPv4 network into hs_net/hs_bits. Returns 1 if the bridge has an address. */
static int hs_detect(void) {
	struct ifaddrs *ifa, *p; int found = 0;
	if (getifaddrs(&ifa) < 0) return 0;
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !p->ifa_netmask || strcmp(p->ifa_name, hs_if)) continue;
		struct in_addr a = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
		uint32_t m = ntohl(((struct sockaddr_in *)p->ifa_netmask)->sin_addr.s_addr);
		struct in_addr n = { htonl(ntohl(a.s_addr) & m) };
		inet_ntop(AF_INET, &n, hs_net, sizeof hs_net);
		hs_bits = m ? 32 - __builtin_ctz(m) : 0;
		found = 1;
	}
	freeifaddrs(ifa);
	return found;
}

/* VPN tun: the -i interface, else a utun with a private IPv4 address (not 169.254/ link-local,
 * not ours); the hotspot bridge must exist */
static int find_tun(char *name, char *addr) {
	struct ifaddrs *ifa, *p; int found = 0;
	if (!hs_detect()) return 0;
	if (getifaddrs(&ifa) < 0) return 0;
	for (p = ifa; p; p = p->ifa_next) {
		if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
		if (vpn_if[0] ? strcmp(p->ifa_name, vpn_if) : strncmp(p->ifa_name, "utun", 4)) continue;
		struct in_addr a = ((struct sockaddr_in *)p->ifa_addr)->sin_addr;
		if (!vpn_if[0] && ((ntohl(a.s_addr) >> 16) == 0xa9fe || (ntohl(a.s_addr) >> 17) == (0xc6120000u >> 17))) continue;
		strlcpy(name, p->ifa_name, IFNAMSIZ); inet_ntop(AF_INET, &a, addr, 32); found = 1;
	}
	freeifaddrs(ifa);
	return found;
}

/* drop hotspot client states to the outside so existing flows pick up the new path */
static int kill_states(void) {
	struct pfioc_state_kill k; memset(&k, 0, sizeof k);
	if (!hs_net[0]) return -1;
	k.psk_af = AF_INET;
	cidr(&k.psk_src.addr, hs_net, hs_bits);
	cidr(&k.psk_dst.addr, hs_net, hs_bits); k.psk_dst.neg = 1;
	if (ioctl(fd, DIOCKILLSTATES, &k) < 0) return -1;
	return k.psk_af;
}

static void sin_set(struct sockaddr *sa, const char *ip) {
	struct sockaddr_in *s = (struct sockaddr_in *)sa;
	s->sin_len = sizeof *s; s->sin_family = AF_INET; inet_pton(AF_INET, ip, &s->sin_addr);
}

/* legacy utun (UTUN_OPT_ENABLE_NETIF not set): output is ctl_enqueuembuf, no skywalk copy */
static int utun_open(char *name) {
	int s = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
	if (s < 0) { perror("utun socket"); return -1; }
	struct ctl_info ci; memset(&ci, 0, sizeof ci);
	strlcpy(ci.ctl_name, UTUN_CONTROL_NAME, sizeof ci.ctl_name);
	if (ioctl(s, CTLIOCGINFO, &ci) < 0) { perror("CTLIOCGINFO"); return -1; }
	struct sockaddr_ctl sc; memset(&sc, 0, sizeof sc);
	sc.sc_len = sizeof sc; sc.sc_family = AF_SYSTEM; sc.ss_sysaddr = AF_SYS_CONTROL; sc.sc_id = ci.ctl_id;
	if (connect(s, (struct sockaddr *)&sc, sizeof sc) < 0) { perror("utun connect"); return -1; }
	socklen_t l = IFNAMSIZ;
	if (getsockopt(s, SYSPROTO_CONTROL, UTUN_OPT_IFNAME, name, &l) < 0) { perror("UTUN_OPT_IFNAME"); return -1; }
	int rb = 1 << 20; setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rb, sizeof rb);
	int d = socket(AF_INET, SOCK_DGRAM, 0);
	struct ifaliasreq ar; memset(&ar, 0, sizeof ar);
	strlcpy(ar.ifra_name, name, IFNAMSIZ);
	sin_set(&ar.ifra_addr, OWN_ADDR); sin_set(&ar.ifra_broadaddr, OWN_PEER); sin_set(&ar.ifra_mask, "255.255.255.255");
	if (ioctl(d, SIOCAIFADDR, &ar) < 0) { perror("SIOCAIFADDR"); return -1; }
	struct ifreq ifr; memset(&ifr, 0, sizeof ifr); strlcpy(ifr.ifr_name, name, IFNAMSIZ);
	if (ioctl(d, SIOCGIFFLAGS, &ifr) == 0) { ifr.ifr_flags |= IFF_UP; ioctl(d, SIOCSIFFLAGS, &ifr); }
	close(d);
	fcntl(s, F_SETFL, O_NONBLOCK);
	return s;
}

static void logf_(const char *fmt, const char *a, const char *b) {
	char t[32]; time_t now = time(NULL); strftime(t, sizeof t, "%F %T", localtime(&now));
	printf("%s ", t); printf(fmt, a, b); printf("\n"); fflush(stdout);
}

int proc_listallpids(void *buf, int size);
int proc_name(int pid, void *buf, uint32_t size);
int proc_pid_rusage(int pid, int flavor, void *buffer);

int memorystatus_control(uint32_t command, int32_t pid, uint32_t flags, void *buffer, size_t buffersize);
typedef struct { int32_t active, active_attr, inactive, inactive_attr; } memlimit_t;

/* iOS kills VPN extensions at 50 MB; a userspace-networking VPN core can reach that under load.
 * Root may change a process's jetsam limit; it stays fatal so a runaway extension still dies.
 * The target is ne_limit_mb (-m); 0 leaves the limit alone. */

/* current limit in MB after raising it; *was gets the old limit when it was changed */
static int ne_limit(pid_t pid, int *was) {
	memlimit_t l;
	if (memorystatus_control(8 /* GET_MEMLIMIT_PROPERTIES */, pid, 0, &l, sizeof l)) return 0;
	if (ne_limit_mb && l.active < ne_limit_mb) {
		memlimit_t n = {ne_limit_mb, 1, ne_limit_mb, 1};
		if (!memorystatus_control(7 /* SET_MEMLIMIT_PROPERTIES */, pid, 0, &n, sizeof n)) { *was = l.active; l.active = ne_limit_mb; }
	}
	return l.active;
}

/* Above THR_LOW of the limit, drop client data packets (not ACKs/DNS) with a probability rising
 * to 100% at THR_HIGH, so client TCP backs off before the extension buffers too much.
 * Measured overshoot past the full-drop point is ~3 MB. */
#define THR_LOW 0.80
#define THR_HIGH 0.86
#define THR_MINLEN 256

static long long now_ms(void) {
	struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* drop probability in per mille from the extension's phys_footprint; *peak keeps the max MB */
static unsigned throttle(pid_t pid, int limit, double *peak) {
	struct rusage_info_v4 ri;
	if (!pid || !limit || proc_pid_rusage(pid, RUSAGE_INFO_V4, &ri) < 0) return 0;
	double mb = ri.ri_phys_footprint / 1048576.0, lo = limit * THR_LOW, hi = limit * THR_HIGH;
	if (mb > *peak) *peak = mb;
	if (mb < lo) return 0;
	if (mb >= hi) return 1000;
	return 200 + (unsigned)((mb - lo) * 800 / (hi - lo));
}

static pid_t find_proc(const char *name) {
	static pid_t p[4096];
	int n = proc_listallpids(p, sizeof p);
	for (int i = 0; i < n; i++) {
		char nm[64] = "";
		if (p[i] > 0 && proc_name(p[i], nm, sizeof nm) > 0 && !strcmp(nm, name)) return p[i];
	}
	return 0;
}

int proc_pidpath(int pid, void *buf, uint32_t size);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int size);
int proc_pidfdinfo(int pid, int fd, int flavor, void *buffer, int size);

/* does the process hold a utun kernel-control socket (i.e. is it a packet tunnel)? */
static int has_utun_socket(pid_t pid) {
	static struct proc_fdinfo fds[1024];
	int n = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, sizeof fds) / (int)sizeof fds[0];
	for (int i = 0; i < n; i++) {
		struct socket_fdinfo si;
		if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
		if (proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != (int)sizeof si) continue;
		if (si.psi.soi_kind == SOCKINFO_KERN_CTL && !strcmp(si.psi.soi_proto.pri_kern_ctl.kcsi_name, UTUN_CONTROL_NAME)) return 1;
	}
	return 0;
}

/* A NEPacketTunnelProvider extension is an .appex process that holds a utun control socket.
 * Returns its pid and stores its process name in ne_name. */
static pid_t detect_ne(void) {
	static pid_t p[4096];
	int n = proc_listallpids(p, sizeof p);
	for (int i = 0; i < n; i++) {
		char path[1024] = "", nm[64] = "";
		if (p[i] <= 0 || proc_pidpath(p[i], path, sizeof path) <= 0 || !strstr(path, ".appex/")) continue;
		if (!has_utun_socket(p[i]) || proc_name(p[i], nm, sizeof nm) <= 0) continue;
		strlcpy(ne_name, nm, sizeof ne_name);
		return p[i];
	}
	return 0;
}

/* Ask the helper for the VPN configuration UUID that belongs to the extension at <pid>'s path. */
static void resolve_vpnid(pid_t pid) {
	char path[1024] = "", cmd[1200], line[64] = "";
	uuid_t u;
	if (proc_pidpath(pid, path, sizeof path) <= 0 || strchr(path, '\'')) return;
	snprintf(cmd, sizeof cmd, "%s '%s' 2>/dev/null", VPNID_HELPER, path);
	FILE *f = popen(cmd, "r");
	if (!f) return;
	if (fgets(line, sizeof line, f)) line[strcspn(line, "\r\n")] = 0;
	pclose(f);
	if (uuid_parse(line, u)) { logf_("VPN configuration UUID not found (%s%s): automatic restart disabled", ne_name, ""); return; }
	strlcpy(vpnid, line, sizeof vpnid);
	logf_("VPN configuration %s%s", vpnid, "");
}

/* private libsystem_networkextension API (what scutil --nc start uses on macOS) */
static int vpn_start(const char *id) {
	uuid_t u;
	void *(*create)(uuid_t, int) = dlsym(RTLD_DEFAULT, "ne_session_create");
	void (*start)(void *) = dlsym(RTLD_DEFAULT, "ne_session_start");
	if (uuid_parse(id, u) || !create || !start) return -1;
	void *s = create(u, 1 /* VPN */);
	if (!s) return -1;
	start(s);  /* session object is kept: releasing it right away may cancel the request */
	return 0;
}

static int run(void) {
	char own[IFNAMSIZ] = "", cur[IFNAMSIZ] = "", cur_net[40] = "", stat[160];
	int kq = kqueue(), tries = 0;
	pid_t nepid = 0;
	time_t restart_at = 0;
	int u = utun_open(own); if (u < 0) return 1;
	int rs = socket(AF_INET, SOCK_RAW, IPPROTO_RAW); if (rs < 0) { perror("raw"); return 1; }
	int on = 1, sb = 1 << 20;
	setsockopt(rs, IPPROTO_IP, IP_HDRINCL, &on, sizeof on);
	setsockopt(rs, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
	logf_("own utun %s%s", own, "");
	unsigned long long pk = 0, by = 0, bad = 0, big = 0, err = 0, thr = 0, last = 0;
	unsigned drop = 0;
	int nelim = 0;
	double peak = 0;
	long long polled = 0;
	time_t tick = 0, report = time(NULL);
	unsigned char buf[65536];
	for (;;) {
		time_t now = time(NULL);
		if (now != tick) {
			tick = now;
			char n[IFNAMSIZ], a[32];
			int up = find_tun(n, a);
			/* the dead extension's utun lingers a few seconds: done only once a new extension runs */
			if (up && nepid) { restart_at = 0; tries = 0; }
			else if (restart_at && now >= restart_at) {
				/* a start sent while the old session is still tearing down (~6 s) is ignored: retry */
				if (tries++ < 8) { logf_("restart VPN: %s%s", vpn_start(vpnid) ? "FAIL" : "sent", ""); restart_at = now + 4; }
				else { logf_("restart VPN: giving up%s%s", "", ""); restart_at = 0; }
			}
			if (!nepid) {
				if (ne_name[0]) nepid = find_proc(ne_name);
				else if (up && (nepid = detect_ne())) logf_("detected VPN extension %s%s", ne_name, "");
				if (nepid) {
					struct kevent ke;
					EV_SET(&ke, nepid, EVFILT_PROC, EV_ADD | EV_ONESHOT, NOTE_EXIT | NOTE_EXIT_DETAIL, 0, NULL);
					if (kevent(kq, &ke, 1, NULL, 0, NULL) < 0) nepid = 0;
					else if (!vpnid[0]) resolve_vpnid(nepid);
				}
			}
			nelim = 0;
			if (nepid) {
				int was = 0;
				nelim = ne_limit(nepid, &was);
				if (was) { snprintf(stat, sizeof stat, "%d -> %d MB", was, nelim); logf_("VPN extension memory limit %s%s", stat, ""); }
			}
			if (up && forwarding_on()) {
				char net[40]; snprintf(net, sizeof net, "%s/%d", hs_net, hs_bits);
				if (strcmp(n, cur) || strcmp(net, cur_net) || !count(PF_NAT) || !count(PF_PASS)) {
					unsigned idx = if_nametoindex(n);
					setsockopt(rs, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);
					int r = install(n, a, own);
					snprintf(stat, sizeof stat, "%s %s, hotspot %s, killed %d states", a, r ? "FAIL" : "ok", net, r ? 0 : kill_states());
					logf_("install %s %s", n, stat);
					if (!r) { strlcpy(cur, n, sizeof cur); strlcpy(cur_net, net, sizeof cur_net); } else cur[0] = 0;
				}
			} else if (cur[0]) { del(PF_PASS); del(PF_NAT); kill_states(); cur[0] = 0; logf_("cleared%s%s", "", ""); }
			if (now - report >= 60 && pk != last) {
				snprintf(stat, sizeof stat, "pkts=%llu bytes=%llu bad=%llu big=%llu senderr=%llu throttled=%llu ne_peak=%.1fMB",
					pk, by, bad, big, err, thr, peak);
				logf_("%s%s", stat, ""); report = now; last = pk; peak = 0;
			}
		}
		long long ms = now_ms();
		if (ms - polled >= 100) { polled = ms; drop = throttle(nepid, nelim, &peak); }
		fd_set rf; FD_ZERO(&rf); FD_SET(u, &rf); FD_SET(kq, &rf);
		struct timeval tv = {1, 0};
		if (select((u > kq ? u : kq) + 1, &rf, NULL, NULL, &tv) <= 0) continue;
		if (FD_ISSET(kq, &rf)) {
			struct kevent ke; struct timespec z = {0, 0};
			if (kevent(kq, NULL, 0, &ke, 1, &z) == 1) {
				nepid = 0;
				int jetsam = (ke.data & NOTE_EXIT_DETAIL_MASK) == NOTE_EXIT_MEMORY;
				logf_("VPN extension %s%s", jetsam ? "killed by jetsam" : "exited", jetsam && vpnid[0] ? ", restarting" : "");
				if (jetsam && vpnid[0]) { restart_at = time(NULL); tries = 0; }
				/* a detected extension may be a different VPN app next time */
				if (!jetsam) {
					if (ne_auto) ne_name[0] = 0;
					if (vpn_auto) vpnid[0] = 0;
				}
			}
		}
		for (int i = 0; i < 256; i++) {
			ssize_t n = read(u, buf, sizeof buf);
			if (n <= 0) break;
			struct ip *ip = (struct ip *)(buf + 4); size_t len = n - 4;
			if (n < 24 || ip->ip_v != 4 || ntohs(ip->ip_len) != len) { bad++; continue; }
			if (len > 1500) { big++; continue; }
			if (!cur[0]) continue;
			if (drop && len >= THR_MINLEN && arc4random_uniform(1000) < drop) { thr++; continue; }
			/* raw IP_HDRINCL wants ip_len/ip_off in host order */
			ip->ip_len = (u_short)len; ip->ip_off = ntohs(ip->ip_off);
			struct sockaddr_in to; memset(&to, 0, sizeof to);
			to.sin_len = sizeof to; to.sin_family = AF_INET; to.sin_addr = ip->ip_dst;
			if (sendto(rs, ip, len, 0, (struct sockaddr *)&to, sizeof to) < 0) err++;
			else { pk++; by += len; }
		}
	}
}

static int usage(void) {
	fprintf(stderr, "usage: pftun run [-b bridge-if] [-i vpn-if] [-u vpn-config-uuid] [-n extension-process-name] [-m limit-mb]\n"
		"       pftun del [-b bridge-if]\n");
	return 2;
}

int main(int argc, char **argv) {
	if (argc < 2) return usage();
	for (int i = 2; i < argc; i++) {
		char o = argv[i][0] == '-' ? argv[i][1] : 0;
		if (!o || argv[i][2] || i + 1 >= argc) return usage();
		const char *v = argv[++i];
		if (o == 'b') strlcpy(hs_if, v, sizeof hs_if);
		else if (o == 'i') strlcpy(vpn_if, v, sizeof vpn_if);
		else if (o == 'u') { uuid_t u; if (uuid_parse(v, u)) return usage(); strlcpy(vpnid, v, sizeof vpnid); vpn_auto = 0; }
		else if (o == 'n') { strlcpy(ne_name, v, sizeof ne_name); ne_auto = 0; }
		else if (o == 'm') ne_limit_mb = atoi(v);
		else return usage();
	}
	fd = open("/dev/pf", O_RDWR); if (fd < 0) { perror("/dev/pf"); return 1; }
	if (!strcmp(argv[1], "del")) { hs_detect(); int r = del(PF_PASS) + del(PF_NAT); printf("removed %d, killed %d states\n", r, kill_states()); return 0; }
	if (!strcmp(argv[1], "run")) return run();
	return usage();
}
