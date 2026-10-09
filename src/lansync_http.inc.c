/*
 * LAN save sync — HTTP API. Included from main.c after the cloud queue
 * handlers (needs http_send/http_json/json_escape/url_param_decode/safe_queue_id).
 *
 *   GET  /api/backup/status
 *   GET  /api/backup/queue[?pending=1]
 *   GET  /api/backup/queue_download?id=<id>   (X-Shallot-SHA256 header)
 *   POST /api/backup/queue_ack?id=<id>&dest=pc|cloud&sha256=<hex>
 *   GET  /api/backup/events[?limit=N]
 *   GET  /api/backup/titles                   (per-title overview for the web UI)
 *   POST /api/backup/pc_manifest?title=<TID>  (PC client: archives it holds; body JSON, <= 4 MiB)
 *   GET  /api/backup/compare?title=<TID>      (Compare With PC: sync state per save)
 *   POST /api/restore/from_queue?id=<qid>[&user=]   (restore a version Shallot still holds)
 *   POST /api/restore/upload?title=&save=&user=&archive_sha256=[&container_sha256=]  (body: ZIP from the PC)
 *   GET  /api/restore/status
 *   GET/POST /api/settings/sync               (sync.json, atomic write, known keys only)
 *   GET  /api/backup/saves[?title=<TID>]
 *   GET  /api/backup/diag/title?id=<TID>[&recent=<min>]   (read-only: where are this title's saves?)
 *   GET  /api/backup/history?title=<TID>[&save=<name>]   (generations, newest first)
 *   POST /api/reconcile/title/sanity?id=<TID>   (Level 1 Sanity Check, synchronous)
 *   GET  /api/reconcile/result[?title=<TID>]    (last stored result / summary of all titles)
 *   POST /api/reconcile/title/full?id=<TID>[&protect=1]  (Level 3, background job)
 *   GET  /api/reconcile/status                  (current/last background job)
 *   POST /api/reconcile/cancel                  (cancel at the next save/title boundary)
 *   POST /api/reconcile/catalogue/quick         (Level 2, background)
 *   POST /api/reconcile/catalogue/full[?protect=1]  (Level 4 / Level 5, background)
 *   GET  /api/reconcile/catalogue/result[?kind=full]  (last catalogue summary)
 *   POST /api/backup/protect_title?id=<TID>   (Back Up Changed Saves)
 *   POST /api/backup/scan_changed             (Scan Changed Saves Now = idle sweep on demand)
 *
 * None of these take g_mtx, so the PC can drain the queue while a
 * mount/backup job is running.
 */

#define LS_ERR_QSIZE "{\"error\":\"Queue file size does not match metadata\"}"
#define LS_ERR_NOTFOUND "{\"error\":\"Queue item not found\"}"
#define LS_ERR_404 "{\"error\":\"Not found\"}"
static void ls_vault_status_json(char *out, size_t out_sz);   /* lansync_vault.inc.c */
static int ls_vault_route(int sock, const char *method, const char *url, char *req, int n);   /* lansync_vault.inc.c */

static void ls_append_row_json(char **buf, size_t *cap, size_t *pos, const ls_queue_row_t *r, int first) {
    char id[400], fn[440], tid[64], sv[512], uid[32], reason[64];
    json_escape(r->id, id, sizeof(id));
    json_escape(r->filename, fn, sizeof(fn));
    json_escape(r->title_id, tid, sizeof(tid));
    json_escape(r->save_name, sv, sizeof(sv));
    json_escape(r->user_id, uid, sizeof(uid));
    json_escape(r->reason, reason, sizeof(reason));
    ls_json_append(buf, cap, pos,
        "%s{\"id\":\"%s\",\"filename\":\"%s\",\"title_id\":\"%s\",\"save_name\":\"%s\","
        "\"user_id\":\"%s\",\"sha256\":\"%s\",\"size\":%lld,\"created_at\":%lld,\"reason\":\"%s\","
        "\"state\":\"%s\",\"gen_id\":%lld,\"lan_ack_at\":%lld,\"cloud_needed\":%s,"
        "\"cloud_ack_at\":%lld,\"pc_attempts\":%d,\"vault_ack_at\":%lld}",
        first ? "" : ",", id, fn, tid, sv, uid, r->sha256, (long long)r->size,
        (long long)r->created_at, reason, r->state, (long long)r->gen_id,
        (long long)r->lan_ack_at, r->cloud_needed ? "true" : "false",
        (long long)r->cloud_ack_at, r->pc_attempts, (long long)r->vault_ack_at);
}

static void ls_send_queue(int sock, int pending_only, int cloud_only, const char *standin) {
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"queue\":[");
    const char *sql = cloud_only
        ? "SELECT " LS_QUEUE_COLS " FROM queue WHERE state IN ('ready','packed') AND cloud_needed = 1 AND cloud_ack_at = 0 ORDER BY created_at"
        : pending_only && standin
        ? "SELECT " LS_QUEUE_COLS " FROM queue WHERE state IN ('ready','packed') AND lan_ack_at = 0 AND ? AND standin_ack != ? ORDER BY created_at"
        : pending_only
        ? "SELECT " LS_QUEUE_COLS " FROM queue WHERE state IN ('ready','packed') AND lan_ack_at = 0 AND ? ORDER BY created_at"
        : "SELECT " LS_QUEUE_COLS " FROM queue WHERE state != 'pruned' ORDER BY created_at";
    int first = 1;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (pending_only && !cloud_only) {
                /* the PC collects only when it is a destination of the mode
                   (PS5 only / VPS only: a running PC client gets nothing) */
                ls_config_t cfg;
                ls_load_config(&cfg);
                sqlite3_bind_int(st, 1, cfg.storage_pc);
                if (standin) ls_bind_text(st, 2, standin);
            }
            ls_queue_row_t r;
            while (sqlite3_step(st) == SQLITE_ROW) {
                ls_queue_row_from(st, &r);
                ls_append_row_json(&buf, &cap, &pos, &r, first);
                first = 0;
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

/* Streams a ready queue archive. Returns 1 if handled (found), 0 if not found. */
static int ls_queue_stream(int sock, const char *id, int count_pc_attempt) {
    ls_queue_row_t r;
    if (!ls_queue_get(id, &r)) return 0;
    if (strcmp(r.state, "packed") == 0) {            /* PS5-only mode: rebuild the exact ZIP first */
        char e[200];
        if (ls_archive_ensure_ready(id, e, sizeof(e)) < 0 || !ls_queue_get(id, &r)) return 0;
    }
    if (strcmp(r.state, "ready") != 0) return 0;
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, r.filename);
    struct stat st;
    if (stat(path, &st) < 0 || !S_ISREG(st.st_mode)) return 0;
    if ((int64_t)st.st_size != r.size) {
        ls_event("QUEUE_SIZE_MISMATCH", r.title_id, "id=%s file=%lld db=%lld",
                 id, (long long)st.st_size, (long long)r.size);
        http_send(sock, "409 Conflict", "application/json",
                  LS_ERR_QSIZE, (int)strlen(LS_ERR_QSIZE));
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    if (count_pc_attempt) {
        ls_queue_bump_attempts(id);
        if (r.pc_attempts == 0) ls_event("PC_DOWNLOAD_STARTED", r.title_id, "id=%s", id);
    }
    char hdr[1536], tid[64], sv[300];
    snprintf(tid, sizeof(tid), "%s", r.title_id);
    snprintf(sv, sizeof(sv), "%s", r.save_name);
    sanitize_filename(sv);
    for (char *p = sv; *p; p++) if ((unsigned char)*p < 0x20 || *p == '"') *p = '_';
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/zip\r\n"
        "Content-Disposition: attachment; filename=\"%s\"\r\n"
        "Content-Length: %lld\r\n"
        "X-Shallot-SHA256: %s\r\n"
        "X-Shallot-Queue-Id: %s\r\n"
        "X-Shallot-Title: %s\r\n"
        "X-Shallot-Save: %s\r\n"
        "X-Shallot-User: %s\r\n"
        "X-Shallot-Created: %lld\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Expose-Headers: Content-Disposition, X-Shallot-SHA256, X-Shallot-Queue-Id,"
        " X-Shallot-Title, X-Shallot-Save, X-Shallot-User, X-Shallot-Created\r\n"
        "Connection: close\r\n\r\n",
        r.filename, (long long)st.st_size, r.sha256, r.id, tid, sv, r.user_id,
        (long long)r.created_at);
    send_all_fd(sock, hdr, (size_t)hlen);
    char *buf = malloc(BUF_SIZE);
    if (buf) {
        ssize_t nr;
        while ((nr = read(fd, buf, BUF_SIZE)) > 0)
            if (send_all_fd(sock, buf, (size_t)nr) < 0) break;
        free(buf);
    }
    close(fd);
    return 1;
}

