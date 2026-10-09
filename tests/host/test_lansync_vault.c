/*
 * VPS only mode against a real vault (vault/shallot_vault.py). Skipped unless
 * SHALLOT_VAULT_E2E_PORT (the vault's signed-only port) and
 * SHALLOT_VAULT_E2E_TOKEN are set; tests/host/vault_e2e.sh sets them up.
 * Uploads archives and restore points signed and encrypted, fetches a
 * pruned archive back, checks refusal with a wrong token, and writes
 * SHALLOT_VAULT_E2E_OUT (key + archive ids/sha) for the PC-client cross-check.
 */
#include "host_worker_stubs.h"
#define PORT 8082
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
static char g_out[64 * 1024];
static void http_send(int sock, const char *status, const char *ctype, const char *body, int len) {
    (void)sock; (void)ctype;
    snprintf(g_out, sizeof(g_out), "%s|%.*s", status, len, body);
}
static void http_json(int sock, const char *json) { http_send(sock, "200 OK", "application/json", json, (int)strlen(json)); }
static int send_all_fd(int fd, const void *buf, size_t len) { (void)fd; (void)buf; return (int)len; }
static ssize_t recv_all(int sock, void *buf, size_t len) { (void)sock; (void)buf; (void)len; return -1; }
static int read_request_body(int sock, char *req, int n, char *iobuf, char **out, size_t *out_len, char *err, size_t err_sz) {
    (void)sock; (void)req; (void)n; (void)iobuf; *out = NULL; *out_len = 0; snprintf(err, err_sz, "no body"); return -1;
}
static void url_param_decode(const char *src, char *dst, size_t dst_sz) {
    size_t n = 0;
    while (src && *src && *src != '&' && n + 1 < dst_sz) dst[n++] = *src++;
    dst[n] = 0;
}
static int safe_queue_id(const char *id) { return id && id[0] && !strchr(id, '/'); }
#include "../../src/lansync_pclink.inc.c"
#include "../../src/lansync_launcher.inc.c"
#include "../../src/lansync_http.inc.c"
static int tcp_connect_host(const char *host, int port, char *err, size_t err_sz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr(host);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        snprintf(err, err_sz, "connect to %s:%d failed", host, port);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}
#include "../../src/lansync_vault.inc.c"

#define T "PPSA00002"
#define U "10000000"
#define D LS_USER_HOME "/" U "/savedata_prospero/" T
#define IMG (128 * 1024)

