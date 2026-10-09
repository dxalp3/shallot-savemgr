/* Stubs for the main.c pieces the worker/sync/reconcile/restore code uses; includes them. */
#ifndef LS_HOST_WORKER_STUBS_H
#define LS_HOST_WORKER_STUBS_H
#include "host_stubs.h"

/* ── Stubs for the main.c pieces the worker uses ────────────────── */
typedef struct { const char *path; const char *name; } zip_file_t;

static int g_running = 0;                 /* 1 => g_running_title is the running Big App */
static const char *g_running_title = "PPSA01234";
static int64_t g_test_power_now = 0;     /* power-down budget clock (0 = time(NULL)) */
static int64_t g_test_boottime = 1000;     /* kern.boottime for the system probe */
static int g_running_after = 0;           /* >0: the title "starts" after this many more polls */
static int auto_get_running_title(char *title_id, size_t sz, int *app_id) {
    if (g_running_after > 0 && --g_running_after == 0) g_running = 1;
    if (app_id) *app_id = g_running ? 0x1234 : 0;
    if (!g_running) { title_id[0] = 0; return 0; }
    snprintf(title_id, sz, "%s", g_running_title);
    return 1;
}
static void notify(const char *fmt, ...) { (void)fmt; }
static const char *lookup_title(const char *title_id) {
    return strcmp(title_id, "PPSA24701") == 0 ? "Test Launcher" : NULL;
}
static void sanitize_filename(char *s) {
    for (char *p = s; *p; p++) if (*p == '/' || *p == ' ') *p = '_';
}
static void recursive_mkdir(const char *path) { mkdirs(path); }
static void delete_recursive(const char *path) {
    char cmd[MAX_PATH_LEN + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'/* '%s'/.[!.]* 2>/dev/null", path, path);
    if (system(cmd) != 0) { /* nothing to delete */ }
}

/* Writes a real uncompressed ZIP like upstream's zip_write_files: local
 * headers with the data-descriptor flag, sizes only in the central directory. */
static void host_put16(FILE *f, uint16_t v) { fputc(v & 0xff, f); fputc(v >> 8, f); }
static void host_put32(FILE *f, uint32_t v) { host_put16(f, v & 0xffff); host_put16(f, v >> 16); }
static int host_write_zip(const char *out, const zip_file_t *files, int nfiles) {
    FILE *o = fopen(out, "wb");
    if (!o) return -1;
    uint32_t offs[16], sizes[16];
    for (int i = 0; i < nfiles && i < 16; i++) {
        FILE *in = fopen(files[i].path, "rb");
        offs[i] = (uint32_t)ftell(o);
        uint16_t nl = (uint16_t)strlen(files[i].name);
        host_put32(o, 0x04034b50); host_put16(o, 20); host_put16(o, 0x08); host_put16(o, 0);
        host_put32(o, 0); host_put32(o, 0); host_put32(o, 0); host_put32(o, 0);
        host_put16(o, nl); host_put16(o, 0);
        fwrite(files[i].name, 1, nl, o);
        uint32_t n = 0;
        int c;
        while (in && (c = fgetc(in)) != EOF) { fputc(c, o); n++; }
        if (in) fclose(in);
        sizes[i] = n;
        host_put32(o, 0x08074b50); host_put32(o, 0); host_put32(o, n); host_put32(o, n);
    }
    uint32_t cd = (uint32_t)ftell(o);
    for (int i = 0; i < nfiles && i < 16; i++) {
        uint16_t nl = (uint16_t)strlen(files[i].name);
        host_put32(o, 0x02014b50); host_put16(o, 20); host_put16(o, 20); host_put16(o, 0x08); host_put16(o, 0);
        host_put32(o, 0); host_put32(o, 0); host_put32(o, sizes[i]); host_put32(o, sizes[i]);
        host_put16(o, nl); host_put16(o, 0); host_put16(o, 0); host_put16(o, 0); host_put16(o, 0);
        host_put32(o, 0); host_put32(o, offs[i]);
        fwrite(files[i].name, 1, nl, o);
    }
    uint32_t cd_size = (uint32_t)ftell(o) - cd;
    host_put32(o, 0x06054b50); host_put16(o, 0); host_put16(o, 0);
    host_put16(o, (uint16_t)nfiles); host_put16(o, (uint16_t)nfiles);
    host_put32(o, cd_size); host_put32(o, cd); host_put16(o, 0);
    fclose(o);
    return 0;
}

static int g_stage_fail = 0;
static const char *g_mutate_during_stage = NULL;   /* path to modify while "zipping" */
static int cloud_stage_zip_file(const char *filename, const zip_file_t *files, int nfiles,
                                char *iobuf, char *out_path, size_t out_sz, char *err, size_t err_sz) {
    (void)iobuf;
    if (g_stage_fail) { snprintf(err, err_sz, "simulated stage failure"); return -1; }
    snprintf(out_path, out_sz, "%s/%s", BACKUP_TMP_DIR, filename);
    if (host_write_zip(out_path, files, nfiles) < 0) { snprintf(err, err_sz, "zip write"); return -1; }
    if (g_mutate_during_stage) {
        FILE *f = fopen(g_mutate_during_stage, "ab"); fputs("+late-write", f); fclose(f);
        g_mutate_during_stage = NULL;
    }
    return 0;
}

/* Minimal cloud config: load fails, so legacy distribution is skipped. */
typedef struct { int enabled; char path[MAX_PATH_LEN]; } cloud_file_target_t;
typedef struct { cloud_file_target_t local, usb; int ftp_enabled, drive_enabled; } cloud_targets_config_t;
typedef struct { int queue_for_browser_drive; } cloud_auto_config_t;
typedef struct { char mode[16]; } cloud_drive_config_t;
typedef struct {
    int enabled;
    cloud_targets_config_t targets;
    cloud_auto_config_t auto_backup;
    cloud_drive_config_t drive;
} cloud_config_t;
static int cloud_load_config(cloud_config_t *c, char *err, size_t n) { (void)c; snprintf(err, n, "none"); return -1; }
static int cloud_title_is_watched(const cloud_config_t *c, const char *t) { (void)c; (void)t; return 1; }
static int cloud_copy_staged_zip_target(const char *a, const char *b, const char *c, const char *d,
                                        const char *e, char *f, size_t g, char *h, size_t i) {
    (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; (void)h; (void)i; return -1;
}
static void cloud_remote_dir(const cloud_config_t *c, const char *t, const char *u, char *o, size_t n) {
    (void)c; (void)t; (void)u; snprintf(o, n, "/");
}
static int cloud_upload_zip_file(const cloud_config_t *c, const char *d, const char *n, const char *p,
                                 char *io, char *err, size_t es) {
    (void)c; (void)d; (void)n; (void)p; (void)io; (void)err; (void)es; return -1;
}

/* ── Upstream mount / SFO / account stubs used by the restore ───── */
typedef struct sfo_info {
    char title_id[16];
    char dir_name[64];
    char main_title[128];
    char sub_title[128];
    char detail[256];
    char game_title_id[16];
    uint32_t user_param;
    uint32_t blocks;
    uint64_t account_id;
    uint32_t user_id;
    uint32_t aid_offset;
    uint32_t uid_offset;
    char format[4];
} sfo_info_t;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_mounted = 0;
static char g_mount_point[MAX_PATH_LEN] = "";
static int g_test_mount_rc = 0;          /* <0 => mount fails */
static sfo_info_t g_test_sfo;            /* what param.sfo "contains" */
static uint64_t g_test_aid = 0x1111;     /* local account id */
static int g_test_mounts = 0;
static char g_test_last_mounted[MAX_PATH_LEN];
static int g_test_live_aid_set = 0;     /* param.sfo of a copy of the live save */
static uint64_t g_test_live_aid = 0;
static uint64_t g_test_mounted_aid = 0;
#define LS_TEST_MP LS_BASE_DIR "/mount_sd"
static int mount_by_path(const char *path) {
    g_test_mounts++;
    snprintf(g_test_last_mounted, sizeof(g_test_last_mounted), "%s", path);
    if (g_test_mount_rc < 0) return g_test_mount_rc;
    uint64_t aid = g_test_sfo.account_id;
    if (g_test_live_aid_set && strstr(path, "/check_live/")) aid = g_test_live_aid;
    char buf[4096] = {0};
    FILE *f = fopen(path, "rb");
    if (f) { size_t n = fread(buf, 1, sizeof(buf) - 1, f); buf[n] = 0; fclose(f); }
    char *m = strstr(buf, "|RESIGNED=");
    if (m) aid = strtoull(m + 10, NULL, 16);
    mkdir(LS_TEST_MP, 0777);
    mkdir(LS_TEST_MP "/sce_sys", 0777);
    FILE *sf = fopen(LS_TEST_MP "/sce_sys/param.sfo", "wb");
    if (sf) { char z[0x700] = {0}; memcpy(z + 0x1B8, &aid, 8); fwrite(z, 1, sizeof(z), sf); fclose(sf); }
    g_test_mounted_aid = aid;
    g_mounted = 1;
    snprintf(g_mount_point, sizeof(g_mount_point), "%s", LS_TEST_MP);
    return 0;
}
static int unmount_save(void) {
    uint64_t aid = g_test_mounted_aid;
    FILE *sf = fopen(LS_TEST_MP "/sce_sys/param.sfo", "rb");
    if (sf) { fseek(sf, 0x1B8, SEEK_SET); if (fread(&aid, 8, 1, sf) != 1) aid = g_test_mounted_aid; fclose(sf); }
    if (aid != g_test_mounted_aid) {
        FILE *f = fopen(g_test_last_mounted, "ab");
        if (f) { fprintf(f, "|RESIGNED=%llx", (unsigned long long)aid); fclose(f); }
    }
    g_mounted = 0; g_mount_point[0] = 0;
    return 0;
}
static int g_test_sfo_dir_from_path = 0;   /* 1: dir_name = mounted image name without sdimg_ */
static int parse_sfo(const char *path, sfo_info_t *info) {
    *info = g_test_sfo;
    if (g_test_sfo_dir_from_path) {
        const char *b = strrchr(g_test_last_mounted, '/');
        b = b ? b + 1 : g_test_last_mounted;
        if (strncmp(b, "sdimg_", 6) == 0) b += 6;
        snprintf(info->dir_name, sizeof(info->dir_name), "%s", b);
    }
    FILE *sf = fopen(path, "rb");
    if (sf) { fseek(sf, 0x1B8, SEEK_SET); if (fread(&info->account_id, 8, 1, sf) != 1) info->account_id = 0; fclose(sf); }
    return 0;
}
static uint64_t get_account_id_for_user(const char *uid_hex) { (void)uid_hex; return g_test_aid; }

#include "../../src/lansync_crypto.inc.c"
#include "../../src/lansync_worker.inc.c"
#include "../../src/lansync_blocks.inc.c"
#include "../../src/lansync_sync.inc.c"
#include "../../src/lansync_reconcile.inc.c"
#include "../../src/lansync_restore.inc.c"
#include "../../src/lansync_recovery.inc.c"
#include "../../src/lansync_system.inc.c"
#endif
