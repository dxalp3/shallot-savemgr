/*
 * Host tests for the object / snapshot API (src/lansync_http.inc.c):
 * /api/backup/objects, /api/backup/snapshots, /api/restore/snapshot,
 * /api/backup/protect_object and the object fields on existing endpoints,
 * with the HTTP layer captured in memory.
 */
#include "host_worker_stubs.h"

static char g_out[256 * 1024];
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

#define PORT 8082
#define LS_ONION_DIR LS_BASE_DIR "/OnionHEN"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../../src/lansync_pclink.inc.c"
#include "../../src/lansync_launcher.inc.c"
#include "../../src/lansync_http.inc.c"
/* host stand-in for main.c's connect helper (vault client) */
static int tcp_connect_host(const char *host, int port, char *err, size_t err_sz) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr(strcmp(host, "localhost") == 0 ? "127.0.0.1" : host);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) {
        snprintf(err, err_sz, "connect to %s:%d failed", host, port);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}
#include "../../src/lansync_vault.inc.c"
#include "../../src/lansync_auth.inc.c"

/* A full request (headers + optional JSON body) through the login gate. */
static char g_req[4096];
static int gate(const char *method, const char *url, const char *headers, const char *body) {
    g_out[0] = 0;
    int n = snprintf(g_req, sizeof(g_req), "%s %s HTTP/1.1\r\nHost: ps5\r\n%s%s\r\n%s", method, url, headers ? headers : "",
                     body ? "" : "", body ? body : "");
    if (body) {
        char h2[1200];
        snprintf(h2, sizeof(h2), "%sContent-Length: %zu\r\n", headers ? headers : "", strlen(body));
        n = snprintf(g_req, sizeof(g_req), "%s %s HTTP/1.1\r\nHost: ps5\r\n%s\r\n%s", method, url, h2, body);
    }
    return ls_auth_gate(0, method, url, g_req, n);
}

#define T "PPSA00002"
#define D LS_USER_HOME "/10000000/savedata_prospero/" T

static int get(const char *url) {
    g_out[0] = 0;
    char req[8] = {0};
    if (ls_http_route_req(0, "GET", url, req, 0)) return 1;
    return ls_http_route(0, "GET", url);
}
static int post(const char *url) {
    g_out[0] = 0;
    char req[8] = {0};
    if (ls_http_route_req(0, "POST", url, req, 0)) return 1;
    return ls_http_route(0, "POST", url);
}
static int has(const char *s) { return strstr(g_out, s) != NULL; }