static int64_t q_int(const char *sql) {
    sqlite3_stmt *st = NULL; int64_t v = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return v;
}
static void put_img(const char *path, const unsigned char *img, int tick) {
    FILE *f = fopen(path, "wb"); fwrite(img, 1, IMG, f); fclose(f);
    struct timespec ts[2]; clock_gettime(CLOCK_REALTIME, &ts[0]); ts[0].tv_sec += 100 + tick * 10; ts[1] = ts[0];
    utimensat(AT_FDCWD, path, ts, 0);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    const char *port = getenv("SHALLOT_VAULT_E2E_PORT"), *token = getenv("SHALLOT_VAULT_E2E_TOKEN"), *outp = getenv("SHALLOT_VAULT_E2E_OUT");
    if (!port || !token) { printf("skipped (no vault; run tests/host/vault_e2e.sh) "); return host_finish(); }
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    g_ls_worker_started = 1;
    ls_vault_register();
    char *iobuf = malloc(BUF_SIZE);

    ls_config_t cfg;
    ls_load_config(&cfg);
    cfg.storage_vps = 1;
    snprintf(cfg.vault_url, sizeof(cfg.vault_url), "http://127.0.0.1:%s", port);
    snprintf(cfg.vault_token, sizeof(cfg.vault_token), "%s", token);
    ls_save_config(&cfg);

    static unsigned char img[IMG];
    srand(11);
    for (int i = 0; i < IMG; i++) img[i] = (unsigned char)rand();
    mkdirs(D);
    int64_t snap1 = 0, snap2 = 0;
    put_img(D "/sdimg_Save04", img, 0);
    ls_protect_title_ex(T, "game_close", iobuf, &snap1);
    img[70000] ^= 0xff;
    put_img(D "/sdimg_Save04", img, 1);
    ls_protect_title_ex(T, "game_close", iobuf, &snap2);
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE vault_ack_at = 0") == 2, "two archives to send");

    ls_vault_job();
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE vault_ack_at = 0") == 0, "vault has both archives (%s)", g_ls_vault.error);
    CHECK(strcmp(g_ls_vault.state, "ok") == 0, "state ok (%s %s)", g_ls_vault.state, g_ls_vault.error);
    CHECK(ls_meta_get_int("vault_snap_upto", 0) >= snap2, "restore points sent");
    ls_vault_job();                                       /* nothing left: no work, still ok */
    CHECK(strcmp(g_ls_vault.state, "ok") == 0, "second run idle");

    /* PC cross-check data: key + archives */
    if (outp) {
        uint8_t secret[32];
        char hex[65];
        ls_vault_key(secret, 0);
        ls_hex(secret, 32, hex);
        FILE *f = fopen(outp, "w");
        fprintf(f, "{\"key\":\"%s\",\"archives\":{", hex);
        sqlite3_stmt *st = NULL;
        int first = 1;
        pthread_mutex_lock(&g_ls_mtx);
        if (sqlite3_prepare_v2(g_ls_db, "SELECT id, sha256 FROM queue", -1, &st, NULL) == SQLITE_OK)
            while (sqlite3_step(st) == SQLITE_ROW) {
                fprintf(f, "%s\"%s\":\"%s\"", first ? "" : ",", sqlite3_column_text(st, 0), sqlite3_column_text(st, 1));
                first = 0;
            }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        fprintf(f, "}}\n");
        fclose(f);
    }

    /* Prune the older archive on the PS5, then get it back from the vault. */
    char qid[200] = "", fname[220] = "", sha[65] = "";
    {
        sqlite3_stmt *st = NULL;
        pthread_mutex_lock(&g_ls_mtx);
        if (sqlite3_prepare_v2(g_ls_db, "SELECT id, filename, sha256 FROM queue ORDER BY created_at, id LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            ls_col_text(st, 0, qid, sizeof(qid)); ls_col_text(st, 1, fname, sizeof(fname)); ls_col_text(st, 2, sha, sizeof(sha));
        }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, fname);
    unlink(path);
    ls_queue_mark(qid, "pruned");
    char err[200] = "";
    int fid = ls_fetch_request(snap1, "Save04", err, sizeof(err));
    CHECK(fid > 0, "fetch request (%s)", err);
    char sj[400] = "";
    for (int i = 0; i < 300; i++) {
        ls_fetch_status_json(fid, sj, sizeof(sj));
        if (strstr(sj, "\"ready\"") || strstr(sj, "\"failed\"")) break;
        usleep(100 * 1000);
    }
    ls_queue_row_t q;
    char hexs[65] = "";
    CHECK(ls_queue_get(qid, &q) && strcmp(q.state, "ready") == 0, "archive back from the vault (%s; %s)", q.state, sj);
    CHECK(ls_sha256_file(path, hexs, NULL, iobuf) == 0 && strcmp(hexs, sha) == 0, "byte-identical");

    /* Every archive has its block list next to it. */
    ls_vurl_t vu;
    char *resp = NULL;
    size_t rlen = 0;
    ls_vurl_parse(cfg.vault_url, &vu, err, sizeof(err));
    int s = ls_vault_req(&vu, token, "GET", "/v1/stats", NULL, 0, &resp, &rlen, err, sizeof(err));
    CHECK(s == 200 && resp && strstr(resp, "\"archive\": 2") && strstr(resp, "\"refs\": 2"), "block lists stored (%s)", resp ? resp : err);
    free(resp); resp = NULL;

    /* A PS5 clock an hour off: refused once, corrected from the vault's time, retried. */
    g_ls_vault_skew = -3600;
    s = ls_vault_req(&vu, token, "GET", "/v1/ping", NULL, 0, &resp, &rlen, err, sizeof(err));
    CHECK(s == 200 && g_ls_vault_skew > -30 && g_ls_vault_skew < 30, "clock corrected (%d, skew %lld)", s, (long long)g_ls_vault_skew);
    free(resp); resp = NULL;

    /* The vault follows the history schedule: 40 days later, one of the two versions goes. */
    pthread_mutex_lock(&g_ls_mtx);
    ls_exec_locked("UPDATE queue SET created_at = created_at - 40 * 86400");
    ls_exec_locked("UPDATE snapshots SET created_at = created_at - 40 * 86400");
    pthread_mutex_unlock(&g_ls_mtx);
    ls_meta_set_int("last_vault_thin", 0);
    uint8_t vsecret[32];
    ls_vault_key(vsecret, 0);
    ls_vkeys_t vk;
    ls_vkeys_init(&vk, vsecret);
    int thinned = ls_vault_thin(&vu, token, &vk, (int64_t)time(NULL), err, sizeof(err));
    CHECK(thinned == 1, "one old version thinned out of the vault (%d, %s)", thinned, err);
    CHECK(q_int("SELECT COUNT(*) FROM queue WHERE vault_gone = 1") == 1 && q_int("SELECT COUNT(*) FROM snapshots WHERE vault_gone = 1") >= 1,
          "recorded as gone from the vault");
    s = ls_vault_req(&vu, token, "GET", "/v1/stats", NULL, 0, &resp, &rlen, err, sizeof(err));
    CHECK(s == 200 && resp && strstr(resp, "\"archive\": 1") && strstr(resp, "\"refs\": 1"), "vault holds one version (%s)", resp ? resp : err);
    free(resp); resp = NULL;
    s = ls_vault_req(&vu, token, "POST", "/v1/gc", "{\"min_age_s\": 0}", 16, &resp, &rlen, err, sizeof(err));
    CHECK(s == 200 && resp && strstr(resp, "\"deleted\": ") && !strstr(resp, "\"deleted\": 0"), "its own blocks deleted (%s)", resp ? resp : err);
    free(resp); resp = NULL;
    CHECK(ls_vault_thin(&vu, token, &vk, (int64_t)time(NULL), err, sizeof(err)) == 0, "at most daily");

    /* A wrong token is refused by the signed port. */
    snprintf(cfg.vault_token, sizeof(cfg.vault_token), "wrong-token-wrong-token-wrong");
    ls_save_config(&cfg);
    pthread_mutex_lock(&g_ls_mtx); ls_exec_locked("UPDATE queue SET vault_ack_at = 0"); pthread_mutex_unlock(&g_ls_mtx);
    ls_vault_job();
    CHECK(strcmp(g_ls_vault.state, "error") == 0 && strstr(g_ls_vault.error, "signature"), "wrong token refused (%s)", g_ls_vault.error);
    free(iobuf);
    return host_finish();
}
