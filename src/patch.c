// BimmerCode 4.4 unlock patch — v8
//
// What's new vs v7:
//   * Adds NSBundle.localizedStringForKey:value:table: hook to replace
//     specific localization keys with custom strings (signature easter egg).
//     Currently replaces "MY_PURCHASES" → "Cracked by @xiaochoumao 2026/5/13".
//     All other keys are passed through to the original implementation
//     untouched, so system framework localization stays intact.
//   * Keeps everything from v7: image-filtered class scan, narrow patterns,
//     triple-channel logging (stderr + syslog + /tmp/bimmercode_mod.log).

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <syslog.h>
#include <pthread.h>
#include <unistd.h>

typedef signed char BOOL_t;
typedef void *id;
typedef void *Class_t;
typedef void *SEL_t;
typedef void *Method_t;
typedef BOOL_t (*IMP_t)(id, SEL_t);

// objc runtime
static Class_t (*p_objc_getClass)(const char *);
static SEL_t   (*p_sel_registerName)(const char *);
static const char *(*p_sel_getName)(SEL_t);
static Method_t (*p_class_getInstanceMethod)(Class_t, SEL_t);
static IMP_t   (*p_method_setImplementation)(Method_t, IMP_t);
static BOOL_t  (*p_class_addMethod)(Class_t, SEL_t, IMP_t, const char *);
static int     (*p_objc_getClassList)(Class_t *, int);
static const char *(*p_class_getName)(Class_t);
static Method_t *(*p_class_copyMethodList)(Class_t, unsigned int *);
static SEL_t   (*p_method_getName)(Method_t);
static const char *(*p_method_getTypeEncoding)(Method_t);
static IMP_t   (*p_method_getImplementation)(Method_t);
static Class_t (*p_object_getClass)(id);

// objc_msgSend trampoline (we'll cast it per call site)
static void   *p_objc_msgSend = NULL;

// ---------- triple-channel log ----------
static FILE *bm_logfile = NULL;
static pthread_mutex_t bm_log_mu = PTHREAD_MUTEX_INITIALIZER;

static void bm_log(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&bm_log_mu);
    fprintf(stderr, "%s\n", buf); fflush(stderr);
    syslog(LOG_NOTICE, "%s", buf);
    if (!bm_logfile) bm_logfile = fopen("/tmp/bimmercode_mod.log", "a");
    if (bm_logfile) { fprintf(bm_logfile, "%s\n", buf); fflush(bm_logfile); }
    pthread_mutex_unlock(&bm_log_mu);
}

// ============================================================
// Layer 1 (carried over from v7): purchase-gate hooks
// ============================================================
static int contains(const char *s, const char *n) { return strstr(s, n) != NULL; }
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int classify(const char *sel) {
    // === Force NO ===
    if (strcmp(sel, "isAdapterRestrictionEnabled") == 0) return -1;
    if (strcmp(sel, "isFreeTrial") == 0)                 return -1;
    if (strcmp(sel, "needsPresentSpeedlockUnlockMessage") == 0) return -1;

    // === Force YES ===
    if (strcmp(sel, "isAdvancedAdapter") == 0)           return +1;
    if (contains(sel, "FullVersion") && contains(sel, "Purchased")) return +1;
    if (contains(sel, "UnlockCoding") && contains(sel, "Purchased")) return +1;
    if (strcmp(sel, "hasAnyPurchase") == 0)              return +1;
    if (strcmp(sel, "isPurchased") == 0)                 return +1;
    if (strcmp(sel, "isUnlocked") == 0)                  return +1;
    if (contains(sel, "ProMode") && (starts_with(sel, "is") || starts_with(sel, "has"))) return +1;

    return 0;
}

static BOOL_t impl_YES(id self, SEL_t _cmd) { (void)self; (void)_cmd; return 1; }
static BOOL_t impl_NO (id self, SEL_t _cmd) { (void)self; (void)_cmd; return 0; }

static int is_bimmercode_image(const char *path) {
    if (!path) return 0;
    if (strncmp(path, "/usr/", 5) == 0) return 0;
    if (strncmp(path, "/System/", 8) == 0) return 0;
    if (strstr(path, "/BimmerCode") != NULL || strstr(path, "/BimmerLinkCoder") != NULL) return 1;
    return 0;
}