static void ls_send_status(int sock) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    int64_t pending = 0, pending_bytes = 0, total = 0, total_bytes = 0, missing = 0, cloud_pending = 0;
    int64_t last_queued = 0, vps_pending = 0, vps_pending_bytes = 0, all_made = 0, at_pc = 0, at_vps = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "SELECT"
                " SUM(state IN ('ready','packed')), SUM(CASE WHEN state IN ('ready','packed') THEN size ELSE 0 END),"
                " SUM(state IN ('ready','packed') AND lan_ack_at=0), SUM(CASE WHEN state IN ('ready','packed') AND lan_ack_at=0 THEN size ELSE 0 END),"
                " SUM(state='missing'), SUM(state IN ('ready','packed') AND cloud_needed=1 AND cloud_ack_at=0),"
                " MAX(created_at), SUM(state IN ('ready','packed') AND vault_ack_at=0),"
                " SUM(CASE WHEN state IN ('ready','packed') AND vault_ack_at=0 THEN size ELSE 0 END),"
                " COUNT(*), SUM(lan_ack_at > 0), SUM(vault_ack_at > 0) FROM queue WHERE state != 'failed'", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            total = sqlite3_column_int64(st, 0);
            total_bytes = sqlite3_column_int64(st, 1);
            pending = sqlite3_column_int64(st, 2);
            pending_bytes = sqlite3_column_int64(st, 3);
            missing = sqlite3_column_int64(st, 4);
            cloud_pending = sqlite3_column_int64(st, 5);
            last_queued = sqlite3_column_int64(st, 6);
            vps_pending = sqlite3_column_int64(st, 7);
            vps_pending_bytes = sqlite3_column_int64(st, 8);
            all_made = sqlite3_column_int64(st, 9);
            at_pc = sqlite3_column_int64(st, 10);
            at_vps = sqlite3_column_int64(st, 11);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    char wcur_raw[64], wcur[160], werr[600];
    pthread_mutex_lock(&g_ls_job_mtx);
    snprintf(wcur_raw, sizeof(wcur_raw), "%s", g_ls_worker_current);
    pthread_mutex_unlock(&g_ls_job_mtx);
    json_escape(wcur_raw, wcur, sizeof(wcur));
    json_escape(g_ls_last_error, werr, sizeof(werr));
    char pcj[1600], stj[512], vj[800];
    ls_pc_json(pcj, sizeof(pcj));
    ls_ps5_store_json(stj, sizeof(stj));
    ls_vault_status_json(vj, sizeof(vj));
    char json[6400];
    snprintf(json, sizeof(json),
        "{\"build\":\"" LS_BUILD "\",\"storage_mode\":\"%s\",\"vps_route\":\"%s\",\"ready\":%s,\"enabled\":%s,\"queue\":{\"count\":%lld,\"bytes\":%lld,"
        "\"pending_pc\":%lld,\"pending_pc_bytes\":%lld,\"pending_vps\":%lld,\"pending_vps_bytes\":%lld,\"pending_cloud\":%lld,\"missing\":%lld,"
        "\"made\":%lld,\"at_pc\":%lld,\"at_vps\":%lld,"
        "\"constrained\":%s,\"max_queue_mb\":%d,\"keep_acked_hours\":%d},"
        "\"last_backup_queued\":%lld,\"last_pc_ack\":%lld,\"last_cloud_ack\":%lld,"
        "\"last_startup_recovery\":%lld,\"last_startup_reconciliation\":%lld,\"last_idle_scan\":%lld,"
        "\"pc_last_seen\":%lld,\"recovery_open\":%lld,\"rest_resumes\":%d,"
        "\"last_backup\":%lld,\"pc\":%s,\"store\":%s,\"vault\":%s,"
        "\"worker\":{\"running\":%s,\"current\":\"%s\",\"pending\":%d,\"last_error\":\"%s\"},"
        "\"config\":{\"game_close_backup\":%s,\"stability_check\":%s,\"stability_interval_s\":%d,"
        "\"stability_samples\":%d,\"stability_timeout_s\":%d,\"idle_sweep\":%s,\"idle_delay_min\":%d,"
        "\"bidirectional\":\"%s\",\"regression_recovery\":\"%s\",\"rollback_before_restore\":%s}}",
        ls_storage_mode_name(&cfg), cfg.vps_via_pc ? "pc" : "ps5",
        g_ls_ready ? "true" : "false", cfg.enabled ? "true" : "false",
        (long long)total, (long long)total_bytes, (long long)(cfg.storage_pc ? pending : 0), (long long)(cfg.storage_pc ? pending_bytes : 0),
        (long long)(cfg.storage_vps || cfg.vps_via_pc ? vps_pending : 0), (long long)(cfg.storage_vps || cfg.vps_via_pc ? vps_pending_bytes : 0),
        (long long)cloud_pending, (long long)missing, (long long)all_made, (long long)at_pc, (long long)at_vps,
        g_ls_queue_constrained ? "true" : "false", cfg.max_queue_mb, cfg.keep_acked_hours,
        (long long)last_queued, (long long)ls_meta_get_int("last_pc_ack", 0),
        (long long)ls_meta_get_int("last_cloud_ack", 0),
        (long long)ls_meta_get_int("last_startup_recovery", 0),
        (long long)ls_meta_get_int("last_startup_reconciliation", 0),
        (long long)ls_meta_get_int("last_idle_scan", 0),
        (long long)g_ls_last_pc_poll, (long long)ls_recovery_open_count(""), g_ls_sys.resumes,
        (long long)ls_meta_get_int("last_backup", 0), pcj, stj, vj,
        g_ls_worker_started ? "true" : "false", wcur, ls_worker_pending(), werr,
        cfg.game_close_backup ? "true" : "false", cfg.stability_check ? "true" : "false",
        cfg.stability_interval_s, cfg.stability_samples, cfg.stability_timeout_s,
        cfg.idle_sweep ? "true" : "false", cfg.idle_delay_min,
        cfg.bidirectional, cfg.regression_recovery, cfg.rollback_before_restore ? "true" : "false");
    http_json(sock, json);
}

