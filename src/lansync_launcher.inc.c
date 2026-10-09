/*
 * Home-screen tile that opens Shallot's web UI in the PS5 browser.
 * Included from main.c after lansync_pclink.inc.c.
 *
 * The same mechanism other homebrew uses: an app folder under /user/app/
 * with sce_sys/param.json + icon0.png, registered with AppInstUtil. The
 * param.json "deeplinkUri" makes the tile open a URL instead of running
 * code, so nothing executes when it is launched; Shallot itself must be
 * running for the page to load.
 *   GET  /api/system/launcher                 {installed, title_id, last_rc}
 *   POST /api/system/launcher?action=install|remove   (also switches config home_tile)
 * With home_tile on (default) the tile is added at start when it is missing.
 * AppInstUtil is resolved at run time (no extra link library).
 */

#define LS_LAUNCHER_TID "SHAL00001"
#define LS_LAUNCHER_DIR "/user/app/" LS_LAUNCHER_TID

#ifndef LS_HOST_TEST
#include "launcher_icon.h"
#endif

static int g_ls_launcher_rc = 0;            /* last AppInstUtil result (0 = ok / not tried) */
static char g_ls_launcher_msg[160] = "";

static const char *ls_launcher_param_json(char *out, size_t out_sz) {
    /* applicationCategoryType 65536: a media app, not a game (without it the
     * tile was filed under games on the owner's PS5). */
    snprintf(out, out_sz,
             "{\n  \"applicationCategoryType\": 65536,\n  \"titleId\": \"%s\",\n  \"deeplinkUri\": \"http://127.0.0.1:%d/\",\n"
             "  \"localizedParameters\": {\n    \"defaultLanguage\": \"en-US\",\n"
             "    \"en-US\": { \"titleName\": \"Shallot SaveMgr\" },\n"
             "    \"zh-Hans\": { \"titleName\": \"Shallot \\u5b58\\u6863\\u7ba1\\u7406\" },\n"
             "    \"zh-Hant\": { \"titleName\": \"Shallot \\u5b58\\u6a94\\u7ba1\\u7406\" }\n  }\n}\n",
             LS_LAUNCHER_TID, PORT);
    return out;
}

static int ls_launcher_installed(void) {
    struct stat st;
    return stat(LS_LAUNCHER_DIR "/sce_sys/param.json", &st) == 0;
}

/* 1 if the installed param.json is not what this build writes (older tile). */
static int ls_launcher_outdated(void) {
    char want[640], have[700] = "";
    ls_launcher_param_json(want, sizeof(want));
    FILE *f = fopen(LS_LAUNCHER_DIR "/sce_sys/param.json", "rb");
    if (!f) return 0;
    size_t n = fread(have, 1, sizeof(have) - 1, f);
    fclose(f);
    have[n] = 0;
    return strcmp(want, have) != 0;
}

static int ls_write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

#ifndef LS_HOST_TEST
typedef int (*ls_aiu_init_fn)(void);
typedef int (*ls_aiu_install_fn)(const char *title_id, const char *dir, void *opt);
typedef int (*ls_aiu_uninstall_fn)(const char *title_id);
typedef int (*ls_aiu_install_all_fn)(void *opt);
static ls_aiu_install_all_fn g_ls_aiu_all;    /* fallback: registers every folder under /user/app */

int sceKernelLoadStartModule(const char *path, size_t argc, const void *argv, uint32_t flags, void *opt, int *res);
int sceKernelDlsym(int handle, const char *sym, void **addr);
static char g_ls_aiu_diag[160] = "";

/* Resolves AppInstUtil once. The library is not loaded into Shallot by
 * default (owner's PS5: dlopen by name failed), so it is loaded from
 * /system/common/lib first; symbols by name, install also by NID through
 * the loaded module (as other homebrew does). */