static int total_patched = 0;
static int yes_patched = 0;
static int no_patched = 0;
static int skipped_foreign_image = 0;

static void scan_and_patch_purchase_gates(void) {
    int n = p_objc_getClassList(NULL, 0);
    if (n <= 0) return;
    Class_t *all = (Class_t *)malloc(sizeof(Class_t) * (unsigned)n);
    n = p_objc_getClassList(all, n);

    for (int i = 0; i < n; ++i) {
        Class_t cls = all[i];
        const char *cname = p_class_getName(cls);
        if (!cname) continue;

        unsigned int mcount = 0;
        Method_t *methods = p_class_copyMethodList(cls, &mcount);
        if (!methods) continue;
        for (unsigned int j = 0; j < mcount; ++j) {
            Method_t m = methods[j];
            SEL_t sel = p_method_getName(m);
            const char *sname = p_sel_getName(sel);
            if (!sname) continue;
            const char *types = p_method_getTypeEncoding(m);
            if (!types) continue;
            if (types[0] != 'B' && types[0] != 'c') continue;
            if (strlen(types) > 9) continue;

            int verdict = classify(sname);
            if (verdict == 0) continue;

            IMP_t cur_imp = p_method_getImplementation(m);
            Dl_info dl;
            if (dladdr((void *)cur_imp, &dl) == 0) continue;
            if (!is_bimmercode_image(dl.dli_fname)) {
                skipped_foreign_image++;
                continue;
            }

            IMP_t newimp = (verdict > 0) ? impl_YES : impl_NO;
            p_method_setImplementation(m, newimp);
            if (verdict > 0) yes_patched++; else no_patched++;
            total_patched++;
            bm_log("[BimmerMod] %s -[%s %s]  (image=%s)",
                   (verdict > 0) ? "YES" : " NO",
                   cname, sname,
                   strrchr(dl.dli_fname, '/') ? strrchr(dl.dli_fname, '/') + 1 : dl.dli_fname);
        }
        free(methods);
    }
    free(all);
}

// ============================================================
// Layer 2 (NEW in v8): localization-string replacement hook
// ============================================================
//
// We hook -[NSBundle localizedStringForKey:value:table:] and substitute
// the return value for a specific allowlist of keys. All other keys
// are forwarded to the original implementation unchanged.

typedef id (*localizedStringForKeyIMP)(id self, SEL_t _cmd, id key, id value, id table);
static localizedStringForKeyIMP orig_localizedString = NULL;

// One pre-built NSString instance per replacement, prepared in ctor.
static id g_replacement_my_purchases = NULL;
static SEL_t g_sel_utf8 = NULL;

// Substitution table.
struct subst { const char *key; id *replacement_ptr; };
static struct subst g_substitutions[] = {
    { "MY_PURCHASES", &g_replacement_my_purchases },
    { NULL, NULL }
};

static id hooked_localizedStringForKey(id self, SEL_t _cmd, id key, id value, id table) {
    if (key && p_objc_msgSend) {
        // const char *k = [key UTF8String];
        const char *(*utf8_fn)(id, SEL_t) = (const char *(*)(id, SEL_t))p_objc_msgSend;
        const char *k = utf8_fn(key, g_sel_utf8);
        if (k) {
            for (int i = 0; g_substitutions[i].key != NULL; ++i) {
                if (strcmp(k, g_substitutions[i].key) == 0 &&
                    g_substitutions[i].replacement_ptr &&
                    *g_substitutions[i].replacement_ptr) {
                    return *g_substitutions[i].replacement_ptr;
                }
            }
        }
    }
    // forward everything else to original
    if (orig_localizedString) {
        return orig_localizedString(self, _cmd, key, value, table);
    }
    return value ? value : key;  // fallback
}

// Create and retain a new NSString with the given UTF-8 contents.
// Returns nil if NSString isn't available yet.
static id make_nsstring(const char *utf8) {
    if (!p_objc_msgSend) return NULL;
    Class_t NSStringClass = p_objc_getClass("NSString");
    if (!NSStringClass) return NULL;
    SEL_t sel_new = p_sel_registerName("stringWithUTF8String:");
    id (*newStr)(Class_t, SEL_t, const char *) =
        (id (*)(Class_t, SEL_t, const char *))p_objc_msgSend;
    id str = newStr(NSStringClass, sel_new, utf8);
    if (!str) return NULL;
    // retain so it survives across autorelease pool drains
    SEL_t sel_retain = p_sel_registerName("retain");
    id (*retain_fn)(id, SEL_t) = (id (*)(id, SEL_t))p_objc_msgSend;
    retain_fn(str, sel_retain);
    return str;
}