static void ls_send_events(int sock, int limit) {
    if (limit <= 0 || limit > 1000) limit = 200;
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"events\":[");
    int first = 1;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db,
                "SELECT id, ts, code, title_id, detail FROM events ORDER BY id DESC LIMIT ?",
                -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int(st, 1, limit);
            while (sqlite3_step(st) == SQLITE_ROW) {
                char code[64], tid[32], detail[512], ec[128], et[64], ed[1024];
                ls_col_text(st, 2, code, sizeof(code));
                ls_col_text(st, 3, tid, sizeof(tid));
                ls_col_text(st, 4, detail, sizeof(detail));
                json_escape(code, ec, sizeof(ec));
                json_escape(tid, et, sizeof(et));
                json_escape(detail, ed, sizeof(ed));
                ls_json_append(&buf, &cap, &pos,
                    "%s{\"id\":%lld,\"ts\":%lld,\"code\":\"%s\",\"title_id\":\"%s\",\"detail\":\"%s\"}",
                    first ? "" : ",", (long long)sqlite3_column_int64(st, 0),
                    (long long)sqlite3_column_int64(st, 1), ec, et, ed);
                first = 0;
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

static void ls_handle_ack(int sock, const char *url) {
    char id[256] = {0}, dest[16] = "pc", sha[80] = {0};
    char *p;
    if ((p = strstr(url, "id="))) url_param_decode(p + 3, id, sizeof(id));
    if ((p = strstr(url, "dest="))) url_param_decode(p + 5, dest, sizeof(dest));
    if ((p = strstr(url, "sha256="))) url_param_decode(p + 7, sha, sizeof(sha));
    if (!safe_queue_id(id)) { http_json(sock, "{\"error\":\"Invalid id\"}"); return; }
    if (strcmp(dest, "pc") != 0 && strcmp(dest, "cloud") != 0) {
        http_json(sock, "{\"error\":\"dest must be pc or cloud\"}"); return;
    }
    /* The PC must prove it holds the exact bytes before the PS5 forgets them. */
    if (strcmp(dest, "pc") == 0 && strlen(sha) != 64) {
        http_json(sock, "{\"error\":\"sha256 required for pc acknowledgement\"}"); return;
    }
    int rc = ls_queue_ack(id, dest, sha);
    if (rc == -1) { http_json(sock, "{\"error\":\"Queue item not found\"}"); return; }
    if (rc == -2) {
        ls_event("PC_ACK_REJECTED", NULL, "id=%s sha256 mismatch", id);
        http_json(sock, "{\"error\":\"sha256 mismatch\"}"); return;
    }
    ls_config_t cfg;
    ls_load_config(&cfg);
    ls_queue_prune(&cfg);
    http_json(sock, "{\"ok\":true}");
}

static void ls_send_saves(int sock, const char *title_id) {
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"saves\":[");
    int first = 1;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        const char *sql = title_id[0]
            ? "SELECT user_id, title_id, save_name, state, last_seen_at, last_packaged_at, last_packaged_sha,"
              " last_queue_id, last_error FROM saves WHERE title_id = ? ORDER BY title_id, save_name"
            : "SELECT user_id, title_id, save_name, state, last_seen_at, last_packaged_at, last_packaged_sha,"
              " last_queue_id, last_error FROM saves ORDER BY title_id, save_name";
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (title_id[0]) ls_bind_text(st, 1, title_id);
            while (sqlite3_step(st) == SQLITE_ROW) {
                char f[9][300], e[9][620];
                for (int i = 0; i < 9; i++) {
                    if (i == 4 || i == 5) continue;
                    ls_col_text(st, i, f[i], sizeof(f[i]));
                    json_escape(f[i], e[i], sizeof(e[i]));
                }
                char eo[600];
                ls_json_append(&buf, &cap, &pos,
                    "%s{\"user_id\":\"%s\",\"title_id\":\"%s\",%s\"save_name\":\"%s\",\"state\":\"%s\","
                    "\"last_seen_at\":%lld,\"last_packaged_at\":%lld,\"last_packaged_sha\":\"%s\","
                    "\"last_queue_id\":\"%s\",\"last_error\":\"%s\"}",
                    first ? "" : ",", e[0], e[1], ls_object_json(f[2], eo, sizeof(eo)), e[2], e[3],
                    (long long)sqlite3_column_int64(st, 4), (long long)sqlite3_column_int64(st, 5),
                    e[6], e[7], e[8]);
                first = 0;
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

/* Reads a request body of up to max bytes (the upstream reader caps at 64 KiB). */
static int ls_read_body(int sock, char *req, int n, size_t max, char **out, size_t *out_len) {
    *out = NULL;
    *out_len = 0;
    char *cl = strcasestr(req, "Content-Length:");
    size_t clen = cl ? strtoul(cl + 15, NULL, 10) : 0;
    if (clen == 0 || clen > max) return -1;
    char *bs = strstr(req, "\r\n\r\n");
    if (!bs) return -1;
    bs += 4;
    size_t inbuf = (size_t)(n - (bs - req));
    if (inbuf > clen) inbuf = clen;
    char *body = malloc(clen + 1);
    if (!body) return -1;
    memcpy(body, bs, inbuf);
    size_t got = inbuf;
    while (got < clen) {
        ssize_t r = recv_all(sock, body + got, clen - got);
        if (r <= 0) break;
        got += (size_t)r;
    }
    body[got] = 0;
    if (got != clen) { free(body); return -1; }
    *out = body;
    *out_len = got;
    return 0;
}

static int ls_valid_title_id(const char *t) {
    size_t n = strlen(t);
    if (n < 4 || n > 16) return 0;
    for (size_t i = 0; i < n; i++) if (!isalnum((unsigned char)t[i])) return 0;
    return 1;
}

/* Generations of one title (optionally one save), newest first, with archive state. */
static void ls_send_history(int sock, const char *title_id, const char *save_name) {
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"title_id\":\"%s\",\"generations\":[", title_id);
    int first = 1;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 3) {
        sqlite3_stmt *st = NULL;
        const char *sql =
            "SELECT g.gen_id, g.user_id, g.save_name, g.sha256, g.size, g.parent_gen, g.created_at,"
            " g.reason, g.queue_id, g.pc_acked_at, g.cloud_acked_at, COALESCE(q.state, ''),"
            " COALESCE(s.current_gen_id, 0)"
            " FROM generations g"
            " LEFT JOIN queue q ON q.id = g.queue_id"
            " LEFT JOIN saves s ON s.user_id = g.user_id AND s.title_id = g.title_id AND s.save_name = g.save_name"
            " WHERE g.title_id = ? AND (? = '' OR g.save_name = ?)"
            " ORDER BY g.save_name, g.gen_id DESC";
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            ls_bind_text(st, 1, title_id);
            ls_bind_text(st, 2, save_name);
            ls_bind_text(st, 3, save_name);
            while (sqlite3_step(st) == SQLITE_ROW) {
                char uid[32], save[300], sha[80], reason[64], qid[220], qstate[32];
                char euid[64], esave[620], ereason[128], eqid[440];
                ls_col_text(st, 1, uid, sizeof(uid));
                ls_col_text(st, 2, save, sizeof(save));
                ls_col_text(st, 3, sha, sizeof(sha));
                ls_col_text(st, 7, reason, sizeof(reason));
                ls_col_text(st, 8, qid, sizeof(qid));
                ls_col_text(st, 11, qstate, sizeof(qstate));
                json_escape(uid, euid, sizeof(euid));
                json_escape(save, esave, sizeof(esave));
                json_escape(reason, ereason, sizeof(ereason));
                json_escape(qid, eqid, sizeof(eqid));
                int64_t gen = sqlite3_column_int64(st, 0);
                char eo[600];
                ls_json_append(&buf, &cap, &pos,
                    "%s{\"gen_id\":%lld,\"user_id\":\"%s\",%s\"save_name\":\"%s\",\"sha256\":\"%s\","
                    "\"size\":%lld,\"parent_gen\":%lld,\"created_at\":%lld,\"reason\":\"%s\","
                    "\"queue_id\":\"%s\",\"pc_acked_at\":%lld,\"cloud_acked_at\":%lld,"
                    "\"archive_on_ps5\":%s,\"is_current\":%s}",
                    first ? "" : ",", (long long)gen, euid, ls_object_json(save, eo, sizeof(eo)), esave, sha,
                    (long long)sqlite3_column_int64(st, 4), (long long)sqlite3_column_int64(st, 5),
                    (long long)sqlite3_column_int64(st, 6), ereason, eqid,
                    (long long)sqlite3_column_int64(st, 9), (long long)sqlite3_column_int64(st, 10),
                    (strcmp(qstate, "ready") == 0 || strcmp(qstate, "packed") == 0) ? "true" : "false",
                    sqlite3_column_int64(st, 12) == gen ? "true" : "false");
                first = 0;
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

/* Returns 1 if the request was handled. */
static void ls_send_reconcile_result(int sock, const char *title_id) {
    pthread_mutex_lock(&g_ls_mtx);
    char *report = NULL;
    if (g_ls_db && g_ls_schema >= 4) {
        sqlite3_stmt *st = NULL;
        if (title_id[0]) {
            if (sqlite3_prepare_v2(g_ls_db, "SELECT last_report FROM reconcile WHERE title_id = ?",
                                   -1, &st, NULL) == SQLITE_OK) {
                ls_bind_text(st, 1, title_id);
                if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
                    report = strdup((const char *)sqlite3_column_text(st, 0));
            }
            sqlite3_finalize(st);
        } else {
            size_t cap = 4096, pos = 0;
            report = malloc(cap);
            if (report) {
                report[0] = 0;
                ls_json_append(&report, &cap, &pos, "{\"titles\":[");
                int first = 1;
                if (sqlite3_prepare_v2(g_ls_db,
                        "SELECT title_id, COALESCE(last_result,''), COALESCE(recommended_action,''),"
                        " COALESCE(last_sanity_check,0), COALESCE(last_full_reconcile,0),"
                        " COALESCE(last_catalogue_scan,0) FROM reconcile ORDER BY title_id",
                        -1, &st, NULL) == SQLITE_OK) {
                    while (sqlite3_step(st) == SQLITE_ROW) {
                        char tid[32], res[40], act[200], eact[420];
                        ls_col_text(st, 0, tid, sizeof(tid));
                        ls_col_text(st, 1, res, sizeof(res));
                        ls_col_text(st, 2, act, sizeof(act));
                        json_escape(act, eact, sizeof(eact));
                        ls_json_append(&report, &cap, &pos,
                            "%s{\"title_id\":\"%s\",\"last_result\":\"%s\",\"recommended_action\":\"%s\","
                            "\"last_sanity_check\":%lld,\"last_full_reconcile\":%lld,\"last_catalogue_scan\":%lld}",
                            first ? "" : ",", tid, res, eact,
                            (long long)sqlite3_column_int64(st, 3), (long long)sqlite3_column_int64(st, 4),
                            (long long)sqlite3_column_int64(st, 5));
                        first = 0;
                    }
                }
                sqlite3_finalize(st);
                ls_json_append(&report, &cap, &pos, "]}");
            }
        }
    }
    pthread_mutex_unlock(&g_ls_mtx);
    if (report) http_json(sock, report);
    else http_json(sock, "{\"error\":\"No result stored for this title\"}");
    free(report);
}

/* /api/reconcile/... — manual diagnostics. Never takes g_mtx. */
/* One row per title for the web UI: name, saves, protection, last check. */
static void ls_send_titles(int sock) {
    ls_config_t dcfg;
    ls_load_config(&dcfg);
    int dest_pc = dcfg.storage_pc, dest_vps = dcfg.storage_vps || dcfg.vps_via_pc;   /* counts only for destinations in use */
    ls_tid_t *titles = NULL;
    int n = ls_catalogue_titles(&titles);
    ls_save_list_t l = {0};
    ls_enum_saves(&l, NULL);
    size_t cap = 8192, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { free(titles); ls_save_list_free(&l); http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    char running[32] = {0};
    auto_get_running_title(running, sizeof(running), NULL);
    ls_json_append(&buf, &cap, &pos, "{\"running\":\"%s\",\"titles\":[", running);
    for (int i = 0; i < n; i++) {
        const char *tid = titles[i].id;
        int saves = 0, objects = 0;
        for (int k = 0; k < l.count; k++) {
            if (strcmp(l.items[k].title_id, tid) != 0) continue;
            saves++;
            char id[256];
            int role = ls_object_of(l.items[k].save_name, id, sizeof(id));
            if (role == LS_ROLE_PRIMARY) { objects++; continue; }
            /* a backup copy counts as an object only when its primary is missing */
            char pname[300];
            ls_object_member_name(l.items[k].save_name, id, LS_ROLE_PRIMARY, pname, sizeof(pname));
            int has_primary = 0;
            for (int j = 0; j < l.count && !has_primary; j++)
                has_primary = strcmp(l.items[j].title_id, tid) == 0 && strcmp(l.items[j].user_id, l.items[k].user_id) == 0 &&
                              strcmp(l.items[j].save_name, pname) == 0;
            if (!has_primary) objects++;
        }
        int tracked = 0;
        int64_t last_backup = 0, pending = 0, last_check = 0, pending_vps = 0;
        char res[40] = {0}, act[300] = {0};
        pthread_mutex_lock(&g_ls_mtx);
        if (g_ls_db) {
            tracked = ls_title_tracked_locked(tid);
            last_backup = ls_db_int_locked("SELECT COALESCE(MAX(last_packaged_at),0) FROM saves WHERE title_id = ?",
                                           tid, NULL, NULL, 0);
            if (g_ls_schema >= 3)
                pending = ls_db_int_locked("SELECT COUNT(*) FROM saves s JOIN generations g ON g.gen_id = s.current_gen_id"
                                           " WHERE s.title_id = ? AND g.pc_acked_at = 0", tid, NULL, NULL, 0);
            pending_vps = ls_db_int_locked("SELECT COUNT(*) FROM queue WHERE title_id = ? AND state IN ('ready','packed')"
                                           " AND vault_ack_at = 0", tid, NULL, NULL, 0);
            if (g_ls_schema >= 4) {
                sqlite3_stmt *st = NULL;
                if (sqlite3_prepare_v2(g_ls_db, "SELECT COALESCE(last_result,''), COALESCE(recommended_action,''),"
                        " MAX(COALESCE(last_sanity_check,0), COALESCE(last_full_reconcile,0), COALESCE(last_catalogue_scan,0))"
                        " FROM reconcile WHERE title_id = ?", -1, &st, NULL) == SQLITE_OK) {
                    ls_bind_text(st, 1, tid);
                    if (sqlite3_step(st) == SQLITE_ROW) {
                        ls_col_text(st, 0, res, sizeof(res));
                        ls_col_text(st, 1, act, sizeof(act));
                        last_check = sqlite3_column_int64(st, 2);
                    }
                }
                sqlite3_finalize(st);
            }
        }
        pthread_mutex_unlock(&g_ls_mtx);
        const char *name = lookup_title(tid);
        char en[520], ea[620];
        json_escape(name ? name : "", en, sizeof(en));
        json_escape(act, ea, sizeof(ea));
        ls_json_append(&buf, &cap, &pos,
            "%s{\"title_id\":\"%s\",\"name\":\"%s\",\"saves\":%d,\"objects\":%d,\"recovery_open\":%lld,\"tracked\":%s,\"running\":%s,"
            "\"last_backup\":%lld,\"pending_pc\":%lld,\"pending_vps\":%lld,\"last_result\":\"%s\",\"recommended_action\":\"%s\",\"last_check\":%lld}",
            i ? "," : "", tid, en, saves, objects, (long long)ls_recovery_open_count(tid), tracked ? "true" : "false",
            strcmp(running, tid) == 0 ? "true" : "false", (long long)last_backup, (long long)(dest_pc ? pending : 0),
            (long long)(dest_vps ? pending_vps : 0), res, ea, (long long)last_check);
    }
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
    free(titles);
    ls_save_list_free(&l);
}

/*
 * GET /api/backup/objects?title=  — the game's save objects as they are on
 * the PS5 now: members (primary / sce_backup), per-file protection state,
 * aggregate state, and whether the PC still waits for any of it.
 */
static const char *ls_member_state_rank(const char *st, int *rank) {
    static const char *order[] = { "protected", "untracked", "unknown", "changed", "unstable", "missing", "error" };
    for (int i = 0; i < 7; i++) if (strcmp(st, order[i]) == 0) { *rank = i; return st; }
    *rank = 2;
    return st;
}

static void ls_send_objects(int sock, const char *title_id) {
    ls_save_list_t l = {0};
    ls_object_list_t o = {0};
    ls_enum_saves(&l, title_id);
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf || ls_group_objects(&l, &o) < 0) {
        free(buf); ls_save_list_free(&l); http_json(sock, "{\"error\":\"Out of memory\"}"); return;
    }
    ls_save_meta_t *metas = l.count ? calloc((size_t)l.count, sizeof(*metas)) : NULL;
    if (metas) ls_read_metas(&l, metas);
    buf[0] = 0;
    const char *name = lookup_title(title_id);
    char en[520];
    json_escape(name ? name : "", en, sizeof(en));
    ls_json_append(&buf, &cap, &pos, "{\"title_id\":\"%s\",\"name\":\"%s\",\"objects\":[", title_id, en);
    for (int k = 0; k < o.count; k++) {
        const ls_object_t *x = &o.items[k];
        char eid[520], fp[65] = {0};
        json_escape(x->id, eid, sizeof(eid));
        if (metas) ls_object_fingerprint(&l, metas, x, fp);
        int worst = 0, pending = 0, pending_vps = 0;
        int64_t modified = 0, backed = 0;
        const char *agg = "protected";
        ls_config_t dcfg;
        ls_load_config(&dcfg);
        int dest_vps = dcfg.storage_vps || dcfg.vps_via_pc;
        char lbl[200], elbl[420];
        ls_label_get(title_id, x->id, lbl, sizeof(lbl));
        json_escape(lbl, elbl, sizeof(elbl));
        size_t mark = pos;
        (void)mark;
        ls_json_append(&buf, &cap, &pos, "%s{\"id\":\"%s\",\"label\":\"%s\",\"user_id\":\"%s\",\"is_ps4\":%s,\"orphan\":%s,\"fingerprint\":\"%s\",\"files\":{",
                       k ? "," : "", eid, elbl, x->user_id, x->is_ps4 ? "true" : "false", ls_object_is_orphan(x) ? "true" : "false", fp);
        int firstf = 1;
        for (int role = 0; role < 2; role++) {
            int i = x->member[role];
            if (i < 0) continue;
            ls_save_row_t row;
            ls_saves_get(&l.items[i], &row);
            char ms[256] = "";
            if (metas) ls_save_meta_str(&metas[i], ms, sizeof(ms));
            const char *st = row.found ? row.state : "untracked";
            /* metadata moved since the last archive: changed, whatever the row says */
            if (row.found && strcmp(st, "protected") == 0 && strcmp(row.last_packaged_meta, ms) != 0) st = "changed";
            int rank = 0;
            ls_member_state_rank(st, &rank);
            if (rank > worst) { worst = rank; agg = st; }
            int64_t acked = 1;
            pthread_mutex_lock(&g_ls_mtx);
            if (g_ls_db && row.current_gen_id > 0)
                acked = ls_db_int_locked("SELECT pc_acked_at FROM generations WHERE ? = ? AND ? IS NOT NULL AND gen_id = ?",
                                         "x", "x", "x", row.current_gen_id);
            pthread_mutex_unlock(&g_ls_mtx);
            if (row.current_gen_id > 0 && acked == 0) pending = 1;
            int64_t vwait = 0;                     /* the current version's archive has not reached the vault */
            if (dest_vps && row.current_gen_id > 0) {
                pthread_mutex_lock(&g_ls_mtx);
                if (g_ls_db)
                    vwait = ls_db_int_locked("SELECT COUNT(*) FROM queue WHERE ? = ? AND ? IS NOT NULL AND gen_id = ?"
                                             " AND state IN ('ready','packed') AND vault_ack_at = 0", "x", "x", "x", row.current_gen_id);
                pthread_mutex_unlock(&g_ls_mtx);
                if (vwait) pending_vps = 1;
            }
            char es[520];
            json_escape(l.items[i].save_name, es, sizeof(es));
            int64_t mod = metas ? metas[i].mtime_ns / 1000000000LL : 0;
            if (mod > modified) modified = mod;
            if (row.last_packaged_at > backed) backed = row.last_packaged_at;
            ls_json_append(&buf, &cap, &pos, "%s\"%s\":{\"save_name\":\"%s\",\"state\":\"%s\",\"current_gen\":%lld,"
                           "\"last_packaged_at\":%lld,\"modified_at\":%lld,\"size\":%lld,\"pending_pc\":%s,\"pending_vps\":%s}",
                           firstf ? "" : ",", ls_role_name(role), es, st, (long long)row.current_gen_id,
                           (long long)row.last_packaged_at, (long long)mod, metas ? (long long)metas[i].size : 0LL,
                           (row.current_gen_id > 0 && acked == 0) ? "true" : "false", vwait ? "true" : "false");
            firstf = 0;
        }
        ls_json_append(&buf, &cap, &pos, "},\"state\":\"%s\",\"pending_pc\":%s,\"pending_vps\":%s,\"modified_at\":%lld,\"last_backup\":%lld}",
                       agg, pending && dcfg.storage_pc ? "true" : "false", pending_vps ? "true" : "false", (long long)modified, (long long)backed);
    }
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
    free(metas);
    ls_object_list_free(&o);
    ls_save_list_free(&l);
}

/*
 * One snapshot as a self-describing manifest (format "shallot-snapshot" v1):
 * game, user, the save objects with their files (relative names), the
 * generation / container hash of each file and the archive (queue id) that
 * holds it, plus whether that archive is still on the PS5 / on the PC.
 */
static void ls_snapshot_json(int64_t snap_id, char **buf, size_t *cap, size_t *pos) {
    char user[16] = {0}, title[32] = {0}, kind[16] = {0}, obj[256] = {0}, reason[64] = {0};
    int64_t created = 0;
    int nobj = 0, nfiles = 0;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "SELECT user_id, title_id, kind, object_id, created_at, reason, objects, files"
                                      " FROM snapshots WHERE snap_id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)snap_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            ls_col_text(st, 0, user, sizeof(user));
            ls_col_text(st, 1, title, sizeof(title));
            ls_col_text(st, 2, kind, sizeof(kind));
            ls_col_text(st, 3, obj, sizeof(obj));
            created = sqlite3_column_int64(st, 4);
            ls_col_text(st, 5, reason, sizeof(reason));
            nobj = sqlite3_column_int(st, 6);
            nfiles = sqlite3_column_int(st, 7);
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_snap_file_t *f = NULL;
    int n = ls_snapshot_files(snap_id, &f);
    const char *name = lookup_title(title);
    char en[520], eobj[520];
    json_escape(name ? name : "", en, sizeof(en));
    json_escape(obj, eobj, sizeof(eobj));
    /* archive availability per file */
    int on_ps5_all = 1, on_pc_all = 1;
    int *on_ps5 = n ? calloc((size_t)n, sizeof(int)) : NULL, *on_pc = n ? calloc((size_t)n, sizeof(int)) : NULL;
    for (int i = 0; i < n && on_ps5 && on_pc; i++) {
        ls_queue_row_t q;
        on_ps5[i] = f[i].queue_id[0] && ls_queue_get(f[i].queue_id, &q) &&
                    (strcmp(q.state, "ready") == 0 || strcmp(q.state, "packed") == 0);
        pthread_mutex_lock(&g_ls_mtx);
        on_pc[i] = g_ls_db && ls_db_int_locked("SELECT pc_acked_at > 0 FROM generations WHERE ? = ? AND ? IS NOT NULL AND gen_id = ?",
                                               "x", "x", "x", f[i].gen_id) > 0;
        pthread_mutex_unlock(&g_ls_mtx);
        if (!on_ps5[i]) on_ps5_all = 0;
        if (!on_pc[i]) on_pc_all = 0;
    }
    ls_json_append(buf, cap, pos,
        "{\"format\":\"shallot-snapshot\",\"version\":1,\"snapshot_id\":%lld,\"kind\":\"%s\",\"object_id\":\"%s\","
        "\"game\":{\"title_id\":\"%s\",\"name\":\"%s\"},\"user_id\":\"%s\",\"created_at\":%lld,\"reason\":\"%s\","
        "\"object_count\":%d,\"file_count\":%d,\"restorable_on_ps5\":%s,\"all_on_pc\":%s,\"objects\":[",
        (long long)snap_id, kind, eobj, title, en, user, (long long)created, reason, nobj, nfiles,
        n && on_ps5_all ? "true" : "false", n && on_pc_all ? "true" : "false");
    for (int i = 0; i < n; i++) {
        int first_of_obj = i == 0 || strcmp(f[i].object_id, f[i - 1].object_id) != 0;
        int last_of_obj = i == n - 1 || strcmp(f[i].object_id, f[i + 1].object_id) != 0;
        char eid[520], es[520], eq[420];
        json_escape(f[i].object_id, eid, sizeof(eid));
        json_escape(f[i].save_name, es, sizeof(es));
        json_escape(f[i].queue_id, eq, sizeof(eq));
        if (first_of_obj) {
            char lbl[200], elbl[420];
            ls_label_get(title, f[i].object_id, lbl, sizeof(lbl));
            json_escape(lbl, elbl, sizeof(elbl));
            ls_json_append(buf, cap, pos, "%s{\"id\":\"%s\",\"label\":\"%s\",\"members\":[", i ? "," : "", eid, elbl);
        }
        ls_json_append(buf, cap, pos, "%s{\"role\":\"%s\",\"file\":\"%s\",\"is_ps4\":%s,\"gen_id\":%lld,\"sha256\":\"%s\","
                       "\"size\":%lld,\"archive_id\":\"%s\",\"archive_on_ps5\":%s,\"on_pc\":%s}",
                       first_of_obj ? "" : ",", ls_role_name(f[i].role), es, f[i].is_ps4 ? "true" : "false",
                       (long long)f[i].gen_id, f[i].sha, (long long)f[i].size, eq,
                       on_ps5 && on_ps5[i] ? "true" : "false", on_pc && on_pc[i] ? "true" : "false");
        if (last_of_obj) ls_json_append(buf, cap, pos, "]}");
    }
    ls_json_append(buf, cap, pos, "]}");
    free(on_ps5);
    free(on_pc);
    free(f);
}

/* GET /api/backup/snapshots?title=TID  or  ?since=<snap_id> (all titles, oldest first, max 100). */
static void ls_send_snapshots(int sock, const char *title_id, int64_t since) {
    enum { MAXS = 100 };
    int64_t ids[MAXS];
    int n = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 6) {
        sqlite3_stmt *st = NULL;
        const char *sql = title_id[0]
            ? "SELECT snap_id FROM snapshots WHERE title_id = ? ORDER BY snap_id DESC LIMIT 100"
            : "SELECT snap_id FROM snapshots WHERE snap_id > ? ORDER BY snap_id ASC LIMIT 100";
        if (sqlite3_prepare_v2(g_ls_db, sql, -1, &st, NULL) == SQLITE_OK) {
            if (title_id[0]) ls_bind_text(st, 1, title_id);
            else sqlite3_bind_int64(st, 1, (sqlite3_int64)since);
            while (n < MAXS && sqlite3_step(st) == SQLITE_ROW) ids[n++] = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"snapshots\":[");
    for (int i = 0; i < n; i++) {
        if (i) ls_json_append(&buf, &cap, &pos, ",");
        ls_snapshot_json(ids[i], &buf, &cap, &pos);
    }
    ls_json_append(&buf, &cap, &pos, "]}");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

#define LS_TOKEN_MASK "********"
static void ls_send_sync_settings(int sock) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (cfg.vault_token[0]) snprintf(cfg.vault_token, sizeof(cfg.vault_token), LS_TOKEN_MASK);   /* never shown again */
    cfg.ui_pass_salt[0] = cfg.ui_pass_hash[0] = cfg.client_key[0] = 0;   /* Settings > Web UI login handles these */
    char json[4096], body[4300];
    ls_config_to_json(&cfg, json, sizeof(json));
    snprintf(body, sizeof(body), "{\"path\":\"%s\",\"config\":%s}", LS_CONFIG_PATH, json);
    http_json(sock, body);
}

static void ls_handle_sync_settings_post(int sock, char *req, int n) {
    char *iobuf = malloc(BUF_SIZE);
    char *body = NULL, err[128] = {0};
    size_t blen = 0;
    if (!iobuf || read_request_body(sock, req, n, iobuf, &body, &blen, err, sizeof(err)) < 0) {
        char j[256], e[200];
        json_escape(err[0] ? err : "Out of memory", e, sizeof(e));
        snprintf(j, sizeof(j), "{\"error\":\"%s\"}", e);
        http_json(sock, j);
        free(iobuf); free(body);
        return;
    }
    pthread_mutex_lock(&g_ls_cfg_mtx);
    ls_config_t cfg, old;
    ls_load_config(&cfg);          /* start from current values; posted keys override */
    old = cfg;
    ls_parse_config(body, &cfg);
    if (strcmp(cfg.vault_token, LS_TOKEN_MASK) == 0) snprintf(cfg.vault_token, sizeof(cfg.vault_token), "%s", old.vault_token);
    /* the login and the PC client list are changed through their own routes only */
    memcpy(cfg.ui_pass_salt, old.ui_pass_salt, sizeof(cfg.ui_pass_salt));
    memcpy(cfg.ui_pass_hash, old.ui_pass_hash, sizeof(cfg.ui_pass_hash));
    memcpy(cfg.client_key, old.client_key, sizeof(cfg.client_key));
    memcpy(cfg.pc_active, old.pc_active, sizeof(cfg.pc_active));
    memcpy(cfg.pc_blocked, old.pc_blocked, sizeof(cfg.pc_blocked));
    ls_config_clamp(&cfg);
    int save_rc = ls_save_config(&cfg);
    pthread_mutex_unlock(&g_ls_cfg_mtx);
    if (save_rc < 0) {
        http_json(sock, "{\"error\":\"Could not write sync.json\"}");
    } else {
        ls_event("SYNC_SETTINGS_SAVED", NULL, "game_close=%d stability=%d/%ds/%ds idle=%d/%dmin untracked=%d",
                 cfg.game_close_backup, cfg.stability_samples, cfg.stability_interval_s, cfg.stability_timeout_s,
                 cfg.idle_sweep, cfg.idle_delay_min, cfg.startup_protect_untracked);
        ls_wait_bump("config");            /* the PC client re-reads its vault settings */
        ls_send_sync_settings(sock);
    }
    free(iobuf);
    free(body);
}

/* ── Diagnostics: where does a title keep its saves? (read-only) ───── */
/*
 * GET /api/backup/diag/title?id=<TID>[&recent=<minutes>]
 *  - every entry (any type, incl. dot files) under
 *    /user/home/<user>/savedata_prospero/<TID> and /savedata/<TID>;
 *  - savedata.db rows for the title (PS5 + PS4 DBs, every user);
 *  - files/dirs under /user/home/<user> (depth <= 5) modified in the last
 *    `recent` minutes (default 30), to find where a game actually wrote.
 * Only stat/readdir and read-only SQLite; nothing is opened for writing.
 */
static const char *ls_diag_type(mode_t m) {
    return S_ISREG(m) ? "file" : S_ISDIR(m) ? "dir" : S_ISLNK(m) ? "link" : "other";
}

static void ls_diag_entry(char **buf, size_t *cap, size_t *pos, int *first, const char *path,
                          const struct stat *st) {
    char ep[MAX_PATH_LEN * 2];
    json_escape(path, ep, sizeof(ep));
    ls_json_append(buf, cap, pos,
        "%s{\"path\":\"%s\",\"type\":\"%s\",\"size\":%lld,\"mode\":\"%o\",\"mtime\":%lld}",
        *first ? "" : ",", ep, ls_diag_type(st->st_mode), (long long)st->st_size,
        (unsigned)(st->st_mode & 07777), (long long)st->st_mtime);
    *first = 0;
}

static void ls_diag_list_dir(char **buf, size_t *cap, size_t *pos, int *first, const char *dir, int depth) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[MAX_PATH_LEN];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) < 0) continue;
        ls_diag_entry(buf, cap, pos, first, p, &st);
        if (S_ISDIR(st.st_mode) && depth > 0) ls_diag_list_dir(buf, cap, pos, first, p, depth - 1);
    }
    closedir(d);
}