/* A queued archive while a PC waits: wakes it. */
static void *bump_later(void *arg) {
    usleep(300 * 1000);
    ls_wait_bump((const char *)arg);
    return NULL;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    g_ls_worker_started = 1;
    char *iobuf = malloc(BUF_SIZE);

    mkdirs(D);
    write_file(D "/sdimg_Save00", "a");
    write_file(D "/sdimg_sce_bu_Save00", "a");
    write_file(D "/sdimg_Save04", "b");
    write_file(D "/sdimg_sce_bu_0001.bin", "orphan");
    int64_t snap = 0;
    ls_protect_title_ex(T, "game_close", iobuf, &snap);

    /* Objects: grouped, raw backup copies are members, not saves of their own. */
    CHECK(get("/api/backup/objects?title=" T) && has("200 OK"), "objects endpoint");
    CHECK(has("\"id\":\"Save00\"") && has("\"primary\":{\"save_name\":\"sdimg_Save00\"") &&
          has("\"sce_backup\":{\"save_name\":\"sdimg_sce_bu_Save00\""), "Save00 with both members");
    CHECK(has("\"id\":\"Save04\"") && has("\"id\":\"0001.bin\"") && has("\"orphan\":true"), "single + orphan objects");
    CHECK(!has("\"id\":\"sce_bu_Save00\""), "no backup copy as its own object");
    CHECK(has("\"state\":\"protected\"") && has("\"pending_pc\":true"), "state + PC pending");

    /* Titles: files and objects counted separately. */
    get("/api/backup/titles");
    CHECK(has("\"saves\":4,\"objects\":3"), "titles: 4 files, 3 objects");

    /* Snapshots: self-describing manifest with relative names and archives. */
    CHECK(get("/api/backup/snapshots?title=" T) && has("\"format\":\"shallot-snapshot\",\"version\":1"), "snapshot manifest");
    CHECK(has("\"object_count\":3,\"file_count\":4") && has("\"restorable_on_ps5\":true") && has("\"all_on_pc\":false"),
          "counts and availability");
    CHECK(has("{\"id\":\"Save00\",\"label\":\"\",\"members\":[{\"role\":\"primary\",\"file\":\"sdimg_Save00\"") &&
          has("{\"role\":\"sce_backup\",\"file\":\"sdimg_sce_bu_Save00\""), "object members in manifest");
    CHECK(!has(LS_USER_HOME), "no absolute paths in the manifest");
    CHECK(get("/api/backup/snapshots?since=0") && has("\"snapshot_id\":1"), "PC feed by snap id");
    CHECK(get("/api/backup/snapshots?since=99") && has("{\"snapshots\":[]}"), "nothing newer");

    /* Existing endpoints carry object id + role. */
    get("/api/backup/history?title=" T);
    CHECK(has("\"object_id\":\"Save00\",\"role\":\"sce_backup\",\"save_name\":\"sdimg_sce_bu_Save00\""), "history rows grouped");

    /* Object backup and snapshot restore requests. */
    CHECK(post("/api/backup/protect_object?id=" T "&object=Save04") && has("\"queued\":true"), "object backup queued");
    CHECK(post("/api/backup/protect_object?id=" T "&object=..") && has("Invalid"), "bad object id refused");
    char url[128];
    snprintf(url, sizeof(url), "/api/restore/snapshot?id=%lld&object=Save04", (long long)snap);
    CHECK(post(url) && has("\"ok\":true"), "snapshot object restore accepted (%s)", g_out);
    CHECK(post("/api/restore/snapshot?id=999") && has("error"), "unknown snapshot refused");
    get("/api/restore/status");
    CHECK(has("\"kind\":\"object\"") && has("\"object_id\":\"Save04\"") && has("\"items\":[{\"save_name\":\"sdimg_Save04\""),
          "status lists the items (%.300s)", g_out);

    /* Display names: stored per game + save, shown in objects and snapshot manifests. */
    CHECK(post("/api/backup/label?title=" T "&object=Save04&label=Act 2 castle") && has("\"ok\":true"), "label stored");
    get("/api/backup/objects?title=" T);
    CHECK(has("\"id\":\"Save04\",\"label\":\"Act 2 castle\"") && has("\"id\":\"Save00\",\"label\":\"\""), "label in objects");
    CHECK(has("\"modified_at\":") && has("\"last_backup\":"), "objects carry modified/last backup times");
    get("/api/backup/snapshots?title=" T);
    CHECK(has("\"label\":\"Act 2 castle\""), "label in snapshot manifest");
    CHECK(post("/api/backup/label?title=" T "&object=Save04&label=") && has("\"ok\":true"), "label cleared");
    get("/api/backup/objects?title=" T);
    CHECK(has("\"id\":\"Save04\",\"label\":\"\""), "label gone");
    CHECK(post("/api/backup/label?title=" T "&object=../x&label=a") && has("error"), "bad object refused");

    /* Home-screen tile: status endpoint; installing needs the PS5. */
    CHECK(get("/api/system/launcher") && has("\"installed\":false") && has("\"title_id\":\"SHAL00001\""), "launcher status");
    CHECK(post("/api/system/launcher?action=install") && has("error"), "install refused off the console");
    CHECK(post("/api/system/launcher?action=bogus") && has("action must be"), "bad action refused");
    {
        char pj[640];
        ls_launcher_param_json(pj, sizeof(pj));
        CHECK(strstr(pj, "\"deeplinkUri\": \"http://127.0.0.1:8082/\"") && strstr(pj, "\"titleId\": \"SHAL00001\"") && strstr(pj, "\"applicationCategoryType\": 65536"), "param.json (%s)", pj);
    }

    /* Getting a pruned archive back from the PC, then restoring as usual. */
    {
        char qid[200] = "", fname[220] = "", sha[65] = "";
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT q.id, q.filename, q.sha256 FROM snapshot_files f JOIN generations g ON g.gen_id = f.gen_id"
                               " JOIN queue q ON q.gen_id = g.gen_id WHERE f.snap_id = ? AND f.object_id = 'Save04' LIMIT 1",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, snap);
            if (sqlite3_step(st) == SQLITE_ROW) {
                ls_col_text(st, 0, qid, sizeof(qid));
                ls_col_text(st, 1, fname, sizeof(fname));
                ls_col_text(st, 2, sha, sizeof(sha));
            }
        }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        CHECK(qid[0] && fname[0], "archive of Save04 in snapshot #%lld", (long long)snap);
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, fname);
        FILE *fh = fopen(path, "rb");
        static char body[1 << 20];
        size_t blen = fh ? fread(body, 1, sizeof(body), fh) : 0;
        if (fh) fclose(fh);
        CHECK(blen > 0, "archive readable (%zu bytes)", blen);
        /* PC has it, PS5 pruned it. */
        unlink(path);
        ls_queue_mark(qid, "pruned");
        char sql[400];
        snprintf(sql, sizeof(sql), "UPDATE queue SET lan_ack_at = 1 WHERE id = '%s'", qid);
        pthread_mutex_lock(&g_ls_mtx); ls_exec_locked(sql); pthread_mutex_unlock(&g_ls_mtx);
        char u[200];
        snprintf(u, sizeof(u), "/api/backup/snapshots?title=" T);
        get(u);
        CHECK(has("\"restorable_on_ps5\":false"), "snapshot not restorable once pruned");
        snprintf(u, sizeof(u), "/api/pc/fetch?snap=%lld&object=Save04", (long long)snap);
        CHECK(post(u) && has("\"ok\":true") && has("\"state\":\"waiting\""), "fetch requested (%.200s)", g_out);
        int fid = 0;
        { const char *q = strstr(g_out, "\"id\":"); if (q) fid = atoi(q + 5); }
        CHECK(get("/api/pc/requests") && has(qid) && has(sha), "PC sees what to upload (%.300s)", g_out);
        snprintf(u, sizeof(u), "/api/pc/fetch_status?id=%d", fid);
        get(u);
        CHECK(has("\"state\":\"waiting\"") && has("\"n\":1"), "status waiting (%.200s)", g_out);
        /* A corrupt upload is refused and nothing is adopted. */
        static char req[(1 << 20) + 512];
        char url[300];
        snprintf(url, sizeof(url), "/api/pc/readopt?id=%s", qid);
        int hl = snprintf(req, 512, "POST %s HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", url, blen);
        memcpy(req + hl, body, blen);
        req[hl + 10] ^= 0x55;
        g_out[0] = 0;
        ls_http_route_req(0, "POST", url, req, hl + (int)blen);
        CHECK(has("does not match"), "corrupt copy refused (%.200s)", g_out);
        ls_queue_row_t qr;
        CHECK(ls_queue_get(qid, &qr) && strcmp(qr.state, "pruned") == 0 && access(path, F_OK) != 0, "nothing adopted");
        /* The good copy is taken back. */
        memcpy(req + hl, body, blen);
        g_out[0] = 0;
        ls_http_route_req(0, "POST", url, req, hl + (int)blen);
        CHECK(has("\"ok\":true"), "archive accepted (%.200s)", g_out);
        CHECK(ls_queue_get(qid, &qr) && strcmp(qr.state, "ready") == 0 && qr.lan_ack_at > 1 && access(path, F_OK) == 0,
              "archive back in the queue");
        snprintf(u, sizeof(u), "/api/pc/fetch_status?id=%d", fid);
        get(u);
        CHECK(has("\"state\":\"ready\"") && has("\"got\":1"), "status ready (%.200s)", g_out);
        CHECK(get("/api/pc/requests") && has("\"requests\":[]"), "nothing left to upload");
        snprintf(u, sizeof(u), "/api/pc/fetch?snap=%lld&object=Save04", (long long)snap);
        CHECK(post(u) && has("\"id\":0") && has("\"state\":\"ready\""), "second request: nothing missing");
        CHECK(post("/api/pc/fetch?snap=99999") && has("error"), "unknown restore point refused");
    }

    /* Start with the console: store the ELF, onionHEN auto-start marker. */
    {
        static char rq[4096];
        const char *url = "/api/system/autostart?action=upload";
        int hl = snprintf(rq, 512, "POST %s HTTP/1.1\r\nContent-Length: 8\r\n\r\n", url);
        memcpy(rq + hl, "notanelf", 8);
        g_out[0] = 0;
        ls_http_route_req(0, "POST", url, rq, hl + 8);
        CHECK(has("not an ELF"), "non-ELF refused (%.120s)", g_out);
        memcpy(rq + hl, "\x7f" "ELF\x02\x01\x01\x09", 8);
        g_out[0] = 0;
        ls_http_route_req(0, "POST", url, rq, hl + 8);
        CHECK(has("\"stored\":true") && has("\"onionhen\":false"), "ELF stored (%.200s)", g_out);
        CHECK(post("/api/system/autostart?action=on") && has("onionHEN was not found"), "needs onionHEN");
        mkdirs(LS_BASE_DIR "/OnionHEN");
        CHECK(post("/api/system/autostart?action=on") && has("\"autostart\":true") && has("\"in_payloads\":true"), "auto-start on (%.200s)", g_out);
        CHECK(access(LS_BASE_DIR "/OnionHEN/payloads/shallot-savemgr.elf.auto_start", F_OK) == 0, "marker next to the ELF");
        CHECK(post("/api/system/autostart?action=off") && has("\"autostart\":false") && has("\"in_payloads\":true"), "auto-start off");
        CHECK(get("/api/system/autostart") && has("\"stored\":true"), "status");
    }

    /* PC link: long wait, kick, presence in the status. */
    CHECK(get("/api/backup/wait?seq=0&timeout=5&client=DESKTOP-1&version=2.0") && has("\"reason\":\"") &&
          has("\"pending\":"), "first wait returns at once (%.200s)", g_out);
    int64_t seq = 0;
    { const char *q = strstr(g_out, "\"seq\":"); if (q) seq = strtoll(q + 6, NULL, 10); }
    char u[160];
    snprintf(u, sizeof(u), "/api/backup/wait?seq=%lld&timeout=1&client=DESKTOP-1", (long long)seq);
    int64_t t0 = ls_io_now_ms(NULL);
    get(u);
    CHECK(has("\"reason\":\"timeout\"") && ls_io_now_ms(NULL) - t0 >= 900, "nothing new: waits for the timeout");
    pthread_t th;
    pthread_create(&th, NULL, bump_later, "new");
    t0 = ls_io_now_ms(NULL);
    snprintf(u, sizeof(u), "/api/backup/wait?seq=%lld&timeout=20", (long long)seq);
    get(u);
    pthread_join(th, NULL);
    CHECK(has("\"reason\":\"new\"") && ls_io_now_ms(NULL) - t0 < 5000, "new archive wakes the waiting PC (%.120s)", g_out);
    CHECK(post("/api/backup/kick") && has("\"ok\":true"), "kick accepted");
    get("/api/backup/status");
    CHECK(has("\"pc\":{\"id\":\"ip:unknown\"") && has("\"connected\":true") && has("\"name\":\"DESKTOP-1\"") && has("\"version\":\"2.0\"") &&
          has("\"backlog\":"), "status shows the PC (%.300s)", strstr(g_out, "\"pc\"") ? strstr(g_out, "\"pc\"") : g_out);
    CHECK(get("/api/pc/status") && has("\"connected\":true") && has("\"vault\":{\"state\":\"off\""), "pc status endpoint");
    get("/api/backup/wait?seq=0&timeout=0&client=DESKTOP-1&version=2.1&vault=pending&vault_pending=7&vault_at=123");
    CHECK(get("/api/pc/status") && has("\"vault\":{\"state\":\"pending\",\"pending\":7,\"at\":123,"),
          "vault status from the PC (%.300s)", g_out);

    /* Managing the PC client: a command reaches the open wait, the client reports back. */
    get("/api/backup/wait?seq=0&timeout=0&client=NAS&version=2.1&kind=docker&caps=ping,sync,pause,resume&paused=0"
        "&held=42&held_bytes=1000&free=5000&vault=ok&vault_src=ps5");
    CHECK(get("/api/pc/status") && has("\"kind\":\"docker\"") && has("\"caps\":\"ping,sync,pause,resume\"") &&
          has("\"held\":42") && has("\"free_bytes\":5000") && has("\"paused\":false") && has("\"src\":\"ps5\"") &&
          has("\"bundled_version\":\"9.9\""), "client kind, abilities and store (%.400s)", g_out);
    CHECK(post("/api/pc/command?do=format_c") && has("unknown command"), "unknown command refused");
    get("/api/backup/wait?seq=0&timeout=0");
    { const char *q = strstr(g_out, "\"seq\":"); seq = q ? strtoll(q + 6, NULL, 10) : 0; }
    CHECK(post("/api/pc/command?do=ping") && has("\"ok\":true") && has("\"id\":"), "ping queued (%.200s)", g_out);
    int cid = 0;
    { const char *q = strstr(g_out, "\"id\":"); if (q) cid = atoi(q + 5); }
    CHECK(get("/api/pc/status") && has("\"cmd\":{") && has("\"do\":\"ping\"") && has("\"state\":\"waiting\""), "UI sees it waiting");
    snprintf(u, sizeof(u), "/api/backup/wait?seq=%lld&timeout=20", (long long)seq);
    t0 = ls_io_now_ms(NULL);
    get(u);
    CHECK(has("\"reason\":\"cmd\"") && has("\"cmd\":\"ping\"") && ls_io_now_ms(NULL) - t0 < 2000,
          "the waiting client gets it at once (%.200s)", g_out);
    CHECK(get("/api/pc/status") && has("\"state\":\"running\""), "picked up");
    snprintf(u, sizeof(u), "/api/pc/command_done?id=%d&ok=1&result=pong", cid);
    CHECK(post(u) && has("\"ok\":true"), "client reports the result");
    CHECK(get("/api/pc/status") && has("\"state\":\"done\"") && has("\"result\":\"pong\""), "UI sees the answer (%.400s)", g_out);
    get("/api/backup/wait?seq=0&timeout=0");
    CHECK(!has("\"cmd_id\""), "a finished command is not handed out again");
    snprintf(u, sizeof(u), "/api/pc/command_done?id=%d&ok=1&result=again", cid);
    CHECK(post(u) && has("\"ok\":false"), "a second report is ignored");

    /* PS5 only / VPS only: a running PC client is offered nothing. */
    CHECK(get("/api/backup/queue?pending=1") && !has("{\"queue\":[]}"), "PC mode: the PC collects (%.120s)", g_out);
    {
        ls_config_t cfg;
        ls_load_config(&cfg);
        ls_parse_config("{\"storage_mode\": \"vps\"}", &cfg);
        CHECK(ls_save_config(&cfg) == 0, "VPS only saved");
    }
    CHECK(get("/api/backup/queue?pending=1") && has("{\"queue\":[]}"), "VPS only: nothing for the PC (%.120s)", g_out);
    CHECK(get("/api/backup/snapshots?since=0") && has("{\"snapshots\":[]}"), "VPS only: no restore points for the PC");
    CHECK(get("/api/backup/snapshots?title=" T) && !has("{\"snapshots\":[]}"), "the web UI still sees restore points");
    CHECK(get("/api/backup/queue") && !has("{\"queue\":[]}"), "the full queue is still listed");
    CHECK(get("/api/pc/status") && has("\"backlog\":0"), "no PC backlog when the PC is not a destination");
    {
        ls_config_t cfg;
        ls_load_config(&cfg);
        ls_parse_config("{\"storage_mode\": \"pc\"}", &cfg);
        ls_save_config(&cfg);
    }

    /* One PC client collects: a second one is on standby and gets nothing; the UI can switch. */
    CHECK(get("/api/backup/wait?seq=0&timeout=0&cid=nas2&client=NAS2&kind=docker") && has("\"standby\":true"),
          "second client: standby (%.200s)", g_out);
    CHECK(get("/api/backup/queue?pending=1&cid=nas2") && has("{\"queue\":[]}"), "standby gets no queue");
    CHECK(get("/api/backup/queue?pending=1") && !has("{\"queue\":[]}"), "the active one does");
    CHECK(post("/api/backup/queue_ack?id=x&dest=pc&sha256=00&cid=nas2") && has("another PC client"), "standby ack refused");
    CHECK(get("/api/pc/vault_config?cid=nas2") && has("\"standby\":true"), "standby gets no vault settings");
    CHECK(get("/api/pc/status") && has("\"standby\":1"), "status counts the standby client");
    CHECK(get("/api/pc/clients") && has("\"id\":\"nas2\"") && has("\"state\":\"standby\"") && has("\"state\":\"active\"") &&
          has("\"kind\":\"docker\""), "clients listed (%.400s)", g_out);
    CHECK(post("/api/pc/clients?id=nas2&do=activate") && has("\"ok\":true"), "switch to the NAS");
    CHECK(get("/api/backup/queue?pending=1&cid=nas2") && !has("{\"queue\":[]}"), "the NAS collects now");
    CHECK(get("/api/backup/queue?pending=1") && has("{\"queue\":[]}"), "the old one gets nothing");
    CHECK(post("/api/pc/clients?id=nas2&do=block") && get("/api/pc/clients") && has("\"state\":\"blocked\""), "NAS blocked");
    CHECK(get("/api/backup/queue?pending=1&cid=nas2") && has("{\"queue\":[]}"), "a blocked client gets nothing");
    CHECK(get("/api/backup/queue?pending=1") && !has("{\"queue\":[]}"), "the next client took over");
    CHECK(post("/api/pc/clients?id=nas2&do=unblock") && get("/api/pc/clients") && !has("\"state\":\"blocked\""), "unblocked");
    CHECK(post("/api/pc/clients?id=nas2&do=forget") && get("/api/pc/clients") && !has("\"id\":\"nas2\""), "forgotten");
    CHECK(post("/api/pc/clients?id=nas2&do=explode") && has("unknown action"), "unknown action refused");

    /* The primary is away: a client that is here stands in and collects copies, but
       the PS5 keeps everything until the primary has it (no history split). */
    get("/api/backup/wait?seq=0&timeout=0&cid=gone1&kind=docker");
    CHECK(post("/api/pc/clients?id=gone1&do=activate") && get("/api/backup/queue?pending=1&cid=here1") && has("{\"queue\":[]}"),
          "while the primary is around, another one stands by");
    pthread_mutex_lock(&g_ls_pc_mtx);
    for (int i = 0; i < LS_PC_PEERS; i++)
        if (strcmp(g_ls_peers[i].id, "gone1") == 0) g_ls_peers[i].last_seen -= LS_PC_LEASE_S + 60;
    pthread_mutex_unlock(&g_ls_pc_mtx);
    CHECK(get("/api/backup/queue?pending=1&cid=here1") && !has("{\"queue\":[]}"), "after the lease the present client stands in");
    char sid[200] = "", ssha[80] = "";
    {
        const char *q = strstr(g_out, "\"id\":\""), *h = strstr(g_out, "\"sha256\":\"");
        if (q) { q += 6; size_t l = strcspn(q, "\""); if (l < sizeof(sid)) { memcpy(sid, q, l); sid[l] = 0; } }
        if (h) { memcpy(ssha, h + 10, 64); ssha[64] = 0; }
    }
    CHECK(get("/api/pc/clients") && has("\"active\":\"gone1\"") && has("\"state\":\"standin\""), "the primary keeps its role (%.300s)", g_out);
    snprintf(u, sizeof(u), "/api/backup/queue_ack?id=%s&dest=pc&sha256=%s&cid=here1", sid, ssha);
    CHECK(post(u) && has("\"standin\":true"), "the stand-in's copy is noted (%.200s)", g_out);
    {
        ls_queue_row_t qr;
        CHECK(ls_queue_get(sid, &qr) && qr.lan_ack_at == 0 && strcmp(qr.state, "ready") == 0, "but the PS5 keeps it for the primary");
    }
    get("/api/backup/queue?pending=1&cid=here1");
    CHECK(!has(sid), "the stand-in is not offered it again");
    get("/api/backup/queue?pending=1&cid=gone1");
    CHECK(has(sid), "the returning primary collects what it missed");
    CHECK(get("/api/pc/clients") && !has("\"state\":\"standin\""), "and the stand-in steps back");
    /* promoting the stand-in: what it holds counts as delivered */
    CHECK(post("/api/pc/clients?id=here1&do=activate"), "stand-in made primary");
    {
        ls_queue_row_t qr;
        CHECK(ls_queue_get(sid, &qr) && qr.lan_ack_at > 0, "its copies count once it is the primary");
    }
    post("/api/pc/clients?id=gone1&do=forget");
    post("/api/pc/clients?id=here1&do=forget");

    /* Relay settings for "PC and VPS" with the PC uploading. */
    CHECK(get("/api/pc/vault_config") && has("\"route\":\"off\""), "no relay in PC mode (%.200s)", g_out);
    {
        ls_config_t cfg;
        ls_load_config(&cfg);
        ls_parse_config("{\"storage_mode\": \"both\", \"vps_route\": \"pc\", \"vault_url\": \"http://10.0.0.9:8091\","
                        " \"vault_token\": \"abcdefghijklmnopqrstuvwxyz\"}", &cfg);
        CHECK(ls_save_config(&cfg) == 0, "relay mode saved");
        ls_vault_register();
    }
    CHECK(get("/api/pc/vault_config") && has("\"route\":\"pc\"") && has("\"url\":\"http://10.0.0.9:8091\"") &&
          has("\"token\":\"abcdefghijklmnopqrstuvwxyz\"") && has("\"key\":\""), "the PC gets url, token and key (%.300s)", g_out);
    {
        const char *k = strstr(g_out, "\"key\":\"");
        CHECK(k && strlen(k) >= 7 + 64 && k[7 + 64] == '"', "key is 64 hex characters");
    }
    CHECK(get("/api/backup/status") && has("\"storage_mode\":\"both\"") && has("\"vps_route\":\"pc\"") &&
          has("\"route\":\"pc\""), "status names mode and route");

    /* Optional web UI login: off by default, then password, session, PC client key. */
    CHECK(gate("GET", "/api/backup/status", NULL, NULL) == 1, "login off: everything open");
    CHECK(gate("GET", "/api/auth/status", NULL, NULL) == 0 && has("\"enabled\":false"), "status says off (%.120s)", g_out);
    gate("POST", "/api/auth/password", NULL, "{\"new\":\"abc\"}");
    CHECK(has("at least 6"), "too short a password refused (%.120s)", g_out);
    gate("POST", "/api/auth/password", NULL, "{\"new\":\"secret-123\"}");
    {
        ls_config_t c;
        ls_load_config(&c);
        CHECK(strlen(c.ui_pass_hash) == 64 && strlen(c.ui_pass_salt) == 32 && strlen(c.client_key) == 48,
              "password stored as salt + hash, client key made");
        CHECK(ls_pass_ok(&c, "secret-123") && !ls_pass_ok(&c, "secret-124"), "password check");
        CHECK(!strstr(g_out, "secret-123"), "the password never comes back");
        char hdr[120];
        CHECK(gate("GET", "/api/backup/status", NULL, NULL) == 0, "login on: anonymous request refused");
        snprintf(hdr, sizeof(hdr), "X-Shallot-Key: %s\r\n", c.client_key);
        CHECK(gate("GET", "/api/backup/wait?seq=0", hdr, NULL) == 1, "the PC client key lets the client in");
        CHECK(gate("GET", "/api/backup/wait?seq=0", "X-Shallot-Key: 00112233\r\n", NULL) == 0, "a wrong key does not");
        char tok[65], ck[120];
        ls_sess_new(tok);
        snprintf(ck, sizeof(ck), "Cookie: theme=dark; gsid=%s\r\n", tok);
        CHECK(gate("GET", "/api/backup/status", ck, NULL) == 1, "a session cookie lets the browser in");
        CHECK(gate("GET", "/api/backup/status", "Cookie: gsid=0000000000000000000000000000000000000000000000000000000000000000\r\n", NULL) == 0,
              "a made-up session does not");
        CHECK(gate("GET", "/api/auth/client_key", NULL, NULL) == 0 && !strstr(g_out, c.client_key), "the key is not shown without login");
        CHECK(gate("GET", "/api/auth/client_key", ck, NULL) == 0 && has(c.client_key), "but with login");
        CHECK(gate("POST", "/api/auth/login", NULL, "{\"password\":\"nope-nope\"}") == 0 && has("wrong password"), "wrong password refused");
        int before = 0, after = 0;
        for (int i = 0; i < LS_SESS_MAX; i++) before += g_ls_sess[i].tok[0] != 0;
        gate("POST", "/api/auth/login", NULL, "{\"password\":\"secret-123\"}");
        for (int i = 0; i < LS_SESS_MAX; i++) after += g_ls_sess[i].tok[0] != 0;
        CHECK(after == before + 1, "right password opens a session (%d -> %d)", before, after);
        ls_send_sync_settings(0);
        CHECK(!has(c.ui_pass_hash) && !has(c.client_key), "settings never show the hash or the key");
        gate("POST", "/api/auth/password", ck, "{\"current\":\"wrong-one\",\"new\":\"\"}");
        CHECK(has("current password is wrong"), "turning it off needs the current password");
        gate("POST", "/api/auth/password", ck, "{\"current\":\"secret-123\",\"new\":\"\"}");
        ls_load_config(&c);
        CHECK(!c.ui_pass_hash[0] && gate("GET", "/api/backup/status", NULL, NULL) == 1, "login off again");
        CHECK(gate("GET", "/api/backup/status", ck, NULL) == 1, "(sessions were dropped, nothing needs them)");
    }

    free(iobuf);
    return host_finish();
}