static void ls_aiu_resolve(ls_aiu_init_fn *init, ls_aiu_install_fn *inst, ls_aiu_uninstall_fn *uninst) {
    static int done = 0;
    static ls_aiu_init_fn s_init;
    static ls_aiu_install_fn s_inst;
    static ls_aiu_uninstall_fn s_uninst;
    if (!done) {
        done = 1;
        int mod = -1, mres = 0, hrc = -1;
        void *h = dlopen("libSceAppInstUtil.sprx", RTLD_LAZY);
        if (h) {
            s_init = (ls_aiu_init_fn)dlsym(h, "sceAppInstUtilInitialize");
            s_inst = (ls_aiu_install_fn)dlsym(h, "sceAppInstUtilAppInstallTitleDir");
            s_uninst = (ls_aiu_uninstall_fn)dlsym(h, "sceAppInstUtilAppUnInstall");
            g_ls_aiu_all = (ls_aiu_install_all_fn)dlsym(h, "sceAppInstUtilAppInstallAll");
        }
        if (!s_init || !s_inst || !s_uninst) {
            mod = sceKernelLoadStartModule("/system/common/lib/libSceAppInstUtil.sprx", 0, NULL, 0, NULL, &mres);
            if (mod >= 0) {
                void *a = NULL;
                if (!s_init && sceKernelDlsym(mod, "sceAppInstUtilInitialize", &a) == 0) s_init = (ls_aiu_init_fn)a;
                a = NULL;
                if (!s_inst && sceKernelDlsym(mod, "sceAppInstUtilAppInstallTitleDir", &a) == 0) s_inst = (ls_aiu_install_fn)a;
                a = NULL;
                if (!s_uninst && sceKernelDlsym(mod, "sceAppInstUtilAppUnInstall", &a) == 0) s_uninst = (ls_aiu_uninstall_fn)a;
                a = NULL;
                if (!g_ls_aiu_all && sceKernelDlsym(mod, "sceAppInstUtilAppInstallAll", &a) == 0) g_ls_aiu_all = (ls_aiu_install_all_fn)a;
            }
        }
        uint32_t handle = 0;
        if ((!s_init || !s_inst || !s_uninst) && (hrc = kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle)) == 0) {
            if (!s_init) s_init = (ls_aiu_init_fn)kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilInitialize");
            if (!s_inst) s_inst = (ls_aiu_install_fn)kernel_dynlib_resolve(-1, handle, "Wudg3Xe3heE");
            if (!s_uninst) s_uninst = (ls_aiu_uninstall_fn)kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppUnInstall");
            if (!g_ls_aiu_all) g_ls_aiu_all = (ls_aiu_install_all_fn)kernel_dynlib_dlsym(-1, handle, "sceAppInstUtilAppInstallAll");
        }
        snprintf(g_ls_aiu_diag, sizeof(g_ls_aiu_diag), "dlopen %s, load 0x%08x, handle %d; init %s install %s install-all %s uninstall %s",
                 h ? "ok" : "no", (unsigned)mod, hrc, s_init ? "ok" : "-", s_inst ? "ok" : "-", g_ls_aiu_all ? "ok" : "-",
                 s_uninst ? "ok" : "-");
        ls_event("LAUNCHER_LOADER", NULL, "%s", g_ls_aiu_diag);
    }
    *init = s_init; *inst = s_inst; *uninst = s_uninst;
}
#endif

/* 0 = ok; otherwise err holds the reason. */
static int ls_launcher_install(char *err, size_t err_sz) {
#ifdef LS_HOST_TEST
    snprintf(err, err_sz, "not available in host tests");
    return -1;
#else
    ls_aiu_init_fn init; ls_aiu_install_fn inst; ls_aiu_uninstall_fn uninst;
    ls_aiu_resolve(&init, &inst, &uninst);
    if (!init || (!inst && !g_ls_aiu_all)) {
        snprintf(err, err_sz, "the PS5 app installer is not available to Shallot (%s)", g_ls_aiu_diag);
        return -1;
    }
    int rc = init();
    if (rc != 0) { snprintf(err, err_sz, "app installer init failed (0x%08x)", (unsigned)rc); g_ls_launcher_rc = rc; return -1; }
    if (uninst) uninst(LS_LAUNCHER_TID);          /* replace an older tile cleanly */
    mkdir("/user/app", 0755);
    mkdir(LS_LAUNCHER_DIR, 0755);
    mkdir(LS_LAUNCHER_DIR "/sce_sys", 0755);
    char pj[640];
    ls_launcher_param_json(pj, sizeof(pj));
    if (ls_write_file(LS_LAUNCHER_DIR "/sce_sys/param.json", pj, strlen(pj)) < 0 ||
        ls_write_file(LS_LAUNCHER_DIR "/sce_sys/icon0.png", assets_icon0_png, assets_icon0_png_len) < 0) {
        snprintf(err, err_sz, "cannot write the tile files (errno %d)", errno);
        return -1;
    }
    /* 13.60 (owner's PS5) exports no InstallTitleDir: register every app
     * folder under /user/app instead, as other homebrew falls back to. */
    rc = inst ? inst(LS_LAUNCHER_TID, "/user/app/", NULL) : g_ls_aiu_all(NULL);
    g_ls_launcher_rc = rc;
    if (rc != 0) {
        snprintf(err, err_sz, "the PS5 refused to add the tile (%s 0x%08x)", inst ? "install" : "install-all", (unsigned)rc);
        return -1;
    }
    return 0;
#endif
}