static void install_localization_hook(void) {
    p_objc_msgSend = dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!p_objc_msgSend) {
        bm_log("[BimmerMod] localization hook: objc_msgSend not found");
        return;
    }
    g_sel_utf8 = p_sel_registerName("UTF8String");

    // Build replacement strings
    g_replacement_my_purchases = make_nsstring("Cracked by @xiaochoumao 2026/5/13");
    if (!g_replacement_my_purchases) {
        bm_log("[BimmerMod] localization hook: failed to build NSString");
        return;
    }

    Class_t NSBundleClass = p_objc_getClass("NSBundle");
    if (!NSBundleClass) {
        bm_log("[BimmerMod] localization hook: NSBundle class not found");
        return;
    }
    SEL_t selLoc = p_sel_registerName("localizedStringForKey:value:table:");
    Method_t m = p_class_getInstanceMethod(NSBundleClass, selLoc);
    if (!m) {
        bm_log("[BimmerMod] localization hook: NSBundle method not found");
        return;
    }

    orig_localizedString = (localizedStringForKeyIMP)p_method_getImplementation(m);
    p_method_setImplementation(m, (IMP_t)(void *)hooked_localizedStringForKey);

    bm_log("[BimmerMod] hooked -[NSBundle localizedStringForKey:value:table:]");
    for (int i = 0; g_substitutions[i].key != NULL; ++i) {
        bm_log("[BimmerMod]   substitution: '%s' -> custom NSString", g_substitutions[i].key);
    }
}

__attribute__((used, section("__TEXT,__cstring")))
static const char watermark[] =
    "Bimmercode 4.4 Mod v8 | hooks + localization easter-egg";

__attribute__((constructor))
static void bimmer_mod_init(void) {
    p_objc_getClass            = dlsym(RTLD_DEFAULT, "objc_getClass");
    p_sel_registerName         = dlsym(RTLD_DEFAULT, "sel_registerName");
    p_sel_getName              = dlsym(RTLD_DEFAULT, "sel_getName");
    p_class_getInstanceMethod  = dlsym(RTLD_DEFAULT, "class_getInstanceMethod");
    p_method_setImplementation = dlsym(RTLD_DEFAULT, "method_setImplementation");
    p_class_addMethod          = dlsym(RTLD_DEFAULT, "class_addMethod");
    p_objc_getClassList        = dlsym(RTLD_DEFAULT, "objc_getClassList");
    p_class_getName            = dlsym(RTLD_DEFAULT, "class_getName");
    p_class_copyMethodList     = dlsym(RTLD_DEFAULT, "class_copyMethodList");
    p_method_getName           = dlsym(RTLD_DEFAULT, "method_getName");
    p_method_getTypeEncoding   = dlsym(RTLD_DEFAULT, "method_getTypeEncoding");
    p_method_getImplementation = dlsym(RTLD_DEFAULT, "method_getImplementation");
    p_object_getClass          = dlsym(RTLD_DEFAULT, "object_getClass");

    openlog("BimmerMod", LOG_PID | LOG_CONS, LOG_USER);

    if (!p_objc_getClass || !p_method_setImplementation || !p_objc_getClassList ||
        !p_class_copyMethodList || !p_method_getName || !p_method_getTypeEncoding ||
        !p_method_getImplementation || !p_class_getName) {
        bm_log("[BimmerMod] FATAL: objc runtime symbols missing");
        return;
    }

    // fresh log per launch
    FILE *clr = fopen("/tmp/bimmercode_mod.log", "w");
    if (clr) { fprintf(clr, "=== BimmerMod load pid=%ld ===\n", (long)getpid()); fclose(clr); }

    bm_log("[BimmerMod] %s", watermark);
    bm_log("[BimmerMod] pid=%d, starting BimmerCode-image-only scan", getpid());

    scan_and_patch_purchase_gates();
    install_localization_hook();

    bm_log("[BimmerMod] === scan summary: total=%d (YES=%d, NO=%d), skipped foreign images=%d ===",
           total_patched, yes_patched, no_patched, skipped_foreign_image);
}