static void ls_diag_recent(char **buf, size_t *cap, size_t *pos, int *first, int *count,
                           const char *dir, time_t since, int depth) {
    if (*count >= 300) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && *count < 300) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char p[MAX_PATH_LEN];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) < 0) continue;
        if (st.st_mtime >= since) { ls_diag_entry(buf, cap, pos, first, p, &st); (*count)++; }
        if (S_ISDIR(st.st_mode) && depth > 0) ls_diag_recent(buf, cap, pos, first, count, p, since, depth - 1);
    }
    closedir(d);
}

static void ls_send_diag_title(int sock, const char *tid, int recent_min) {
    size_t cap = 16 * 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"title_id\":\"%s\",\"save_dirs\":[", tid);
    int first = 1;
    DIR *home = opendir(LS_USER_HOME);
    if (home) {
        struct dirent *u;
        while ((u = readdir(home))) {
            if (u->d_name[0] == '.') continue;
            for (int ps4 = 0; ps4 < 2; ps4++) {
                char dir[MAX_PATH_LEN];
                struct stat st;
                snprintf(dir, sizeof(dir), "%s/%s/%s/%s", LS_USER_HOME, u->d_name,
                         ps4 ? "savedata" : "savedata_prospero", tid);
                if (lstat(dir, &st) < 0) continue;
                ls_diag_entry(&buf, &cap, &pos, &first, dir, &st);
                if (S_ISDIR(st.st_mode)) ls_diag_list_dir(&buf, &cap, &pos, &first, dir, 2);
            }
        }
        closedir(home);
    }

    ls_json_append(&buf, &cap, &pos, "],\"savedata_db\":[");
    first = 1;
    home = opendir(LS_USER_HOME);
    if (home) {
        struct dirent *u;
        while ((u = readdir(home))) {
            if (u->d_name[0] == '.') continue;
            for (int ps4 = 0; ps4 < 2; ps4++) {
                sqlite3 *db = ls_open_savedata_db(u->d_name, ps4);
                if (!db) continue;
                sqlite3_stmt *st = NULL;
                if (sqlite3_prepare_v2(db, "SELECT dir_name, COALESCE(mtime,''), COALESCE(main_title,'')"
                                       " FROM savedata WHERE title_id = ?", -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, tid, -1, SQLITE_TRANSIENT);
                    while (sqlite3_step(st) == SQLITE_ROW) {
                        char dn[256], mt[64], mn[256], edn[520], emt[140], emn[520];
                        ls_col_text(st, 0, dn, sizeof(dn));
                        ls_col_text(st, 1, mt, sizeof(mt));
                        ls_col_text(st, 2, mn, sizeof(mn));
                        json_escape(dn, edn, sizeof(edn));
                        json_escape(mt, emt, sizeof(emt));
                        json_escape(mn, emn, sizeof(emn));
                        ls_json_append(&buf, &cap, &pos,
                            "%s{\"user_id\":\"%s\",\"db\":\"%s\",\"dir_name\":\"%s\",\"mtime\":\"%s\",\"main_title\":\"%s\"}",
                            first ? "" : ",", u->d_name, ps4 ? "savedata" : "savedata_prospero", edn, emt, emn);
                        first = 0;
                    }
                }
                sqlite3_finalize(st);
                sqlite3_close(db);
            }
        }
        closedir(home);
    }

    if (recent_min <= 0 || recent_min > 24 * 60) recent_min = 30;
    time_t since = time(NULL) - (time_t)recent_min * 60;
    ls_json_append(&buf, &cap, &pos, "],\"recent_minutes\":%d,\"recently_modified\":[", recent_min);
    first = 1;
    int count = 0;
    ls_diag_recent(&buf, &cap, &pos, &first, &count, LS_USER_HOME, since, 5);
    ls_json_append(&buf, &cap, &pos, "],\"recent_truncated\":%s}", count >= 300 ? "true" : "false");
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

