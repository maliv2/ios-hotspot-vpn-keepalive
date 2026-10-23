/* ulog [nspredicate] [flags]: stream the unified log (LoggingSupport OSLogEventLiveStream) */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <sys/time.h>
#include <time.h>
#include <dispatch/dispatch.h>
typedef void *id;
static void *o;
static id (*cls)(const char *); static void *(*sel)(const char *); static void *M;
#define MSG(r, ...) ((r (*)(__VA_ARGS__))M)
static const char *str(id s) { return s ? MSG(const char *, id, void *)(s, sel("UTF8String")) : "-"; }
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	o = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
	dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
	dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
	cls = dlsym(o, "objc_getClass"); sel = dlsym(o, "sel_registerName"); M = dlsym(o, "objc_msgSend");
	static id pred; static unsigned long long flags;
	flags = argc > 2 ? strtoull(argv[2], 0, 0) : 0;
	if (argc > 1) {
		id f = MSG(id, id, void *, const char *)(cls("NSString"), sel("stringWithUTF8String:"), argv[1]);
		pred = MSG(id, id, void *, id, id)(cls("NSPredicate"), sel("predicateWithFormat:argumentArray:"), f, NULL);
	}
	id store = MSG(id, id, void *)(cls("OSLogEventLiveStore"), sel("liveLocalStore"));
	MSG(void, id, void *, void (^)(id, id))(store, sel("prepareWithCompletionHandler:"), ^(id src, id err) {
		if (!src) { fprintf(stderr, "prepare failed: %s\n", str(MSG(id, id, void *)(err, sel("description")))); exit(1); }
		id s = MSG(id, id, void *)(cls("OSLogEventLiveStream"), sel("alloc"));
		s = MSG(id, id, void *, id)(s, sel("initWithLiveSource:"), src);
		MSG(void, id, void *, unsigned long long)(s, sel("setFlags:"), flags);
		if (pred) MSG(void, id, void *, id)(s, sel("setFilterPredicate:"), pred);
		MSG(void, id, void *, void (^)(id))(s, sel("setEventHandler:"), ^(id e) {
			struct timeval tv; gettimeofday(&tv, NULL); char t[16]; strftime(t, sizeof t, "%T", localtime(&tv.tv_sec));
			printf("%s.%03d %s[%d] %s/%s: %s\n", t, (int)(tv.tv_usec / 1000), str(MSG(id, id, void *)(e, sel("process"))),
				MSG(int, id, void *)(e, sel("processIdentifier")), str(MSG(id, id, void *)(e, sel("subsystem"))),
				str(MSG(id, id, void *)(e, sel("category"))), str(MSG(id, id, void *)(e, sel("composedMessage"))));
		});
		MSG(void, id, void *, void (^)(int, id))(s, sel("setInvalidationHandler:"), ^(int r, id x) { fprintf(stderr, "invalidated %d\n", r); exit(2); });
		MSG(void, id, void *)(s, sel("activate"));
		static id keep; keep = s;
	});
	dispatch_main();
}
