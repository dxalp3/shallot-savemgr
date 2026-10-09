/*
 * Shared stubs for host tests (Linux/WSL, native compiler). Replaces the
 * main.c helpers the LAN sync include files depend on and points every path
 * at /tmp/lansync-test. Each test .c includes this once.
 */
#ifndef LS_HOST_STUBS_H
#define LS_HOST_STUBS_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <time.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <pthread.h>
#include "sqlite3.h"

#define LS_HOST_TEST 1
#define LS_BASE_DIR "/tmp/lansync-test"
#define LS_USER_HOME LS_BASE_DIR "/user/home"
#define LS_SYSTEM_DATA LS_BASE_DIR "/system_data"
#define BACKUP_TMP_DIR LS_BASE_DIR "/tmp"
#define CLOUD_QUEUE_DIR LS_BASE_DIR "/cloud_queue"
#define BUF_SIZE (64 * 1024)
#define MAX_PATH_LEN 1024
static const char *g_base __attribute__((unused)) = LS_BASE_DIR;
static const char *g_tmp_dir __attribute__((unused)) = BACKUP_TMP_DIR;

static int g_verbose = 0;
static void logprintf(const char *fmt, ...) {
    if (!g_verbose) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}
static char *g_config_json = NULL;
/* g_config_json (if set) stands in for the file; otherwise the real file is read. */
static int read_file_limited(const char *path, char **out, size_t max_len) {
    if (g_config_json) { *out = strdup(g_config_json); return 0; }
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    char *b = calloc(1, max_len + 1);
    size_t n = b ? fread(b, 1, max_len, f) : 0;
    fclose(f);
    if (!b) return -1;
    b[n] = 0;
    *out = b;
    return 0;
}
/* Minimal: recognizes "key": true / "key": false in g_config_json. */
static int json_get_bool_default(const char *j, const char *k, int d) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\": true", k);
    if (j && strstr(j, pat)) return 1;
    snprintf(pat, sizeof(pat), "\"%s\": false", k);
    if (j && strstr(j, pat)) return 0;
    return d;
}
static const char *host_json_val(const char *j, const char *k) {
    char pat[96];
    snprintf(pat, sizeof(pat), "\"%s\":", k);
    const char *p = j ? strstr(j, pat) : NULL;
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ') p++;
    return p;
}
static int json_get_int_default(const char *j, const char *k, int d) {
    const char *p = host_json_val(j, k);
    return (p && (*p == '-' || (*p >= '0' && *p <= '9'))) ? atoi(p) : d;
}
static int json_get_string(const char *j, const char *k, char *o, size_t n) {
    const char *p = host_json_val(j, k);
    if (!p || *p != '"') return -1;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) o[i++] = *p++;
    o[i] = 0;
    return 0;
}
static void json_escape(const char *in, char *out, size_t out_sz) {
    size_t o = 0;
    for (; in && *in && o + 7 < out_sz; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c < 0x20) o += (size_t)snprintf(out + o, out_sz - o, "\\u%04x", c);
        else out[o++] = (char)c;
    }
    out[o] = 0;
}
static void cloud_timestamp(char *out, size_t out_sz) {
    time_t now = time(NULL); struct tm t; localtime_r(&now, &t);
    snprintf(out, out_sz, "%04d%02d%02d-%02d%02d%02d", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
}

#include "../../src/lansync_core.inc.c"
#include "../../src/lansync_save.inc.c"

static int failures = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { failures++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb"); fputs(data, f); fclose(f);
}
static int file_exists(const char *path) { struct stat st; return stat(path, &st) == 0; }
static void mkdirs(const char *path) {
    char cmd[MAX_PATH_LEN + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd) != 0) printf("mkdir -p failed: %s\n", path);
}

static void host_reset_base(void) {
    if (system("rm -rf " LS_BASE_DIR) != 0) printf("cleanup failed\n");
    mkdir(LS_BASE_DIR, 0777);
    mkdir(BACKUP_TMP_DIR, 0777);
    mkdir(CLOUD_QUEUE_DIR, 0777);
}

static int host_open_state_db(void) {
    pthread_mutex_lock(&g_ls_mtx);
    int ok = ls_open_db() == 0;
    pthread_mutex_unlock(&g_ls_mtx);
    g_ls_ready = ok;
    return ok;
}

static int64_t ev_count_code(const char *code) {
    sqlite3_stmt *st = NULL; int64_t n = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*) FROM events WHERE code = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, code, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return n;
}

static int host_finish(void) {
    printf("%d checks, %d failures\n", checks, failures);
    if (!failures) { if (system("rm -rf " LS_BASE_DIR) != 0) printf("cleanup failed\n"); }
    else printf("kept %s for inspection\n", LS_BASE_DIR);
    return failures ? 1 : 0;
}
#endif