static int ls_reconcile_route(int sock, const char *method, const char *url) {
    if (!g_ls_ready) {
        http_json(sock, "{\"error\":\"LAN sync state DB unavailable\",\"ready\":false}");
        return 1;
    }
    char tid[32] = {0};
    char *p = strstr(url, "id=");
    if (!p) p = strstr(url, "title=") ? strstr(url, "title=") + 3 : NULL;
    if (p) url_param_decode(p + 3, tid, sizeof(tid));

    /* Level 1: Sanity Check — cheap, synchronous, read-only on saves. */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/reconcile/title/sanity", 27) == 0) {
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        char *json = ls_sanity_title_json(tid);
        if (json) { http_json(sock, json); free(json); }
        else http_json(sock, "{\"error\":\"Sanity check failed\"}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/reconcile/result", 21) == 0) {
        if (tid[0] && !ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        ls_send_reconcile_result(sock, tid);
        return 1;
    }
    /* Level 3: Full Reconcile Game — hashes every save, background job.
     * protect=1 is the separate "Full Reconcile + Protect Changed Saves". */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/reconcile/title/full", 25) == 0) {
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        if (ls_title_is_running(tid)) {
            http_json(sock, "{\"error\":\"Title is running; run Full Reconcile after closing it\"}");
            return 1;
        }
        int rc = ls_full_title_start(tid, strstr(url, "protect=1") != NULL);
        http_json(sock, rc == 0 ? "{\"ok\":true,\"started\":true,\"workload\":\"moderate\"}" :
                        rc == -1 ? "{\"error\":\"Another reconciliation job is running\"}" :
                                   "{\"error\":\"Could not start the job\"}");
        return 1;
    }
    /* Level 2 / 4 / 5: catalogue jobs (background, one at a time). */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/reconcile/catalogue/quick", 30) == 0) {
        int rc = ls_catalogue_start(0, 0);
        http_json(sock, rc == 0 ? "{\"ok\":true,\"started\":true,\"workload\":\"quick\"}" :
                        rc == -1 ? "{\"error\":\"Another reconciliation job is running\"}" :
                                   "{\"error\":\"Could not start the job\"}");
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/reconcile/catalogue/full", 29) == 0) {
        int rc = ls_catalogue_start(1, strstr(url, "protect=1") != NULL);
        http_json(sock, rc == 0 ? "{\"ok\":true,\"started\":true,\"workload\":\"deep\"}" :
                        rc == -1 ? "{\"error\":\"Another reconciliation job is running\"}" :
                                   "{\"error\":\"Could not start the job\"}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/reconcile/catalogue/result", 31) == 0) {
        char *j = ls_meta_get_text(strstr(url, "kind=full") ? "last_catalogue_full" : "last_catalogue_quick");
        if (j) { http_json(sock, j); free(j); }
        else http_json(sock, "{\"error\":\"No catalogue result yet\"}");
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/reconcile/cancel", 21) == 0) {
        pthread_mutex_lock(&g_ls_rjob_mtx);
        int active = g_ls_rjob.active;
        if (active) g_ls_rjob.cancel = 1;
        pthread_mutex_unlock(&g_ls_rjob_mtx);
        http_json(sock, active ? "{\"ok\":true,\"cancelling\":true}" : "{\"ok\":true,\"cancelling\":false}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/reconcile/status", 21) == 0) {
        size_t cap = 1024, pos = 0;
        char *buf = malloc(cap);
        if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return 1; }
        buf[0] = 0;
        ls_json_append(&buf, &cap, &pos, "{\"job\":");
        ls_rjob_status_json(&buf, &cap, &pos);
        ls_json_append(&buf, &cap, &pos, "}");
        http_json(sock, buf);
        free(buf);
        return 1;
    }
    http_send(sock, "404 Not Found", "application/json", LS_ERR_404, (int)strlen(LS_ERR_404));
    return 1;
}

/* ── Restore (stage M) ──────────────────────────────────────────── */
/* Streams a request body into dst (.part -> fsync -> rename), hashing it. */
static int ls_receive_file(int sock, char *req, int n, const char *dst, size_t max, char sha[65]) {
    char *cl = strcasestr(req, "Content-Length:");
    size_t clen = cl ? strtoul(cl + 15, NULL, 10) : 0;
    char *bs = strstr(req, "\r\n\r\n");
    if (!clen || clen > max || !bs) return -1;
    bs += 4;
    size_t inbuf = (size_t)(n - (bs - req));
    if (inbuf > clen) inbuf = clen;
    char part[MAX_PATH_LEN];
    snprintf(part, sizeof(part), "%s.part", dst);
    int fd = open(part, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) return -1;
    ls_sha256_t c;
    ls_sha256_init(&c);
    int rc = ls_write_all(fd, bs, inbuf);
    ls_sha256_update(&c, bs, inbuf);
    size_t got = inbuf;
    char *buf = malloc(BUF_SIZE);
    while (rc == 0 && buf && got < clen) {
        size_t want = clen - got < BUF_SIZE ? clen - got : BUF_SIZE;
        ssize_t r = recv_all(sock, buf, want);
        if (r <= 0) { rc = -1; break; }
        rc = ls_write_all(fd, buf, (size_t)r);
        ls_sha256_update(&c, buf, (size_t)r);
        got += (size_t)r;
    }
    free(buf);
    if (!buf || got != clen) rc = -1;
    if (rc == 0 && fsync(fd) < 0) rc = -1;
    if (close(fd) < 0) rc = -1;
    if (rc < 0 || rename(part, dst) < 0) { unlink(part); return -1; }
    ls_sha256_final_hex(&c, sha);
    return 0;
}