static int ls_launcher_remove(char *err, size_t err_sz) {
#ifdef LS_HOST_TEST
    snprintf(err, err_sz, "not available in host tests");
    return -1;
#else
    ls_aiu_init_fn init; ls_aiu_install_fn inst; ls_aiu_uninstall_fn uninst;
    ls_aiu_resolve(&init, &inst, &uninst);
    if (!init || !uninst) { snprintf(err, err_sz, "the PS5 app installer is not available to Shallot (%s)", g_ls_aiu_diag); return -1; }
    init();
    int rc = uninst(LS_LAUNCHER_TID);
    g_ls_launcher_rc = rc;
    /* The uninstaller normally removes the folder; clean up what is left. */
    unlink(LS_LAUNCHER_DIR "/sce_sys/param.json");
    unlink(LS_LAUNCHER_DIR "/sce_sys/icon0.png");
    rmdir(LS_LAUNCHER_DIR "/sce_sys");
    rmdir(LS_LAUNCHER_DIR);
    if (rc != 0 && ls_launcher_installed()) { snprintf(err, err_sz, "the PS5 refused to remove the tile (0x%08x)", (unsigned)rc); return -1; }
    return 0;
#endif
}

/* The owner's choice sticks: Add turns the automatic tile on, Remove off. */
static void ls_launcher_remember(int on) {
    pthread_mutex_lock(&g_ls_cfg_mtx);
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (cfg.home_tile != on) {
        cfg.home_tile = on;
        ls_save_config(&cfg);
    }
    pthread_mutex_unlock(&g_ls_cfg_mtx);
}

/* At start: add the tile if it is missing (config home_tile, default on).
 * In its own thread, after the system had a moment; one event per start. */
static void *ls_launcher_auto_thread(void *arg) {
    (void)arg;
    sleep(20);
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.home_tile || (ls_launcher_installed() && !ls_launcher_outdated())) return NULL;
    char err[200] = "";
    int rc = ls_launcher_install(err, sizeof(err));
    snprintf(g_ls_launcher_msg, sizeof(g_ls_launcher_msg), "%s", rc == 0 ? "added at start (or updated)" : err);
    ls_event(rc == 0 ? "LAUNCHER_CHANGED" : "LAUNCHER_FAILED", NULL, "automatic home-screen tile: %s (rc 0x%08x)",
             g_ls_launcher_msg, (unsigned)g_ls_launcher_rc);
    return NULL;
}

static void ls_launcher_autostart(void) {
#ifndef LS_HOST_TEST
    if (!g_ls_ready) return;
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&tid, &attr, ls_launcher_auto_thread, NULL) == 0) pthread_detach(tid);
    pthread_attr_destroy(&attr);
#endif
}

static int ls_launcher_route(int sock, const char *method, const char *url) {
    if (strncmp(url, "/api/system/launcher", 20) != 0) return 0;
    if (strcmp(method, "POST") == 0) {
        char err[200] = "";
        int install = strstr(url, "action=install") != NULL, remove = strstr(url, "action=remove") != NULL;
        if (!install && !remove) { http_json(sock, "{\"error\":\"action must be install or remove\"}"); return 1; }
        int rc = install ? ls_launcher_install(err, sizeof(err)) : ls_launcher_remove(err, sizeof(err));
        ls_launcher_remember(install);
        snprintf(g_ls_launcher_msg, sizeof(g_ls_launcher_msg), "%s", rc == 0 ? (install ? "added" : "removed") : err);
        ls_event(rc == 0 ? "LAUNCHER_CHANGED" : "LAUNCHER_FAILED", NULL, "%s: %s (rc 0x%08x)",
                 install ? "add home-screen tile" : "remove home-screen tile", g_ls_launcher_msg, (unsigned)g_ls_launcher_rc);
        char j[400], ee[300];
        json_escape(err, ee, sizeof(ee));
        if (rc == 0) snprintf(j, sizeof(j), "{\"ok\":true,\"installed\":%s}", ls_launcher_installed() ? "true" : "false");
        else snprintf(j, sizeof(j), "{\"error\":\"%s\"}", ee);
        http_json(sock, j);
        return 1;
    }
    char j[300], em[340];
    json_escape(g_ls_launcher_msg, em, sizeof(em));
    ls_config_t cfg;
    ls_load_config(&cfg);
    snprintf(j, sizeof(j), "{\"installed\":%s,\"auto\":%s,\"title_id\":\"%s\",\"last_rc\":%d,\"last\":\"%s\"}",
             ls_launcher_installed() ? "true" : "false", cfg.home_tile ? "true" : "false", LS_LAUNCHER_TID, g_ls_launcher_rc, em);
    http_json(sock, j);
    return 1;
}