static void ls_restore_reply(int sock, int rc, const char *err) {
    char j[512], e[400];
    if (rc == 0) { http_json(sock, "{\"ok\":true,\"queued\":true}"); return; }
    json_escape(err, e, sizeof(e));
    snprintf(j, sizeof(j), "{\"error\":\"%s\"}", e);
    http_json(sock, j);
}

/* POST /api/restore/from_queue?id=<queue id>[&user=<uid>]: restore a version Shallot still holds. */
static void ls_restore_from_queue(int sock, const char *url) {
    char id[256] = {0}, user[32] = {0}, err[300] = {0};
    char *p;
    if ((p = strstr(url, "id="))) url_param_decode(p + 3, id, sizeof(id));
    if ((p = strstr(url, "user="))) url_param_decode(p + 5, user, sizeof(user));
    if (!safe_queue_id(id)) { http_json(sock, "{\"error\":\"Invalid id\"}"); return; }
    ls_queue_row_t q;
    if (ls_queue_get(id, &q) && strcmp(q.state, "packed") == 0 && ls_archive_ensure_ready(id, err, sizeof(err)) == 0)
        ls_queue_get(id, &q);
    if (!ls_queue_get(id, &q) || strcmp(q.state, "ready") != 0) {
        http_json(sock, "{\"error\":\"That archive is no longer on the PS5; restore it from the PC instead\"}");
        return;
    }
    if (!ls_valid_save_name(q.save_name)) {
        http_json(sock, "{\"error\":\"Title-wide archive: restore single saves from per-save archives\"}");
        return;
    }
    if (!user[0]) snprintf(user, sizeof(user), "%s", q.user_id);
    char expected[65] = {0};
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && q.gen_id > 0) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT sha256 FROM generations WHERE gen_id = ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)q.gen_id);
            if (sqlite3_step(st) == SQLITE_ROW) ls_col_text(st, 0, expected, sizeof(expected));
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, q.filename);
    int rc = ls_restore_request("queue", user, q.title_id, q.save_name, path, expected, q.gen_id, err, sizeof(err));
    ls_restore_reply(sock, rc, err);
}

/* POST /api/restore/upload?title=&save=&user=&archive_sha256=[&container_sha256=]  body = ZIP from the PC. */
static void ls_restore_upload(int sock, const char *url, char *req, int n) {
    char tid[32] = {0}, save[256] = {0}, user[32] = {0}, ash[80] = {0}, csh[80] = {0}, err[300] = {0};
    char *p;
    if ((p = strstr(url, "title="))) url_param_decode(p + 6, tid, sizeof(tid));
    if ((p = strstr(url, "save="))) url_param_decode(p + 5, save, sizeof(save));
    if ((p = strstr(url, "user="))) url_param_decode(p + 5, user, sizeof(user));
    if ((p = strstr(url, "archive_sha256="))) url_param_decode(p + 15, ash, sizeof(ash));
    if ((p = strstr(url, "container_sha256="))) url_param_decode(p + 17, csh, sizeof(csh));
    if (!ls_valid_title_id(tid) || !ls_valid_save_name(save) || !user[0] || !ls_hex64(ash)) {
        http_json(sock, "{\"error\":\"title, save, user and archive_sha256 are required\"}");
        return;
    }
    if (csh[0] && !ls_hex64(csh)) csh[0] = 0;
    if (ls_title_is_running(tid)) { http_json(sock, "{\"error\":\"Title is running; close it before restoring\"}"); return; }
    mkdir(LS_RESTORE_DIR, 0777);
    char stamp[32], dst[MAX_PATH_LEN], got[65];
    cloud_timestamp(stamp, sizeof(stamp));
    snprintf(dst, sizeof(dst), "%s/upload_%s_%d.zip", LS_RESTORE_DIR, stamp, (int)(time(NULL) % 1000));
    if (ls_receive_file(sock, req, n, dst, (size_t)2048 * 1024 * 1024, got) < 0) {
        http_json(sock, "{\"error\":\"Upload incomplete or too large\"}");
        return;
    }
    if (strcasecmp(got, ash) != 0) {
        unlink(dst);
        http_json(sock, "{\"error\":\"Uploaded archive does not match archive_sha256\"}");
        return;
    }
    int64_t gen = 0;
    if (csh[0]) {
        ls_save_ref_t ref;
        memset(&ref, 0, sizeof(ref));
        snprintf(ref.user_id, sizeof(ref.user_id), "%s", user);
        snprintf(ref.title_id, sizeof(ref.title_id), "%s", tid);
        snprintf(ref.save_name, sizeof(ref.save_name), "%s", save);
        gen = ls_gen_find(&ref, csh);
    }
    int rc = ls_restore_request("upload", user, tid, save, dst, csh, gen, err, sizeof(err));
    if (rc != 0) unlink(dst);
    ls_restore_reply(sock, rc, err);
}

/* POST /api/restore/snapshot?id=<snap_id>[&object=<object id>][&exact=1&remove_fp=<from the preview>] */
static void ls_restore_snapshot_req(int sock, const char *url) {
    char ids[32] = {0}, obj[256] = {0}, err[300] = {0}, ex[8] = {0}, fp[80] = {0}, sim[8] = {0};
    char *p;
    if ((p = strstr(url, "?id=")) || (p = strstr(url, "&id="))) url_param_decode(p + 4, ids, sizeof(ids));
    if ((p = strstr(url, "object="))) url_param_decode(p + 7, obj, sizeof(obj));
    if ((p = strstr(url, "exact="))) url_param_decode(p + 6, ex, sizeof(ex));
    if ((p = strstr(url, "remove_fp="))) url_param_decode(p + 10, fp, sizeof(fp));
    if ((p = strstr(url, "simulate_regression="))) url_param_decode(p + 20, sim, sizeof(sim));
    int64_t id = atoll(ids);
    if (id <= 0 || strchr(obj, '/') || strstr(obj, "..")) { http_json(sock, "{\"error\":\"Invalid request\"}"); return; }
    int rc = strcmp(sim, "1") == 0 ? ls_restore_request_simulated_regression(id, obj, err, sizeof(err))
                                     : ls_restore_request_snapshot_ex(id, obj[0] ? obj : NULL, strcmp(ex, "1") == 0, fp, err, sizeof(err));
    ls_restore_reply(sock, rc, err);
}

/* GET /api/restore/snapshot_preview?id=<snap_id> */
static void ls_restore_preview(int sock, const char *url) {
    char ids[32] = {0}, err[300] = {0};
    char *p;
    if ((p = strstr(url, "id="))) url_param_decode(p + 3, ids, sizeof(ids));
    size_t cap = 8192, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    if (ls_restore_preview_json(atoll(ids), &buf, &cap, &pos, err, sizeof(err)) < 0) {
        free(buf);
        char e[400], j[460];
        json_escape(err, e, sizeof(e));
        snprintf(j, sizeof(j), "{\"error\":\"%s\"}", e);
        http_json(sock, j);
        return;
    }
    http_send(sock, "200 OK", "application/json", buf, (int)pos);
    free(buf);
}

static void ls_send_restore_status(int sock) {
    size_t cap = 1024, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return; }
    buf[0] = 0;
    ls_json_append(&buf, &cap, &pos, "{\"restore\":");
    ls_restore_status_json(&buf, &cap, &pos);
    ls_json_append(&buf, &cap, &pos, "}");
    http_json(sock, buf);
    free(buf);
}

/*
 * POST /api/pc/readopt?id=<queue id>  body = the archive, from the PC client.
 * Puts a pruned archive back into the queue when it matches the SHA-256
 * recorded at packaging time; the acknowledgement time is renewed so it
 * stays for keep_acked_hours.
 */
static void ls_pc_readopt(int sock, const char *url, char *req, int n) {
    char id[256] = {0};
    const char *p = strstr(url, "id=");
    if (p) url_param_decode(p + 3, id, sizeof(id));
    ls_queue_row_t q;
    if (!safe_queue_id(id) || !ls_queue_get(id, &q)) { http_json(sock, "{\"error\":\"Unknown archive\"}"); return; }
    ls_pc_touch();
    char final_path[MAX_PATH_LEN], got[65] = {0};
    snprintf(final_path, sizeof(final_path), "%s/%s", LS_QUEUE_DIR, q.filename);
    if (strcmp(q.state, "ready") == 0 || strcmp(q.state, "packed") == 0) { http_json(sock, "{\"ok\":true,\"already\":true}"); ls_fetch_arrived(id); return; }
    mkdir(LS_QUEUE_DIR, 0777);
    if (ls_receive_file(sock, req, n, final_path, (size_t)(q.size > 0 ? q.size : 1), got) < 0) {
        ls_fetch_failed(id, "upload from the PC was incomplete");
        http_json(sock, "{\"error\":\"Upload incomplete or wrong size\"}");
        return;
    }
    if (strcasecmp(got, q.sha256) != 0) {
        unlink(final_path);
        ls_fetch_failed(id, "the PC's copy does not match the archive Shallot made");
        ls_event("PC_FETCH_MISMATCH", q.title_id, "id=%s sha256 %.16s != %.16s", id, got, q.sha256);
        http_json(sock, "{\"error\":\"Archive does not match its SHA-256\"}");
        return;
    }
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET state = 'ready', lan_ack_at = ? WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)time(NULL));
            ls_bind_text(st, 2, id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    ls_event("ARCHIVE_FROM_PC", q.title_id, "id=%s save=%s back on the PS5 (sha256 verified)", id, q.save_name);
    ls_fetch_arrived(id);
    http_json(sock, "{\"ok\":true}");
}

/*
 * Start with the console (onionHEN payload auto-start).
 *   GET  /api/system/autostart             {stored, onionhen, autostart}
 *   POST /api/system/autostart?action=upload   body = shallot-savemgr.elf (from the PC)
 *   POST /api/system/autostart?action=on|off   (the marker next to the ELF)
 * The ELF is kept in /data/shallot and, when onionHEN is installed, in
 * /data/OnionHEN/payloads with "<name>.elf.auto_start" next to it; onionHEN
 * starts every payload with that marker after it booted.
 */
#define LS_SELF_ELF LS_BASE_DIR "/shallot-savemgr.elf"
#ifndef LS_ONION_DIR
#define LS_ONION_DIR "/data/OnionHEN"              /* host tests point this into their sandbox */
#endif
#define LS_ONION_ELF LS_ONION_DIR "/payloads/shallot-savemgr.elf"
#define LS_ONION_MARK LS_ONION_ELF ".auto_start"

static int ls_file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static int ls_copy_file_atomic(const char *src, const char *dst, char *iobuf) {
    char tmp[MAX_PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
    int in = open(src, O_RDONLY), out = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0777);
    int ok = in >= 0 && out >= 0;
    ssize_t r;
    while (ok && (r = read(in, iobuf, BUF_SIZE)) > 0) ok = ls_write_all(out, iobuf, (size_t)r) == 0;
    if (out >= 0) { if (ok && fsync(out) < 0) ok = 0; close(out); }
    if (in >= 0) close(in);
    if (ok && rename(tmp, dst) < 0) ok = 0;
    if (!ok) unlink(tmp);
    return ok ? 0 : -1;
}

static void ls_autostart_json(int sock) {
    char j[300];
    snprintf(j, sizeof(j), "{\"stored\":%s,\"onionhen\":%s,\"in_payloads\":%s,\"autostart\":%s}",
             ls_file_exists(LS_SELF_ELF) ? "true" : "false", ls_file_exists(LS_ONION_DIR) ? "true" : "false",
             ls_file_exists(LS_ONION_ELF) ? "true" : "false", ls_file_exists(LS_ONION_MARK) ? "true" : "false");
    http_json(sock, j);
}

static void ls_autostart_route(int sock, const char *method, const char *url, char *req, int n) {
    if (strcmp(method, "GET") == 0) { ls_autostart_json(sock); return; }
    char err[200] = "";
    if (strstr(url, "action=upload")) {
        char part[MAX_PATH_LEN], got[65];
        snprintf(part, sizeof(part), "%s.upload", LS_SELF_ELF);
        mkdir(LS_BASE_DIR, 0777);
        if (ls_receive_file(sock, req, n, part, (size_t)64 * 1024 * 1024, got) < 0) {
            http_json(sock, "{\"error\":\"Upload incomplete or larger than 64 MB\"}");
            return;
        }
        unsigned char magic[4] = {0};
        int fd = open(part, O_RDONLY);
        if (fd >= 0) { if (read(fd, magic, 4) != 4) memset(magic, 0, 4); close(fd); }
        if (memcmp(magic, "\x7f" "ELF", 4) != 0) {
            unlink(part);
            http_json(sock, "{\"error\":\"That is not an ELF file (choose shallot-savemgr.elf)\"}");
            return;
        }
        if (rename(part, LS_SELF_ELF) < 0) { unlink(part); http_json(sock, "{\"error\":\"Cannot store the file\"}"); return; }
        ls_event("SELF_STORED", NULL, "shallot-savemgr.elf stored on the PS5 (sha256 %.16s)", got);
        /* an existing onionHEN copy is refreshed too */
        if (ls_file_exists(LS_ONION_ELF)) {
            char *iobuf = malloc(BUF_SIZE);
            if (iobuf) { ls_copy_file_atomic(LS_SELF_ELF, LS_ONION_ELF, iobuf); free(iobuf); }
        }
        ls_autostart_json(sock);
        return;
    }
    if (strstr(url, "action=on")) {
        if (!ls_file_exists(LS_SELF_ELF)) { http_json(sock, "{\"error\":\"Upload shallot-savemgr.elf first\"}"); return; }
        if (!ls_file_exists(LS_ONION_DIR)) { http_json(sock, "{\"error\":\"onionHEN was not found (/data/OnionHEN)\"}"); return; }
        mkdir(LS_ONION_DIR "/payloads", 0777);
        char *iobuf = malloc(BUF_SIZE);
        int rc = iobuf ? ls_copy_file_atomic(LS_SELF_ELF, LS_ONION_ELF, iobuf) : -1;
        free(iobuf);
        int mfd = rc == 0 ? open(LS_ONION_MARK, O_CREAT | O_WRONLY | O_TRUNC, 0666) : -1;
        if (mfd >= 0) close(mfd);
        if (rc < 0 || mfd < 0) { snprintf(err, sizeof(err), "cannot write to %s/payloads (errno %d)", LS_ONION_DIR, errno); }
        else ls_event("AUTOSTART_ON", NULL, "onionHEN will start Shallot after boot (%s)", LS_ONION_MARK);
    } else if (strstr(url, "action=off")) {
        unlink(LS_ONION_MARK);
        ls_event("AUTOSTART_OFF", NULL, "onionHEN auto-start for Shallot removed (the payload stays in its list)");
    } else {
        http_json(sock, "{\"error\":\"action must be upload, on or off\"}");
        return;
    }
    if (err[0]) {
        char e[260], j[300];
        json_escape(err, e, sizeof(e));
        snprintf(j, sizeof(j), "{\"error\":\"%s\"}", e);
        http_json(sock, j);
        return;
    }
    ls_autostart_json(sock);
}

/* Routes that need the raw request (body). Returns 1 if handled. */
static int ls_http_route_req(int sock, const char *method, const char *url, char *req, int n) {
    if (strncmp(url, "/api/vault/", 11) == 0) {
        if (!g_ls_ready) { http_json(sock, "{\"error\":\"LAN sync state DB unavailable\"}"); return 1; }
        return ls_vault_route(sock, method, url, req, n);
    }
    if (strncmp(url, "/api/system/autostart", 21) == 0) {
        ls_autostart_route(sock, method, url, req, n);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/pc/readopt?", 16) == 0) {
        if (!g_ls_ready) { http_json(sock, "{\"error\":\"LAN sync state DB unavailable\"}"); return 1; }
        if (!ls_pc_active_req(sock, url)) { http_json(sock, "{\"error\":\"another PC client is the active one\"}"); return 1; }
        ls_pc_readopt(sock, url, req, n);
        return 1;
    }
    /* The PC uploaded these archives to the vault ("PC and VPS", PC route). */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/pc/vault_acks", 18) == 0) {
        if (!g_ls_ready) { http_json(sock, "{\"error\":\"LAN sync state DB unavailable\"}"); return 1; }
        if (!ls_pc_primary_req(sock, url)) { http_json(sock, "{\"error\":\"another PC client is the active one\"}"); return 1; }
        char *body = NULL;
        size_t blen = 0;
        if (ls_read_body(sock, req, n, 1024 * 1024, &body, &blen) < 0) { http_json(sock, "{\"error\":\"Missing body\"}"); return 1; }
        int64_t now = (int64_t)time(NULL);
        int marked = 0, seen = 0;
        const char *p = strstr(body, "\"ids\"");
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *st = NULL;
        if (p && g_ls_db && sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET vault_ack_at = ? WHERE id = ? AND vault_ack_at = 0",
                                                -1, &st, NULL) == SQLITE_OK) {
            for (p = strchr(p + 5, '"'); p; p = strchr(p, '"')) {
                const char *e = strchr(p + 1, '"');
                if (!e) break;
                char id[200];
                size_t l = (size_t)(e - p - 1);
                if (l > 0 && l < sizeof(id)) {
                    memcpy(id, p + 1, l);
                    id[l] = 0;
                    seen++;
                    sqlite3_reset(st);
                    sqlite3_bind_int64(st, 1, now);
                    sqlite3_bind_text(st, 2, id, -1, SQLITE_TRANSIENT);
                    if (sqlite3_step(st) == SQLITE_DONE) marked += sqlite3_changes(g_ls_db);
                }
                p = e + 1;
            }
        }
        sqlite3_finalize(st);
        pthread_mutex_unlock(&g_ls_mtx);
        free(body);
        if (marked) ls_event("VAULT_ACK_FROM_PC", NULL, "%d archive(s) reached the vault through the PC", marked);
        char j[96];
        snprintf(j, sizeof(j), "{\"ok\":true,\"ids\":%d,\"marked\":%d}", seen, marked);
        http_json(sock, j);
        return 1;
    }
    if (strncmp(url, "/api/restore/", 13) == 0) {
        if (!g_ls_ready) { http_json(sock, "{\"error\":\"LAN sync state DB unavailable\"}"); return 1; }
        if (strcmp(method, "POST") == 0 && strncmp(url, "/api/restore/from_queue", 23) == 0) ls_restore_from_queue(sock, url);
        else if (strcmp(method, "POST") == 0 && strncmp(url, "/api/restore/upload", 19) == 0) ls_restore_upload(sock, url, req, n);
        else if (strcmp(method, "GET") == 0 && strncmp(url, "/api/restore/status", 19) == 0) ls_send_restore_status(sock);
        else if (strcmp(method, "GET") == 0 && strncmp(url, "/api/restore/snapshot_preview", 29) == 0) ls_restore_preview(sock, url);
        else if (strcmp(method, "POST") == 0 && strncmp(url, "/api/restore/snapshot", 21) == 0) ls_restore_snapshot_req(sock, url);
        else http_send(sock, "404 Not Found", "application/json", LS_ERR_404, (int)strlen(LS_ERR_404));
        return 1;
    }
    /* PC client reports what it holds for one title (stage L). */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/pc_manifest", 23) == 0) {
        if (!g_ls_ready) { http_json(sock, "{\"error\":\"LAN sync state DB unavailable\"}"); return 1; }
        if (!ls_pc_primary_req(sock, url)) { http_json(sock, "{\"ok\":true,\"stored\":0,\"standby\":true}"); return 1; }
        char tid[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        char *body = NULL;
        size_t blen = 0;
        if (ls_read_body(sock, req, n, 4 * 1024 * 1024, &body, &blen) < 0) {
            http_json(sock, "{\"error\":\"Missing, incomplete or too large body\"}");
            return 1;
        }
        int foreign = 0, repaired = 0;
        int stored = ls_pc_manifest_store(tid, body, &foreign, &repaired);
        free(body);
        if (stored < 0) { http_json(sock, "{\"error\":\"Bad manifest\"}"); return 1; }
        ls_event("PC_MANIFEST_RECEIVED", tid, "%d archive(s), %d not from this PS5, %d PC ack(s) recovered",
                 stored, foreign, repaired);
        char j[160];
        snprintf(j, sizeof(j), "{\"ok\":true,\"stored\":%d,\"foreign\":%d,\"acks_recovered\":%d}", stored, foreign, repaired);
        http_json(sock, j);
        return 1;
    }
    if (strcmp(url, "/api/settings/sync") == 0) {
        if (!g_ls_ready) { http_json(sock, "{\"error\":\"LAN sync state DB unavailable\"}"); return 1; }
        if (strcmp(method, "POST") == 0) ls_handle_sync_settings_post(sock, req, n);
        else ls_send_sync_settings(sock);
        return 1;
    }
    return 0;
}

static int ls_http_route(int sock, const char *method, const char *url) {
    if (strncmp(url, "/api/reconcile/", 15) == 0) return ls_reconcile_route(sock, method, url);
    if ((strncmp(url, "/api/pc/", 8) == 0 || strncmp(url, "/api/backup/wait", 16) == 0 ||
         strcmp(url, "/api/backup/kick") == 0) && ls_pclink_route(sock, method, url))
        return 1;
    if (strncmp(url, "/api/system/launcher", 20) == 0) return ls_launcher_route(sock, method, url);
    if (strncmp(url, "/api/backup/", 12) != 0 && strncmp(url, "/api/recovery", 13) != 0 &&
        strncmp(url, "/api/system/probe", 17) != 0) return 0;
    if (!g_ls_ready) {
        http_json(sock, "{\"error\":\"LAN sync state DB unavailable\",\"ready\":false}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/backup/status") == 0) {
        ls_send_status(sock);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && (strcmp(url, "/api/backup/queue") == 0 ||
                                       strncmp(url, "/api/backup/queue?", 18) == 0)) {
        /* The PC client polls with pending=1: the primary gets what it lacks, a
           stand-in what neither it nor the primary has, everyone else nothing. */
        char cid[40] = "";
        int role = strstr(url, "pending=1") ? ls_pc_role(sock, url, cid, sizeof(cid)) : 1;
        if (!role) { http_json(sock, "{\"queue\":[]}"); return 1; }
        ls_send_queue(sock, strstr(url, "pending=1") != NULL, 0, role == 2 ? cid : NULL);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/queue_download", 26) == 0) {
        char id[256] = {0};
        char *p = strstr(url, "id=");
        if (p) url_param_decode(p + 3, id, sizeof(id));
        if (!safe_queue_id(id)) { http_json(sock, "{\"error\":\"Invalid id\"}"); return 1; }
        ls_pc_active_req(sock, url);
        if (!ls_queue_stream(sock, id, 1))
            http_send(sock, "404 Not Found", "application/json", LS_ERR_NOTFOUND, (int)strlen(LS_ERR_NOTFOUND));
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/queue_ack", 21) == 0) {
        if (strstr(url, "dest=pc")) {
            char cid[40] = "";
            int role = ls_pc_role(sock, url, cid, sizeof(cid));
            if (!role) {
                http_json(sock, "{\"error\":\"another PC client is the active one\"}");   /* its copy must not let the PS5 forget */
                return 1;
            }
            if (role == 2) {                       /* stand-in: noted, but the PS5 keeps it for the primary */
                char id[256] = "", sha[80] = "";
                const char *p;
                if ((p = strstr(url, "id="))) url_param_decode(p + 3, id, sizeof(id));
                if ((p = strstr(url, "sha256="))) url_param_decode(p + 7, sha, sizeof(sha));
                int rc = safe_queue_id(id) ? ls_queue_standin_ack(id, sha, cid) : -1;
                http_json(sock, rc == 0 ? "{\"ok\":true,\"standin\":true}" : rc == -2 ? "{\"error\":\"sha256 mismatch\"}"
                                        : "{\"error\":\"Queue item not found\"}");
                return 1;
            }
        }
        ls_handle_ack(sock, url);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/history", 19) == 0) {
        char tid[32] = {0}, save[256] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if ((p = strstr(url, "save="))) url_param_decode(p + 5, save, sizeof(save));
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        ls_send_history(sock, tid, save);
        return 1;
    }
    /* Compare With PC (cheap: metadata + the PC's last report). */
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/compare", 19) == 0) {
        char tid[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        char *j = ls_compare_title_json(tid);
        if (j) { http_json(sock, j); free(j); } else http_json(sock, "{\"error\":\"Out of memory\"}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strcmp(url, "/api/backup/titles") == 0) {
        ls_send_titles(sock);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/diag/title", 22) == 0) {
        char tid[32] = {0};
        char *p = strstr(url, "id=");
        if (p) url_param_decode(p + 3, tid, sizeof(tid));
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        p = strstr(url, "recent=");
        ls_send_diag_title(sock, tid, p ? atoi(p + 7) : 30);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/saves", 17) == 0) {
        char tid[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if (tid[0] && !ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        ls_send_saves(sock, tid);
        return 1;
    }
    /* "Scan Changed Saves Now": the idle sweep on demand (all titles, metadata
     * first; protect jobs only for changed/new saves). */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/scan_changed", 24) == 0) {
        if (!g_ls_worker_started) { http_json(sock, "{\"error\":\"Backup worker not running\"}"); return 1; }
        int r = ls_worker_enqueue(LS_JOB_IDLE_SWEEP, "manual", 0, 0);
        http_json(sock, r > 0 ? "{\"ok\":true,\"queued\":true}" :
                        r == 0 ? "{\"ok\":true,\"queued\":false,\"message\":\"Already pending\"}" :
                                 "{\"error\":\"Out of memory\"}");
        return 1;
    }
    /* Manual "Back Up Changed Saves" for one title (stability check + changed saves only). */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/protect_title", 25) == 0) {
        char tid[32] = {0};
        char *p = strstr(url, "id=");
        if (p) url_param_decode(p + 3, tid, sizeof(tid));
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        if (!g_ls_worker_started) { http_json(sock, "{\"error\":\"Backup worker not running\"}"); return 1; }
        if (ls_title_is_running(tid)) {
            http_json(sock, "{\"error\":\"Title is running; it will be backed up when it closes\"}");
            return 1;
        }
        int r = ls_worker_enqueue(LS_JOB_TITLE_MANUAL, tid, 0, 0);
        http_json(sock, r > 0 ? "{\"ok\":true,\"queued\":true}" :
                        r == 0 ? "{\"ok\":true,\"queued\":false,\"message\":\"Already pending\"}" :
                                 "{\"error\":\"Out of memory\"}");
        return 1;
    }
    /* System-state probe (stage O). */
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/system/probe", 17) == 0) {
        size_t cap = 4096, pos = 0;
        char *buf = malloc(cap);
        if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return 1; }
        buf[0] = 0;
        ls_sys_json(&buf, &cap, &pos);
        http_send(sock, "200 OK", "application/json", buf, (int)pos);
        free(buf);
        return 1;
    }
    /* Regression recovery offers (stage N). */
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/recovery", 13) == 0 && strncmp(url, "/api/recovery/", 14) != 0) {
        char tid[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if (tid[0] && !ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        size_t cap = 8192, pos = 0;
        char *buf = malloc(cap);
        if (!buf) { http_json(sock, "{\"error\":\"Out of memory\"}"); return 1; }
        buf[0] = 0;
        ls_recovery_json(tid, &buf, &cap, &pos);
        http_send(sock, "200 OK", "application/json", buf, (int)pos);
        free(buf);
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/recovery/resolve", 21) == 0) {
        char ids[32] = {0}, action[16] = {0}, err[300] = {0};
        char *p;
        if ((p = strstr(url, "id="))) url_param_decode(p + 3, ids, sizeof(ids));
        if ((p = strstr(url, "action="))) url_param_decode(p + 7, action, sizeof(action));
        int rc = ls_recovery_resolve(atoll(ids), action, err, sizeof(err));
        ls_restore_reply(sock, rc, err);
        return 1;
    }
    /* Game cover for the Browse page: the console's own app icon, cacheable. */
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/game_icon?", 22) == 0) {
        char tid[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        int ok = strlen(tid) == 9;
        for (int i = 0; ok && tid[i]; i++) ok = isalnum((unsigned char)tid[i]);
        static const char *where[] = { "/user/appmeta/%s/icon0.png", "/user/app/%s/sce_sys/icon0.png",
                                       "/system_ex/app/%s/sce_sys/icon0.png" };
        for (int w = 0; ok && w < 3; w++) {
            char path[128];
            snprintf(path, sizeof(path), where[w], tid);
            struct stat st;
            if (stat(path, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 4 * 1024 * 1024) continue;
            FILE *f = fopen(path, "rb");
            if (!f) continue;
            char *data = malloc((size_t)st.st_size);
            size_t n = data ? fread(data, 1, (size_t)st.st_size, f) : 0;
            fclose(f);
            if (n != (size_t)st.st_size) { free(data); continue; }
            char hdr[256];
            int hl = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: %zu\r\n"
                              "Cache-Control: max-age=86400\r\nConnection: close\r\n\r\n", n);
            send(sock, hdr, hl, 0);
            size_t off = 0;
            while (off < n) { ssize_t s2 = send(sock, data + off, n - off, 0); if (s2 <= 0) break; off += (size_t)s2; }
            free(data);
            return 1;
        }
        http_send(sock, "404 Not Found", "application/json", LS_ERR_404, (int)strlen(LS_ERR_404));
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strcmp(url, "/api/backup/housekeeping") == 0) {
        ls_config_t cfg;
        ls_load_config(&cfg);
        if (!cfg.storage_ps5) { http_json(sock, "{\"error\":\"Only in PS5-only mode (storage_mode = ps5)\"}"); return 1; }
        int r = ls_worker_enqueue(LS_JOB_HOUSEKEEP, "", 200, 0);
        http_json(sock, r >= 0 ? "{\"ok\":true,\"queued\":true}" : "{\"error\":\"Cannot queue the job\"}");
        return 1;
    }
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/label?", 18) == 0) {
        char tid[32] = {0}, obj[256] = {0}, lbl[200] = {0};
        char *p;
        if ((p = strstr(url, "title="))) url_param_decode(p + 6, tid, sizeof(tid));
        if ((p = strstr(url, "object="))) url_param_decode(p + 7, obj, sizeof(obj));
        if ((p = strstr(url, "label="))) url_param_decode(p + 6, lbl, sizeof(lbl));
        for (char *c = lbl; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
        if (!tid[0] || !obj[0] || strchr(obj, '/') || strstr(obj, "..")) { http_json(sock, "{\"error\":\"Invalid save\"}"); return 1; }
        if (ls_label_set(tid, obj, lbl) < 0) { http_json(sock, "{\"error\":\"Cannot store the name\"}"); return 1; }
        http_json(sock, "{\"ok\":true}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/objects", 19) == 0) {
        char tid[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if (!ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        ls_send_objects(sock, tid);
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/snapshots", 21) == 0) {
        char tid[32] = {0}, since[32] = {0};
        char *p = strstr(url, "title=");
        if (p) url_param_decode(p + 6, tid, sizeof(tid));
        if ((p = strstr(url, "since="))) url_param_decode(p + 6, since, sizeof(since));
        if (tid[0] && !ls_valid_title_id(tid)) { http_json(sock, "{\"error\":\"Invalid title\"}"); return 1; }
        if (!tid[0]) {                       /* the PC's feed: only when the PC is a destination, for the active client */
            ls_config_t cfg;
            ls_load_config(&cfg);
            if (!cfg.storage_pc || !ls_pc_active_req(sock, url)) { http_json(sock, "{\"snapshots\":[]}"); return 1; }
        }
        ls_send_snapshots(sock, tid, atoll(since));
        return 1;
    }
    /* "Delete this save": both files of one save object, backed up first (restore pipeline). */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/delete_object", 25) == 0) {
        char tid[32] = {0}, obj[256] = {0}, user[16] = {0}, err[300] = {0};
        char *p = strstr(url, "id=");
        if (p) url_param_decode(p + 3, tid, sizeof(tid));
        if ((p = strstr(url, "object="))) url_param_decode(p + 7, obj, sizeof(obj));
        if ((p = strstr(url, "user="))) url_param_decode(p + 5, user, sizeof(user));
        if (!ls_valid_title_id(tid) || !obj[0] || strchr(obj, '/') || strstr(obj, "..")) {
            http_json(sock, "{\"error\":\"Invalid request\"}");
            return 1;
        }
        int rc = ls_restore_request_delete(user, tid, obj, err, sizeof(err));
        ls_restore_reply(sock, rc, err);
        return 1;
    }
    /* "Back up this save": one save object (primary + backup copy) as a unit. */
    if (strcmp(method, "POST") == 0 && strncmp(url, "/api/backup/protect_object", 26) == 0) {
        char tid[32] = {0}, obj[256] = {0};
        char *p = strstr(url, "id=");
        if (p) url_param_decode(p + 3, tid, sizeof(tid));
        if ((p = strstr(url, "object="))) url_param_decode(p + 7, obj, sizeof(obj));
        if (!ls_valid_title_id(tid) || !obj[0] || strchr(obj, '/') || strstr(obj, "..")) {
            http_json(sock, "{\"error\":\"Invalid request\"}");
            return 1;
        }
        if (!g_ls_worker_started) { http_json(sock, "{\"error\":\"Backup worker not running\"}"); return 1; }
        if (ls_title_is_running(tid)) { http_json(sock, "{\"error\":\"Title is running; close it first\"}"); return 1; }
        int r = ls_worker_enqueue_job(LS_JOB_OBJECT, tid, obj, 0, 0, 0);
        http_json(sock, r > 0 ? "{\"ok\":true,\"queued\":true}" :
                        r == 0 ? "{\"ok\":true,\"queued\":false,\"message\":\"Already pending\"}" :
                                 "{\"error\":\"Out of memory\"}");
        return 1;
    }
    if (strcmp(method, "GET") == 0 && strncmp(url, "/api/backup/events", 18) == 0) {
        char *p = strstr(url, "limit=");
        ls_send_events(sock, p ? atoi(p + 6) : 200);
        return 1;
    }
    http_send(sock, "404 Not Found", "application/json", LS_ERR_404, (int)strlen(LS_ERR_404));
    return 1;
}
