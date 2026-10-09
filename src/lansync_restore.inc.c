/*
 * LAN save sync — conservative archive -> PS5 restore (stage M), for one
 * file (legacy archives), one save object (primary + sce_bu copy) or a
 * whole game snapshot. Included from main.c after lansync_reconcile.inc.c;
 * uses upstream's mount_by_path / unmount_save / parse_sfo /
 * get_account_id_for_user.
 *
 * A restore only ever starts from an explicit request and runs in the
 * serialized backup worker while holding the reconcile job slot:
 *
 *   1. stage   extract every file (+ PS4 key) from its archive into
 *              /data/shallot/restore/<id>/<n>/ as .part, verify SHA-256
 *   2. check   per file: mount a scratch COPY via upstream's mount_by_path,
 *              read param.sfo: title, save dir and account must match
 *              (account-0 archives are re-signed on a copy)
 *              -> any failure here: nothing on the PS5 was touched
 *   3. protect back up the whole game first (normal protect path, reason
 *              pre_restore) = a game snapshot to go back to; byte copies
 *              of the files about to be replaced for a fast undo
 *   4. swap    per file: copy staged -> <target>.restore.tmp (fsync, hash),
 *              rename over the target (PS4 key first); title not running
 *   5. verify  hash every placed file
 *              -> a failure in 4/5 undoes EVERY file placed so far (previous
 *              bytes put back; files that did not exist are removed again);
 *              the result is "failed", or "partial" if an undo failed
 *   6. record  savedata.db (primaries only), protection bookkeeping and a
 *              new game snapshot, events
 *
 * Deletion policy: by default a restore never deletes a save. Save objects
 * on the PS5 that are not in the restored snapshot are left exactly as they
 * are; a backup copy without its primary in the snapshot is not written over
 * a primary that exists on the PS5. The only removal is the undo of a file
 * this restore itself created.
 *
 * Exact game restore (explicit opt-in): files of the game that are not in
 * the snapshot are removed as well. The request must carry the fingerprint
 * of the removal list the user confirmed (GET /api/restore/snapshot_preview);
 * if the saves on the PS5 changed since, it is refused. Every file to be
 * removed must be archived by the pre-restore backup first, a byte copy is
 * kept, removals happen after all placements, and a failure undoes them too.
 */

#define LS_RESTORE_DIR LS_BASE_DIR "/restore"

/* ── Reading one entry of an uncompressed ZIP (Shallot archives) ──── */
static uint32_t ls_le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t ls_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* Finds `name` via the central directory. Returns 0 and data offset/size. */
static int ls_zip_find(int fd, const char *name, int64_t *data_off, int64_t *size, char *err, size_t err_sz) {
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size < 22) { snprintf(err, err_sz, "archive too small"); return -1; }
    int64_t fsz = st.st_size;
    int64_t tail = fsz < 65557 ? fsz : 65557;
    uint8_t *buf = malloc((size_t)tail);
    if (!buf) { snprintf(err, err_sz, "out of memory"); return -1; }
    if (pread(fd, buf, (size_t)tail, fsz - tail) != tail) { free(buf); snprintf(err, err_sz, "read error"); return -1; }
    int64_t eocd = -1;
    for (int64_t i = tail - 22; i >= 0; i--)
        if (ls_le32(buf + i) == 0x06054b50) { eocd = i; break; }
    if (eocd < 0) { free(buf); snprintf(err, err_sz, "not a ZIP archive"); return -1; }
    uint32_t cd_size = ls_le32(buf + eocd + 12), cd_off = ls_le32(buf + eocd + 16);
    uint16_t count = ls_le16(buf + eocd + 10);
    free(buf);
    if ((int64_t)cd_off + cd_size > fsz || cd_size > 16 * 1024 * 1024) { snprintf(err, err_sz, "bad ZIP directory"); return -1; }
    uint8_t *cd = malloc(cd_size ? cd_size : 1);
    if (!cd) { snprintf(err, err_sz, "out of memory"); return -1; }
    if (pread(fd, cd, cd_size, cd_off) != (ssize_t)cd_size) { free(cd); snprintf(err, err_sz, "read error"); return -1; }
    size_t p = 0, nlen_want = strlen(name);
    int rc = -1;
    snprintf(err, err_sz, "entry %s not in archive", name);
    for (uint16_t i = 0; i < count && p + 46 <= cd_size; i++) {
        if (ls_le32(cd + p) != 0x02014b50) break;
        uint16_t method = ls_le16(cd + p + 10);
        uint32_t csize = ls_le32(cd + p + 20), usize = ls_le32(cd + p + 24);
        uint16_t nl = ls_le16(cd + p + 28), xl = ls_le16(cd + p + 30), cl = ls_le16(cd + p + 32);
        uint32_t lho = ls_le32(cd + p + 42);
        if (p + 46 + nl > cd_size) break;
        if (nl == nlen_want && memcmp(cd + p + 46, name, nl) == 0) {
            if (method != 0 || csize != usize) { snprintf(err, err_sz, "entry %s is compressed (not supported)", name); break; }
            uint8_t lh[30];
            if (pread(fd, lh, 30, lho) != 30 || ls_le32(lh) != 0x04034b50) { snprintf(err, err_sz, "bad local header"); break; }
            int64_t off = (int64_t)lho + 30 + ls_le16(lh + 26) + ls_le16(lh + 28);
            if (off + usize > fsz) { snprintf(err, err_sz, "entry %s truncated", name); break; }
            *data_off = off;
            *size = usize;
            rc = 0;
            err[0] = 0;
            break;
        }
        p += 46 + (size_t)nl + xl + cl;
    }
    free(cd);
    return rc;
}

/* Extracts `name` to dst (.part -> fsync -> rename), returning its SHA-256. */
static int ls_zip_extract(const char *zip_path, const char *name, const char *dst, char sha[65],
                          int64_t *size_out, char *iobuf, char *err, size_t err_sz) {
    int fd = open(zip_path, O_RDONLY);
    if (fd < 0) { snprintf(err, err_sz, "cannot open archive"); return -1; }
    int64_t off = 0, size = 0;
    if (ls_zip_find(fd, name, &off, &size, err, err_sz) < 0) { close(fd); return -1; }
    char part[MAX_PATH_LEN];
    snprintf(part, sizeof(part), "%s.part", dst);
    int out = open(part, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (out < 0) { close(fd); snprintf(err, err_sz, "cannot create %s", part); return -1; }
    ls_sha256_t c;
    ls_sha256_init(&c);
    int64_t left = size;
    int rc = 0;
    while (left > 0) {
        size_t want = left < BUF_SIZE ? (size_t)left : BUF_SIZE;
        ssize_t r = pread(fd, iobuf, want, off + (size - left));
        if (r <= 0) { rc = -1; break; }
        if (ls_write_all(out, iobuf, (size_t)r) < 0) { rc = -1; break; }
        ls_sha256_update(&c, iobuf, (size_t)r);
        left -= r;
    }
    close(fd);
    if (rc == 0 && fsync(out) < 0) rc = -1;
    if (close(out) < 0) rc = -1;
    if (rc < 0 || rename(part, dst) < 0) { unlink(part); snprintf(err, err_sz, "extract of %s failed", name); return -1; }
    ls_sha256_final_hex(&c, sha);
    if (size_out) *size_out = size;
    return 0;
}

/* ── Restore request state (one at a time) ───────────────────────── */
#define LS_RESTORE_MAX_ITEMS 64

typedef struct {
    char save_name[256];
    char object_id[256];
    int role;
    int remove;                /* exact restore: remove this file (not in the snapshot) */
    char archive_path[MAX_PATH_LEN];
    char expected_sha[65];     /* container SHA-256 if known ("" = take from archive) */
    int64_t gen_id;
    char result[16];           /* pending | skipped | unchanged | restored | removed | failed | undone | undo_failed */
    char note[160];
} ls_restore_item_t;

typedef struct {
    int active;
    char id[40];
    char state[16];            /* queued | running | done | failed | partial */
    char step[32];
    char source[16];           /* queue | upload | snapshot */
    char kind[16];             /* file | object | title | delete */
    char user_id[16], title_id[32];
    char save_name[256];       /* file restores; first file otherwise */
    char object_id[256];       /* object restores */
    int64_t snap_id;           /* snapshot being restored (0 = single archive) */
    int64_t gen_id;            /* file restores: generation being restored */
    int64_t rollback_gen;      /* file restores: previous generation */
    int64_t rollback_snap;     /* game snapshot taken right before the restore */
    int kept_objects;          /* save objects on the PS5 not in the snapshot (left alone) */
    int exact;                 /* exact game restore: files not in the snapshot are removed */
    int simulate;              /* test aid: book the result as an unexplained change (stage N test) */
    int removed_objects;
    int64_t started_at, finished_at;
    char message[300];
    int nitems;
    ls_restore_item_t items[LS_RESTORE_MAX_ITEMS];
} ls_restore_t;

static pthread_mutex_t g_ls_restore_mtx = PTHREAD_MUTEX_INITIALIZER;
static ls_restore_t g_ls_restore = {0};
/* Test aid (stage N): set only inside ls_restore_request_simulated_regression. */
static pthread_mutex_t g_ls_simulate_mtx = PTHREAD_MUTEX_INITIALIZER;
static int g_ls_next_simulate = 0;

static void ls_restore_step(const char *step) {
    pthread_mutex_lock(&g_ls_restore_mtx);
    snprintf(g_ls_restore.step, sizeof(g_ls_restore.step), "%s", step);
    pthread_mutex_unlock(&g_ls_restore_mtx);
    ls_rjob_progress(0, 1, step);
}

static void ls_restore_item_set(int i, const char *result, const char *fmt, ...) {
    char note[160] = {0};
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(note, sizeof(note), fmt, ap);
        va_end(ap);
    }
    pthread_mutex_lock(&g_ls_restore_mtx);
    if (i >= 0 && i < g_ls_restore.nitems) {
        snprintf(g_ls_restore.items[i].result, sizeof(g_ls_restore.items[i].result), "%s", result);
        if (fmt) snprintf(g_ls_restore.items[i].note, sizeof(g_ls_restore.items[i].note), "%s", note);
    }
    pthread_mutex_unlock(&g_ls_restore_mtx);
}

static void ls_restore_end(const char *state, const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&g_ls_restore_mtx);
    g_ls_restore.active = 0;
    g_ls_restore.finished_at = (int64_t)time(NULL);
    snprintf(g_ls_restore.state, sizeof(g_ls_restore.state), "%s", state);
    snprintf(g_ls_restore.message, sizeof(g_ls_restore.message), "%s", msg);
    pthread_mutex_unlock(&g_ls_restore_mtx);
    ls_rjob_finish(strcmp(state, "done") == 0 ? "completed" : "failed", msg);
}

static int ls_valid_save_name(const char *s) {
    if (strncmp(s, "sdimg_", 6) != 0 || strlen(s) > 200) return 0;
    for (const char *p = s; *p; p++) if (*p == '/' || *p == '\\' || (unsigned char)*p < 0x20) return 0;
    return strstr(s, "..") == NULL;
}

/* Common refusals + the job slot. 0 = slot claimed. */
static int ls_restore_claim(const char *user_id, const char *title_id, char *err, size_t err_sz) {
    if (!g_ls_worker_started) { snprintf(err, err_sz, "backup worker not running"); return -1; }
    if (!user_id[0] || !title_id[0]) { snprintf(err, err_sz, "invalid target"); return -1; }
    if (ls_power_down()) { snprintf(err, err_sz, "the console is powering off; restore after it starts again"); return -1; }
    if (ls_title_is_running(title_id)) { snprintf(err, err_sz, "%s is running; close it before restoring", title_id); return -2; }
    /* Validation needs upstream's single mount point; refuse up front instead of mid-way. */
    if (g_mounted) { snprintf(err, err_sz, "a save is mounted in the browser; unmount it first"); return -6; }
    pthread_mutex_lock(&g_ls_restore_mtx);
    if (g_ls_restore.active) { pthread_mutex_unlock(&g_ls_restore_mtx); snprintf(err, err_sz, "another restore is in progress"); return -3; }
    pthread_mutex_unlock(&g_ls_restore_mtx);
    /* The job slot keeps catalogue/full reconcile from reading saves meanwhile. */
    if (ls_rjob_claim("restore", title_id, 0) < 0) { snprintf(err, err_sz, "a reconciliation job is running; try again after it"); return -4; }
    return 0;
}

/* Fills the common fields of g_ls_restore (mutex held by the caller). */
static void ls_restore_init_locked(const char *source, const char *kind, const char *user_id, const char *title_id) {
    memset(&g_ls_restore, 0, sizeof(g_ls_restore));
    g_ls_restore.active = 1;
    char stamp[32];
    cloud_timestamp(stamp, sizeof(stamp));
    snprintf(g_ls_restore.id, sizeof(g_ls_restore.id), "r%s_%d", stamp, (int)(time(NULL) % 100000));
    snprintf(g_ls_restore.state, sizeof(g_ls_restore.state), "queued");
    snprintf(g_ls_restore.source, sizeof(g_ls_restore.source), "%s", source);
    snprintf(g_ls_restore.kind, sizeof(g_ls_restore.kind), "%s", kind);
    snprintf(g_ls_restore.user_id, sizeof(g_ls_restore.user_id), "%s", user_id);
    snprintf(g_ls_restore.title_id, sizeof(g_ls_restore.title_id), "%s", title_id);
    g_ls_restore.started_at = (int64_t)time(NULL);
    g_ls_restore.simulate = g_ls_next_simulate;
}

static void ls_restore_add_item_locked(const char *save_name, const char *archive_path, const char *expected_sha, int64_t gen_id) {
    if (g_ls_restore.nitems >= LS_RESTORE_MAX_ITEMS) return;
    ls_restore_item_t *it = &g_ls_restore.items[g_ls_restore.nitems++];
    memset(it, 0, sizeof(*it));
    snprintf(it->save_name, sizeof(it->save_name), "%s", save_name);
    it->role = ls_object_of(save_name, it->object_id, sizeof(it->object_id));
    snprintf(it->archive_path, sizeof(it->archive_path), "%s", archive_path);
    snprintf(it->expected_sha, sizeof(it->expected_sha), "%s", expected_sha ? expected_sha : "");
    it->gen_id = gen_id;
    snprintf(it->result, sizeof(it->result), "pending");
}

static int ls_restore_enqueue(const char *title_id, const char *what, const char *user_id, const char *source,
                              char *err, size_t err_sz) {
    if (ls_worker_enqueue(LS_JOB_RESTORE, title_id, 0, 0) < 0) {
        ls_restore_end("failed", "could not queue the restore");
        snprintf(err, err_sz, "could not queue the restore");
        return -5;
    }
    ls_event("RESTORE_REQUESTED", title_id, "%s user=%s source=%s", what, user_id, source);
    return 0;
}

/*
 * Single-file restore from one archive (HTTP thread). Returns 0 queued,
 * otherwise a negative code with a message in err.
 */
static int ls_restore_request(const char *source, const char *user_id, const char *title_id,
                              const char *save_name, const char *archive_path, const char *expected_sha,
                              int64_t gen_id, char *err, size_t err_sz) {
    if (!ls_valid_save_name(save_name)) { snprintf(err, err_sz, "invalid target"); return -1; }
    int rc = ls_restore_claim(user_id, title_id, err, err_sz);
    if (rc < 0) return rc;
    pthread_mutex_lock(&g_ls_restore_mtx);
    ls_restore_init_locked(source, "file", user_id, title_id);
    snprintf(g_ls_restore.save_name, sizeof(g_ls_restore.save_name), "%s", save_name);
    g_ls_restore.gen_id = gen_id;
    ls_restore_add_item_locked(save_name, archive_path, expected_sha, gen_id);
    pthread_mutex_unlock(&g_ls_restore_mtx);
    char what[300];
    snprintf(what, sizeof(what), "%s gen=%lld", save_name, (long long)gen_id);
    return ls_restore_enqueue(title_id, what, user_id, source, err, err_sz);
}

/* Snapshot header. Returns 1 if found. */
static int ls_snapshot_head(int64_t snap_id, char *user, char *title, char *kind, char *obj) {
    int found = 0;
    pthread_mutex_lock(&g_ls_mtx);
    if (g_ls_db && g_ls_schema >= 6) {
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT user_id, title_id, kind, object_id FROM snapshots WHERE snap_id = ?",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)snap_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                found = 1;
                ls_col_text(st, 0, user, 16);
                ls_col_text(st, 1, title, 32);
                ls_col_text(st, 2, kind, 16);
                ls_col_text(st, 3, obj, 256);
            }
        }
        sqlite3_finalize(st);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    return found;
}

/*
 * Files of the game on the PS5 now (snapshot's user) that the snapshot does
 * not contain: what an exact restore would remove. `l_out` gets them in
 * enumeration order; fp = SHA-256 over their names (binds a confirmation to
 * exactly this list). Returns the count.
 */
static int ls_restore_removals(const char *user, const char *title, const ls_snap_file_t *files, int n,
                               ls_save_list_t *l_out, char fp[65]) {
    memset(l_out, 0, sizeof(*l_out));
    ls_save_list_t l = {0};
    ls_enum_saves(&l, title);
    ls_sha256_t c;
    ls_sha256_init(&c);
    for (int i = 0; i < l.count; i++) {
        if (strcmp(l.items[i].user_id, user) != 0 || !ls_valid_save_name(l.items[i].save_name)) continue;
        int in = 0;
        for (int k = 0; k < n && !in; k++) in = strcmp(files[k].save_name, l.items[i].save_name) == 0;
        if (in) continue;
        ls_save_ref_t *r = ls_save_list_push(l_out);
        if (r) *r = l.items[i];
        ls_sha256_update(&c, l.items[i].save_name, strlen(l.items[i].save_name));
        ls_sha256_update(&c, "\n", 1);
    }
    ls_sha256_final_hex(&c, fp);
    ls_save_list_free(&l);
    return l_out->count;
}

/* GET /api/restore/snapshot_preview?id= : what a whole-game restore of this
 * snapshot would restore, and what an exact restore would also remove. */
static int ls_restore_preview_json(int64_t snap_id, char **buf, size_t *cap, size_t *pos, char *err, size_t err_sz) {
    char user[16] = {0}, title[32] = {0}, kind[16] = {0}, sobj[256] = {0};
    if (!ls_snapshot_head(snap_id, user, title, kind, sobj)) { snprintf(err, err_sz, "no snapshot #%lld", (long long)snap_id); return -1; }
    ls_snap_file_t *files = NULL;
    int n = ls_snapshot_files(snap_id, &files);
    ls_save_list_t rm;
    char fp[65];
    ls_restore_removals(user, title, files, n, &rm, fp);
    ls_json_append(buf, cap, pos, "{\"snapshot_id\":%lld,\"kind\":\"%s\",\"title_id\":\"%s\",\"user_id\":\"%s\",\"restore_objects\":[",
                   (long long)snap_id, kind, title, user);
    for (int i = 0, first = 1; i < n; i++) {
        if (i && strcmp(files[i].object_id, files[i - 1].object_id) == 0) continue;
        char e[520];
        json_escape(files[i].object_id, e, sizeof(e));
        ls_json_append(buf, cap, pos, "%s\"%s\"", first ? "" : ",", e);
        first = 0;
    }
    ls_json_append(buf, cap, pos, "],\"remove\":[");
    ls_object_list_t o = {0};
    ls_group_objects(&rm, &o);
    for (int k = 0; k < o.count; k++) {
        char e[520];
        json_escape(o.items[k].id, e, sizeof(e));
        ls_json_append(buf, cap, pos, "%s{\"object_id\":\"%s\",\"files\":[", k ? "," : "", e);
        for (int role = 0, f = 1; role < 2; role++) {
            int i = o.items[k].member[role];
            if (i < 0) continue;
            json_escape(rm.items[i].save_name, e, sizeof(e));
            ls_json_append(buf, cap, pos, "%s\"%s\"", f ? "" : ",", e);
            f = 0;
        }
        ls_json_append(buf, cap, pos, "]}");
    }
    ls_json_append(buf, cap, pos, "],\"remove_files\":%d,\"remove_fp\":\"%s\"}", rm.count, fp);
    ls_object_list_free(&o);
    ls_save_list_free(&rm);
    free(files);
    return 0;
}

/*
 * Restore of a snapshot: the whole game (object_id NULL/"") or one save
 * object of it. Every file's archive must still be on the PS5. exact=1
 * (whole game only) also removes the game's files that are not in the
 * snapshot; expect_fp must equal the preview's remove_fp.
 */
static int ls_restore_request_snapshot_ex(int64_t snap_id, const char *object_id, int exact, const char *expect_fp,
                                          char *err, size_t err_sz);
/*
 * Delete one save object (its save file and PS5 backup copy) of one user,
 * through the restore pipeline: the whole game is backed up first, each
 * file must be archived before it is removed, and a failure undoes it.
 */
static int ls_restore_request_delete(const char *user_id, const char *title_id, const char *object_id,
                                     char *err, size_t err_sz) {
    ls_save_list_t l = {0}, mine = {0};
    ls_enum_saves(&l, title_id);
    char users[4][16];
    int nusers = 0;
    for (int i = 0; i < l.count; i++) {
        char id[256];
        ls_object_of(l.items[i].save_name, id, sizeof(id));
        if (strcmp(id, object_id) != 0 || !ls_valid_save_name(l.items[i].save_name)) continue;
        if (user_id && user_id[0] && strcmp(user_id, l.items[i].user_id) != 0) continue;
        int seen = 0;
        for (int k = 0; k < nusers; k++) seen |= strcmp(users[k], l.items[i].user_id) == 0;
        if (!seen && nusers < 4) snprintf(users[nusers++], 16, "%s", l.items[i].user_id);
        ls_save_ref_t *r = ls_save_list_push(&mine);
        if (r) *r = l.items[i];
    }
    ls_save_list_free(&l);
    if (!mine.count) { ls_save_list_free(&mine); snprintf(err, err_sz, "save %s not found", object_id); return -1; }
    if (nusers > 1) { ls_save_list_free(&mine); snprintf(err, err_sz, "several users have save %s; pass user=", object_id); return -1; }
    int rc = ls_restore_claim(users[0], title_id, err, err_sz);
    if (rc < 0) { ls_save_list_free(&mine); return rc; }
    pthread_mutex_lock(&g_ls_restore_mtx);
    ls_restore_init_locked("delete", "delete", users[0], title_id);
    snprintf(g_ls_restore.object_id, sizeof(g_ls_restore.object_id), "%s", object_id);
    snprintf(g_ls_restore.save_name, sizeof(g_ls_restore.save_name), "%s", mine.items[0].save_name);
    for (int i = 0; i < mine.count; i++) {
        ls_restore_add_item_locked(mine.items[i].save_name, "", NULL, 0);
        g_ls_restore.items[g_ls_restore.nitems - 1].remove = 1;
    }
    pthread_mutex_unlock(&g_ls_restore_mtx);
    char what[300];
    snprintf(what, sizeof(what), "delete save %s (%d file(s))", object_id, mine.count);
    ls_save_list_free(&mine);
    return ls_restore_enqueue(title_id, what, users[0], "delete", err, err_sz);
}

static int ls_restore_request_snapshot(int64_t snap_id, const char *object_id, char *err, size_t err_sz) {
    return ls_restore_request_snapshot_ex(snap_id, object_id, 0, NULL, err, err_sz);
}

/* Test aid for stage N: like an object restore, but the result is booked as
 * an unexplained change so the regression check treats it like a crash
 * that left the older version on disk. Not offered in the UI. */
static int ls_restore_request_simulated_regression(int64_t snap_id, const char *object_id, char *err, size_t err_sz) {
    if (!object_id || !object_id[0]) { snprintf(err, err_sz, "simulate_regression needs object="); return -1; }
    /* The flag is copied into the request while it is created, i.e. before the
     * job is queued, so the worker can never run it without the flag. */
    pthread_mutex_lock(&g_ls_simulate_mtx);
    g_ls_next_simulate = 1;
    int rc = ls_restore_request_snapshot_ex(snap_id, object_id, 0, NULL, err, err_sz);
    g_ls_next_simulate = 0;
    pthread_mutex_unlock(&g_ls_simulate_mtx);
    if (rc == 0)
        ls_event("REGRESSION_SIMULATION", NULL, "save %s: snapshot #%lld will be placed as an unexplained change",
                 object_id, (long long)snap_id);
    return rc;
}
static int ls_restore_request_snapshot_ex(int64_t snap_id, const char *object_id, int exact, const char *expect_fp,
                                          char *err, size_t err_sz) {
    char user[16] = {0}, title[32] = {0}, kind[16] = {0}, sobj[256] = {0};
    if (!ls_snapshot_head(snap_id, user, title, kind, sobj)) { snprintf(err, err_sz, "no snapshot #%lld", (long long)snap_id); return -1; }
    const char *obj = (object_id && object_id[0]) ? object_id : (strcmp(kind, "object") == 0 ? sobj : NULL);
    if (exact && obj) { snprintf(err, err_sz, "an exact restore is only possible for a whole game snapshot"); return -1; }
    ls_snap_file_t *files = NULL;
    int n = ls_snapshot_files(snap_id, &files);
    int sel = 0;
    char missing[300] = {0};
    size_t mpos = 0;
    for (int i = 0; i < n; i++) {
        if (obj && strcmp(files[i].object_id, obj) != 0) { files[i].save_name[0] = 0; continue; }
        if (!ls_valid_save_name(files[i].save_name)) {
            snprintf(err, err_sz, "%s cannot be restored (unsupported name)", files[i].save_name);
            free(files);
            return -1;
        }
        ls_queue_row_t q;
        if (files[i].queue_id[0] && ls_queue_get(files[i].queue_id, &q) && strcmp(q.state, "packed") == 0) {
            char e2[200];                       /* PS5-only mode: rebuild the exact ZIP first */
            if (ls_archive_ensure_ready(files[i].queue_id, e2, sizeof(e2)) < 0) ls_event("RESTORE_UNPACK_FAILED", NULL, "%s: %s", files[i].queue_id, e2);
        }
        if (!files[i].queue_id[0] || !ls_queue_get(files[i].queue_id, &q) || strcmp(q.state, "ready") != 0) {
            if (mpos < sizeof(missing) - 1)
                mpos += (size_t)snprintf(missing + mpos, sizeof(missing) - mpos, "%s%s", mpos ? ", " : "", files[i].save_name);
            if (mpos >= sizeof(missing)) mpos = sizeof(missing) - 1;
        } else {
            snprintf(files[i].queue_id, sizeof(files[i].queue_id), "%s/%s", LS_QUEUE_DIR, q.filename);
        }
        sel++;
    }
    if (!sel) {
        free(files);
        if (obj) snprintf(err, err_sz, "save %s is not in snapshot #%lld", obj, (long long)snap_id);
        else snprintf(err, err_sz, "snapshot #%lld is empty", (long long)snap_id);
        return -1;
    }
    if (missing[0]) {
        free(files);
        snprintf(err, err_sz, "archives no longer on the PS5 for: %s (restore them from the PC)", missing);
        return -7;
    }
    ls_save_list_t rm = {0};
    if (exact) {
        char fp[65];
        ls_restore_removals(user, title, files, n, &rm, fp);
        if (!expect_fp || strcmp(expect_fp, fp) != 0) {
            ls_save_list_free(&rm);
            free(files);
            snprintf(err, err_sz, "the saves on the PS5 changed since you confirmed the removals; review the list again");
            return -8;
        }
    }
    if (sel + rm.count > LS_RESTORE_MAX_ITEMS) {
        ls_save_list_free(&rm);
        free(files);
        snprintf(err, err_sz, "too many files (%d)", sel + rm.count);
        return -1;
    }
    int rc = ls_restore_claim(user, title, err, err_sz);
    if (rc < 0) { ls_save_list_free(&rm); free(files); return rc; }
    pthread_mutex_lock(&g_ls_restore_mtx);
    ls_restore_init_locked("snapshot", obj ? "object" : "title", user, title);
    g_ls_restore.snap_id = snap_id;
    if (obj) snprintf(g_ls_restore.object_id, sizeof(g_ls_restore.object_id), "%s", obj);
    for (int i = 0; i < n; i++) {
        if (!files[i].save_name[0]) continue;
        if (!g_ls_restore.save_name[0])
            snprintf(g_ls_restore.save_name, sizeof(g_ls_restore.save_name), "%s", files[i].save_name);
        ls_restore_add_item_locked(files[i].save_name, files[i].queue_id, files[i].sha, files[i].gen_id);
    }
    g_ls_restore.exact = exact;
    for (int i = 0; i < rm.count; i++) {
        ls_restore_add_item_locked(rm.items[i].save_name, "", NULL, 0);
        g_ls_restore.items[g_ls_restore.nitems - 1].remove = 1;
    }
    pthread_mutex_unlock(&g_ls_restore_mtx);
    free(files);
    char what[300];
    if (obj) snprintf(what, sizeof(what), "save %s from snapshot #%lld (%d file(s))", obj, (long long)snap_id, sel);
    else snprintf(what, sizeof(what), "game snapshot #%lld (%d file(s))%s", (long long)snap_id, sel,
                  exact ? (rm.count ? ", exact: removing the files not in it" : ", exact") : "");
    for (int i = 0; i < rm.count; i++)
        ls_event("RESTORE_REMOVAL_CONFIRMED", title, "%s will be removed (not in snapshot #%lld)", rm.items[i].save_name,
                 (long long)snap_id);
    ls_save_list_free(&rm);
    return ls_restore_enqueue(title, what, user, "snapshot", err, err_sz);
}

/* Copies a file durably with the given mode/owner; returns its SHA-256. */
static int ls_place_copy(const char *src, const char *dst_tmp, mode_t mode, uid_t uid, gid_t gid, int set_owner,
                         char sha[65], char *iobuf) {
    if (ls_copy_hash(src, dst_tmp, sha, NULL, iobuf) < 0) return -1;
    if (chmod(dst_tmp, mode) < 0) { unlink(dst_tmp); return -1; }
    if (set_owner && chown(dst_tmp, uid, gid) < 0) { /* best effort */ }
    return 0;
}

/* savedata.db: refresh mtime, or insert the row (from param.sfo) if the save had been deleted. */
static int ls_restore_db_row(const char *user_id, int is_ps4, const sfo_info_t *sfo, uint64_t aid) {
    char path[MAX_PATH_LEN], dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/%s/%s/db/user", LS_SYSTEM_DATA, is_ps4 ? "savedata" : "savedata_prospero", user_id);
    snprintf(path, sizeof(path), "%s/savedata.db", dir);
    struct stat st;
    if (stat(path, &st) < 0) return -1;            /* never create a system DB from scratch */
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK) { if (db) sqlite3_close(db); return -1; }
    sqlite3_busy_timeout(db, 3000);
    char mtime[64];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(mtime, sizeof(mtime), "%Y-%m-%dT%H:%M:%S.00Z", &tmv);
    int rc = -1;
    sqlite3_stmt *st1 = NULL;
    if (sqlite3_prepare_v2(db, "UPDATE savedata SET mtime = ? WHERE title_id = ? AND dir_name = ?", -1, &st1, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st1, 1, mtime, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st1, 2, sfo->title_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st1, 3, sfo->dir_name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st1) == SQLITE_DONE) rc = sqlite3_changes(db) > 0 ? 0 : 1;
    }
    sqlite3_finalize(st1);
    if (rc == 1) {   /* row missing: same columns upstream's import_finish writes */
        sqlite3_stmt *st2 = NULL;
        rc = -1;
        if (sqlite3_prepare_v2(db,
                "INSERT INTO savedata (title_id, dir_name, main_title, sub_title, detail, is_broken, user_param,"
                " blocks, free_blocks, size_kib, mtime, fake_broken, account_id, user_id, faked_owner, cloud_revision,"
                " game_title_id, system_blocks) VALUES (?,?,?,?,?,0,?,?,?,?,?,0,?,?,0,0,?,0)", -1, &st2, NULL) == SQLITE_OK) {
            sqlite3_bind_text(st2, 1, sfo->title_id, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st2, 2, sfo->dir_name, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st2, 3, sfo->main_title, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st2, 4, sfo->sub_title, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st2, 5, sfo->detail, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st2, 6, sfo->user_param);
            sqlite3_bind_int64(st2, 7, sfo->blocks);
            sqlite3_bind_int64(st2, 8, sfo->blocks);
            sqlite3_bind_int64(st2, 9, (sqlite3_int64)sfo->blocks * 0x40);
            sqlite3_bind_text(st2, 10, mtime, -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st2, 11, (sqlite3_int64)aid);
            sqlite3_bind_int64(st2, 12, (sqlite3_int64)strtoul(user_id, NULL, 16));
            sqlite3_bind_text(st2, 13, sfo->game_title_id[0] ? sfo->game_title_id : sfo->title_id, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st2) == SQLITE_DONE) rc = 0;
        }
        sqlite3_finalize(st2);
    }
    sqlite3_close(db);
    return rc;
}

/*
 * Account id of a PS5 user. Upstream's lookup (username.dat matched against
 * the registry account names) comes first; when it finds nothing, the
 * system's own savedata.db is used: the row of this very save, otherwise the
 * account id that the clear majority of the user's saves carry. `how`
 * describes the source (or why nothing was found) for the event log.
 */
static uint64_t ls_restore_account_id(const char *user_id, int is_ps4, const char *title_id,
                                      const char *dir_name, char *how, size_t how_sz) {
    uint64_t aid = get_account_id_for_user(user_id);
    if (aid) { snprintf(how, how_sz, "registry"); return aid; }
    char upath[MAX_PATH_LEN];
    snprintf(upath, sizeof(upath), "%s/%s/username.dat", LS_USER_HOME, user_id);
    struct stat ust;
    const char *why = stat(upath, &ust) < 0 ? "username.dat missing" : "username.dat not matched in registry";
    sqlite3 *db = ls_open_savedata_db(user_id, is_ps4);
    if (!db) { snprintf(how, how_sz, "%s; savedata.db unreadable", why); return 0; }
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT account_id FROM savedata WHERE title_id = ? AND dir_name = ?"
                               " AND account_id IS NOT NULL AND account_id != 0 LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, title_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, dir_name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) aid = (uint64_t)sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (aid) {
        snprintf(how, how_sz, "%s; used savedata.db entry of this save", why);
    } else {
        long long top = 0, total = 0;
        if (sqlite3_prepare_v2(db, "SELECT account_id, COUNT(*), (SELECT COUNT(*) FROM savedata WHERE account_id IS NOT NULL"
                                   " AND account_id != 0) FROM savedata WHERE account_id IS NOT NULL AND account_id != 0"
                                   " GROUP BY account_id ORDER BY COUNT(*) DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            top = sqlite3_column_int64(st, 1);
            total = sqlite3_column_int64(st, 2);
            if (top * 2 > total) aid = (uint64_t)sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (aid) snprintf(how, how_sz, "%s; used the account of %lld of %lld saves in savedata.db", why, top, total);
        else snprintf(how, how_sz, "%s; savedata.db has no clear account (%lld of %lld)", why, top, total);
    }
    sqlite3_close(db);
    return aid;
}

/*
 * Copies an image (and its PS4 key file) into `dir`, mounts the copy with
 * upstream mount_by_path, reads param.sfo and removes the copy again. The
 * original is never mounted. Returns 0, or -1 with `err` set.
 */
static int ls_restore_read_sfo(const char *img, const char *key, const char *dir, const char *save_name,
                               sfo_info_t *sfo, char *err, size_t err_sz) {
    char cimg[MAX_PATH_LEN], sha[65];
    mkdir(dir, 0777);
    snprintf(cimg, sizeof(cimg), "%s/%s", dir, save_name);
    char *io = malloc(BUF_SIZE);
    if (!io) { snprintf(err, err_sz, "out of memory"); return -1; }
    int rc = ls_copy_hash(img, cimg, sha, NULL, io);
    if (rc == 0 && key) {
        char ckey[MAX_PATH_LEN];
        snprintf(ckey, sizeof(ckey), "%s/%s.bin", dir, save_name + 6);
        rc = ls_copy_hash(key, ckey, sha, NULL, io);
    }
    free(io);
    int ok = 0;
    if (rc < 0) {
        snprintf(err, err_sz, "cannot prepare check copy");
    } else {
        pthread_mutex_lock(&g_mtx);
        if (g_mounted) {
            snprintf(err, err_sz, "a save is mounted in the browser; unmount it first");
        } else {
            int mr = mount_by_path(cimg);
            if (mr < 0) {
                snprintf(err, err_sz, "image does not mount (0x%x)", mr);
            } else {
                char sfo_path[MAX_PATH_LEN];
                snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", g_mount_point);
                int pr = parse_sfo(sfo_path, sfo);
                unmount_save();
                if (pr < 0) snprintf(err, err_sz, "param.sfo unreadable (%d)", pr);
                else ok = 1;
            }
        }
        pthread_mutex_unlock(&g_mtx);
    }
    delete_recursive(dir);
    rmdir(dir);
    return ok ? 0 : -1;
}

/*
 * Validates the staged image: title, save directory and account. The
 * archive must carry either the user's account id (registry / savedata.db)
 * or the account id inside the save currently in that slot on the PS5 (read
 * from a copy). The second case covers consoles without a known account id
 * and saves written before an (offline) account activation changed it.
 * An archive with account id 0 (written while the user had no activated
 * account) is accepted for re-signing: *resign_out is set to the account it
 * must be re-signed to (the user's, else the current save's).
 * `target`/`target_key` are NULL when the save does not exist on the PS5.
 */
static int ls_restore_check_image(const char *work, const char *save_name, int is_ps4, const char *user_id,
                                  const char *title_id, const char *target, const char *target_key,
                                  sfo_info_t *sfo, uint64_t *aid_out, uint64_t *resign_out, char *err, size_t err_sz) {
    *resign_out = 0;
    char dir[MAX_PATH_LEN], img[MAX_PATH_LEN], key[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/check", work);
    snprintf(img, sizeof(img), "%s/%s", work, save_name);
    snprintf(key, sizeof(key), "%s/%s.bin", work, save_name + 6);
    char e2[160];
    if (ls_restore_read_sfo(img, is_ps4 ? key : NULL, dir, save_name, sfo, e2, sizeof(e2)) < 0) {
        snprintf(err, err_sz, "archived %s", e2);
        if (strstr(e2, "mounted")) snprintf(err, err_sz, "%s", e2);
        return -1;
    }
    if (strcmp(sfo->title_id, title_id) != 0) {
        snprintf(err, err_sz, "archive is for %s, not %s", sfo->title_id, title_id);
        return -1;
    }
    char oid[256];
    int role = ls_object_of(save_name, oid, sizeof(oid));
    if (strcmp(sfo->dir_name, save_name + 6) != 0 && !(role == LS_ROLE_BACKUP && strcmp(sfo->dir_name, oid) == 0)) {
        snprintf(err, err_sz, "archive save directory is %s, not %s", sfo->dir_name, save_name + 6);
        return -1;
    }
    char how[240];
    uint64_t aid = ls_restore_account_id(user_id, is_ps4, title_id, role == LS_ROLE_BACKUP ? oid : save_name + 6,
                                         how, sizeof(how));
    if (aid && sfo->account_id == aid) {
        if (strcmp(how, "registry") != 0) ls_event("RESTORE_ACCOUNT", title_id, "%s: %s", save_name, how);
        if (aid_out) *aid_out = aid;
        return 0;
    }
    char known[64];
    if (aid) snprintf(known, sizeof(known), "user account %llx", (unsigned long long)aid);
    else snprintf(known, sizeof(known), "user account unknown");
    uint64_t live_aid = 0;
    char live_txt[200] = "no current save to compare with";
    if (target) {
        sfo_info_t live;
        snprintf(dir, sizeof(dir), "%s/check_live", work);
        if (ls_restore_read_sfo(target, target_key, dir, save_name, &live, e2, sizeof(e2)) < 0) {
            snprintf(live_txt, sizeof(live_txt), "the current save could not be read (%s)", e2);
        } else {
            live_aid = live.account_id;
            snprintf(live_txt, sizeof(live_txt), "current save %llx", (unsigned long long)live_aid);
            if (sfo->account_id == live_aid) {
                ls_event("RESTORE_ACCOUNT", title_id, "%s: archive account %llx matches the save currently on the PS5 (%s; %s)",
                         save_name, (unsigned long long)sfo->account_id, known, how);
                if (aid_out) *aid_out = live_aid;
                return 0;
            }
        }
    }
    uint64_t to = aid ? aid : live_aid;
    if (sfo->account_id == 0 && to) {
        ls_event("RESTORE_ACCOUNT", title_id, "%s: archive has no account (written before account activation);"
                 " it will be re-signed to %llx (%s; %s)", save_name, (unsigned long long)to, known, live_txt);
        *resign_out = to;
        if (aid_out) *aid_out = to;
        return 0;
    }
    snprintf(err, err_sz, "archive belongs to another account (%llx; %s, %s); use Import to re-sign",
             (unsigned long long)sfo->account_id, known, live_txt);
    return -1;
}

/*
 * Re-signs a copy of the staged image to `aid` exactly as upstream's import
 * does (account id in param.sfo at 0x1B8, PS4 0x15C; PS5 also the user id at
 * 0x660), then re-reads it to confirm. On success `img` (and `key`) name the
 * re-signed copy and `sha` its new hash; the archived image is left as is.
 */
static int ls_restore_resign(const char *work, const char *save_name, int is_ps4, const char *user_id, uint64_t aid,
                             char *img, size_t img_sz, char *key, size_t key_sz, char *sha, char *iobuf,
                             char *err, size_t err_sz) {
    char dir[MAX_PATH_LEN], nimg[MAX_PATH_LEN], nkey[MAX_PATH_LEN], ksha[65];
    snprintf(dir, sizeof(dir), "%s/resigned", work);
    mkdir(dir, 0777);
    snprintf(nimg, sizeof(nimg), "%s/%s", dir, save_name);
    snprintf(nkey, sizeof(nkey), "%s/%s.bin", dir, save_name + 6);
    if (ls_copy_hash(img, nimg, sha, NULL, iobuf) < 0 || (is_ps4 && ls_copy_hash(key, nkey, ksha, NULL, iobuf) < 0)) {
        snprintf(err, err_sz, "cannot prepare the re-sign copy");
        return -1;
    }
    uint32_t uid = (uint32_t)strtoul(user_id, NULL, 16);
    int ok = 0;
    pthread_mutex_lock(&g_mtx);
    if (g_mounted) {
        snprintf(err, err_sz, "a save is mounted in the browser; unmount it first");
    } else {
        int mr = mount_by_path(nimg);
        if (mr < 0) {
            snprintf(err, err_sz, "re-sign copy does not mount (0x%x)", mr);
        } else {
            char sfo_path[MAX_PATH_LEN];
            snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", g_mount_point);
            int fd = open(sfo_path, O_RDWR);
            if (fd < 0) {
                snprintf(err, err_sz, "param.sfo of the re-sign copy not writable (%d)", errno);
            } else {
                int w = pwrite(fd, &aid, 8, is_ps4 ? 0x15C : 0x1B8) == 8;
                if (w && !is_ps4) w = pwrite(fd, &uid, 4, 0x660) == 4;
                fsync(fd);
                close(fd);
                sync();
                if (!w) snprintf(err, err_sz, "could not write param.sfo of the re-sign copy");
                else ok = 1;
            }
            unmount_save();
        }
    }
    pthread_mutex_unlock(&g_mtx);
    if (!ok) return -1;
    /* Confirm on a fresh mount (of a further copy) that the account id took. */
    sfo_info_t chk;
    char cdir[MAX_PATH_LEN], e2[160];
    snprintf(cdir, sizeof(cdir), "%s/check_resigned", work);
    if (ls_restore_read_sfo(nimg, is_ps4 ? nkey : NULL, cdir, save_name, &chk, e2, sizeof(e2)) < 0) {
        snprintf(err, err_sz, "re-signed copy unreadable (%s)", e2);
        return -1;
    }
    if (chk.account_id != aid) {
        snprintf(err, err_sz, "re-signing did not take (account %llx after writing %llx)",
                 (unsigned long long)chk.account_id, (unsigned long long)aid);
        return -1;
    }
    if (ls_sha256_file(nimg, sha, NULL, iobuf) < 0) { snprintf(err, err_sz, "cannot hash the re-signed copy"); return -1; }
    snprintf(img, img_sz, "%s", nimg);
    if (is_ps4) snprintf(key, key_sz, "%s", nkey);
    return 0;
}

/* Per-file working state of a running restore. */
typedef struct {
    int skip, is_ps4, existed, changed, placed, kb_placed, kb_existed;
    struct stat tst, kst;
    char work[MAX_PATH_LEN], tdir[MAX_PATH_LEN], target[MAX_PATH_LEN];
    char staged[MAX_PATH_LEN], kb_staged[MAX_PATH_LEN], kb_target[MAX_PATH_LEN];
    char prev_copy[MAX_PATH_LEN], prev_kb[MAX_PATH_LEN];
    char sha[65], kb_sha[65], prev_sha[65];
    int64_t size;
    sfo_info_t sfo;
    uint64_t aid, resign;
} ls_rs_t;

/* Puts back what was on the PS5 before file `x` was placed. 0 = ok. */
static int ls_restore_undo_one(ls_rs_t *x, char *iobuf) {
    int ok = 1;
    if (x->placed) {
        if (x->existed && x->prev_copy[0]) {
            char tmp[MAX_PATH_LEN], bsha[65];
            snprintf(tmp, sizeof(tmp), "%s.undo.tmp", x->target);
            if (ls_place_copy(x->prev_copy, tmp, x->tst.st_mode & 07777, x->tst.st_uid, x->tst.st_gid, 1, bsha, iobuf) < 0 ||
                strcmp(bsha, x->prev_sha) != 0 || rename(tmp, x->target) < 0) { unlink(tmp); ok = 0; }
        } else if (!x->existed) {
            if (unlink(x->target) < 0 && errno != ENOENT) ok = 0;     /* file this restore created */
        } else {
            ok = 0;
        }
    }
    if (x->kb_placed) {
        if (x->kb_existed && x->prev_kb[0]) {
            char tmp[MAX_PATH_LEN], ksha[65];
            snprintf(tmp, sizeof(tmp), "%s.undo.tmp", x->kb_target);
            if (ls_place_copy(x->prev_kb, tmp, x->kst.st_mode & 07777, x->kst.st_uid, x->kst.st_gid, 1, ksha, iobuf) < 0 ||
                rename(tmp, x->kb_target) < 0) { unlink(tmp); ok = 0; }
        } else if (!x->kb_existed) {
            unlink(x->kb_target);
        }
    }
    return ok ? 0 : -1;
}

static const char *ls_restore_game_name(const char *title_id) {
    const char *n = lookup_title(title_id);
    return n && n[0] ? n : title_id;
}

static void ls_restore_run(char *iobuf) {
    ls_restore_t *r = malloc(sizeof(*r));
    if (!r) return;
    pthread_mutex_lock(&g_ls_restore_mtx);
    *r = g_ls_restore;
    if (r->active) snprintf(g_ls_restore.state, sizeof(g_ls_restore.state), "running");
    pthread_mutex_unlock(&g_ls_restore_mtx);
    if (!r->active || r->nitems <= 0) { free(r); return; }
    int single = strcmp(r->kind, "file") == 0;
    int64_t op = ls_op_begin("restore", single ? r->save_name : r->id);
    int touched = 0;      /* set once a rollback archive / a save itself may have changed */
    char err[300] = {0};
    char work[MAX_PATH_LEN];
    mkdir(LS_RESTORE_DIR, 0777);
    snprintf(work, sizeof(work), "%s/%s", LS_RESTORE_DIR, r->id);
    mkdir(work, 0777);
    int n = r->nitems;
    ls_rs_t *xs = calloc((size_t)n, sizeof(*xs));
    if (!xs) {
        ls_op_end(op, "failed", "out of memory");
        ls_restore_end("failed", "out of memory");
        free(r);
        return;
    }
    const char *game = ls_restore_game_name(r->title_id);

#define LS_FAIL(...) do { snprintf(err, sizeof(err), __VA_ARGS__); goto fail; } while (0)

    /* 1+2. Stage and validate every file before anything on the PS5 changes. */
    for (int i = 0; i < n; i++) {
        ls_restore_item_t *it = &r->items[i];
        ls_rs_t *x = &xs[i];
        snprintf(x->work, sizeof(x->work), "%s/%d", work, i);
        mkdir(x->work, 0777);
        snprintf(x->tdir, sizeof(x->tdir), "%s/%s/savedata/%s", LS_USER_HOME, r->user_id, r->title_id);
        snprintf(x->target, sizeof(x->target), "%s/%s", x->tdir, it->save_name);
        if (stat(x->target, &x->tst) == 0) x->is_ps4 = 1;
        else {
            snprintf(x->tdir, sizeof(x->tdir), "%s/%s/savedata_prospero/%s", LS_USER_HOME, r->user_id, r->title_id);
            snprintf(x->target, sizeof(x->target), "%s/%s", x->tdir, it->save_name);
        }
        x->existed = stat(x->target, &x->tst) == 0 && S_ISREG(x->tst.st_mode);
        if (it->remove) {
            if (!x->existed) { x->skip = 1; ls_restore_item_set(i, "skipped", "already gone"); }
            continue;                         /* nothing to stage: this file is removed */
        }
        /* A backup copy whose primary is not part of this restore must not be
         * placed next to a primary that exists on the PS5 (never mixes states).
         * A primary that this exact restore removes counts as part of it. */
        if (it->role == LS_ROLE_BACKUP) {
            int prim_in_set = 0;
            for (int k = 0; k < n; k++)
                if (k != i && r->items[k].role == LS_ROLE_PRIMARY && strcmp(r->items[k].object_id, it->object_id) == 0)
                    prim_in_set = 1;
            char pname[300], ppath[MAX_PATH_LEN];
            struct stat pst;
            ls_object_member_name(it->save_name, it->object_id, LS_ROLE_PRIMARY, pname, sizeof(pname));
            snprintf(ppath, sizeof(ppath), "%s/%s", x->tdir, pname);
            if (!prim_in_set && !single && stat(ppath, &pst) == 0) {
                x->skip = 1;
                ls_restore_item_set(i, "skipped", "backup copy without its primary in the snapshot; %s on the PS5 left alone", pname);
                continue;
            }
        }
        ls_restore_step(n > 1 ? "staging all files" : "staging");
        char entry[400];
        snprintf(entry, sizeof(entry), "%s/%s", r->title_id, it->save_name);
        snprintf(x->staged, sizeof(x->staged), "%s/%s", x->work, it->save_name);
        char e2[300] = {0};
        if (ls_zip_extract(it->archive_path, entry, x->staged, x->sha, &x->size, iobuf, e2, sizeof(e2)) < 0) {
            ls_restore_item_set(i, "failed", "%s", e2);
            LS_FAIL("%s%s%s", single ? "" : it->save_name, single ? "" : ": ", e2);
        }
        if (it->expected_sha[0] && strcasecmp(x->sha, it->expected_sha) != 0) {
            ls_restore_item_set(i, "failed", "staged hash does not match");
            LS_FAIL("%s%sstaged image hash %.16s does not match the expected version %.16s", single ? "" : it->save_name,
                    single ? "" : ": ", x->sha, it->expected_sha);
        }
        /* PS4 if the image header says so (0x01), as upstream detects it. */
        {
            int hfd = open(x->staged, O_RDONLY);
            uint8_t hdr = 0;
            if (hfd >= 0) { if (pread(hfd, &hdr, 1, 0) != 1) hdr = 0; close(hfd); }
            if (hdr == 0x01 && !x->is_ps4 && !x->existed) {
                x->is_ps4 = 1;
                snprintf(x->tdir, sizeof(x->tdir), "%s/%s/savedata/%s", LS_USER_HOME, r->user_id, r->title_id);
                snprintf(x->target, sizeof(x->target), "%s/%s", x->tdir, it->save_name);
            }
        }
        if (x->is_ps4) {
            char kb_entry[400];
            snprintf(kb_entry, sizeof(kb_entry), "%s/%s.bin", r->title_id, it->save_name + 6);
            snprintf(x->kb_staged, sizeof(x->kb_staged), "%s/%s.bin", x->work, it->save_name + 6);
            snprintf(x->kb_target, sizeof(x->kb_target), "%s/%s.bin", x->tdir, it->save_name + 6);
            if (ls_zip_extract(it->archive_path, kb_entry, x->kb_staged, x->kb_sha, NULL, iobuf, e2, sizeof(e2)) < 0) {
                ls_restore_item_set(i, "failed", "%s", e2);
                LS_FAIL("%s: %s", it->save_name, e2);
            }
        }
        ls_event("RESTORE_STAGED", r->title_id, "%s %lld bytes sha=%.16s", it->save_name, (long long)x->size, x->sha);

        ls_restore_step(n > 1 ? "validating all files" : "validating");
        if (ls_restore_check_image(x->work, it->save_name, x->is_ps4, r->user_id, r->title_id, x->existed ? x->target : NULL,
                                   x->existed && x->is_ps4 ? x->kb_target : NULL, &x->sfo, &x->aid, &x->resign,
                                   e2, sizeof(e2)) < 0) {
            ls_restore_item_set(i, "failed", "%s", e2);
            LS_FAIL("%s%s%s", single ? "" : it->save_name, single ? "" : ": ", e2);
        }
        if (x->resign) {
            ls_restore_step("re-signing");
            char archived_sha[65];
            snprintf(archived_sha, sizeof(archived_sha), "%s", x->sha);
            if (ls_restore_resign(x->work, it->save_name, x->is_ps4, r->user_id, x->resign, x->staged, sizeof(x->staged),
                                  x->kb_staged, sizeof(x->kb_staged), x->sha, iobuf, e2, sizeof(e2)) < 0) {
                ls_restore_item_set(i, "failed", "%s", e2);
                LS_FAIL("%s%s%s", single ? "" : it->save_name, single ? "" : ": ", e2);
            }
            if (x->is_ps4 && ls_sha256_file(x->kb_staged, x->kb_sha, NULL, iobuf) < 0) LS_FAIL("cannot hash the re-signed key file");
            x->sfo.account_id = x->resign;
            ls_event("RESTORE_RESIGNED", r->title_id, "%s re-signed to %llx (archived sha %.16s -> %.16s)", it->save_name,
                     (unsigned long long)x->resign, archived_sha, x->sha);
        }
    }
    {
        int todo = 0;
        for (int i = 0; i < n; i++) if (!xs[i].skip && !r->items[i].remove) todo++;
        if (!todo && strcmp(r->kind, "delete") != 0) LS_FAIL("nothing to restore: every file of the selection was skipped");
        if (strcmp(r->kind, "delete") == 0) {
            int any = 0;
            for (int i = 0; i < n; i++) if (!xs[i].skip) any = 1;
            if (!any) LS_FAIL("save %s is already gone", r->object_id);
        }
    }

    /* 3. Back up the whole game as it is now (one snapshot to go back to). */
    if (ls_title_is_running(r->title_id)) LS_FAIL("%s was started; restore aborted before touching the save", r->title_id);
    ls_restore_step("rollback");
    touched = 1;
    {
        int64_t presnap = 0;
        int left = ls_protect_title_ex(r->title_id, strcmp(r->kind, "delete") == 0 ? "pre_delete" : "pre_restore", iobuf, &presnap);
        if (left > 0) LS_FAIL("could not back up the current saves first (%d file(s) not protected); nothing was replaced", left);
        int64_t prev_gen = 0;
        if (single && xs[0].existed) {
            ls_save_ref_t ref;
            memset(&ref, 0, sizeof(ref));
            snprintf(ref.user_id, sizeof(ref.user_id), "%s", r->user_id);
            snprintf(ref.title_id, sizeof(ref.title_id), "%s", r->title_id);
            snprintf(ref.save_name, sizeof(ref.save_name), "%s", r->items[0].save_name);
            ls_save_row_t row;
            ls_saves_get(&ref, &row);
            prev_gen = row.current_gen_id;
        }
        pthread_mutex_lock(&g_ls_restore_mtx);
        g_ls_restore.rollback_snap = presnap;
        g_ls_restore.rollback_gen = prev_gen;
        pthread_mutex_unlock(&g_ls_restore_mtx);
        r->rollback_snap = presnap;
        r->rollback_gen = prev_gen;
        if (single)
            ls_event("ROLLBACK_CREATED", r->title_id, "%s current version is gen %lld (archived before restore; game snapshot #%lld)",
                     r->items[0].save_name, (long long)prev_gen, (long long)presnap);
        else if (presnap)
            ls_event("ROLLBACK_CREATED", r->title_id, "game snapshot #%lld holds the saves as they were before this %s",
                     (long long)presnap, strcmp(r->kind, "delete") == 0 ? "delete" : "restore");
        else
            ls_event("ROLLBACK_CREATED", r->title_id, "the game had no saves on the PS5 before this restore; nothing to keep");
    }
    int changed = 0;
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        if (x->skip) continue;
        if (x->existed) {
            snprintf(x->prev_copy, sizeof(x->prev_copy), "%s/previous.img", x->work);
            if (ls_copy_hash(x->target, x->prev_copy, x->prev_sha, NULL, iobuf) < 0)
                LS_FAIL("could not keep a copy of the current %s", r->items[i].save_name);
        }
        if (x->is_ps4) {
            x->kb_existed = stat(x->kb_target, &x->kst) == 0;
            if (x->kb_existed) {
                char tmpsha[65];
                snprintf(x->prev_kb, sizeof(x->prev_kb), "%s/previous.bin", x->work);
                if (ls_copy_hash(x->kb_target, x->prev_kb, tmpsha, NULL, iobuf) < 0) x->prev_kb[0] = 0;
            }
        }
        if (r->items[i].remove) {
            /* Only remove what the pre-restore backup demonstrably holds. */
            ls_save_ref_t ref;
            memset(&ref, 0, sizeof(ref));
            snprintf(ref.user_id, sizeof(ref.user_id), "%s", r->user_id);
            snprintf(ref.title_id, sizeof(ref.title_id), "%s", r->title_id);
            snprintf(ref.save_name, sizeof(ref.save_name), "%s", r->items[i].save_name);
            ls_save_row_t row;
            ls_saves_get(&ref, &row);
            int64_t safe = 0;
            pthread_mutex_lock(&g_ls_mtx);
            if (g_ls_db && row.current_gen_id > 0)
                safe = ls_db_int_locked("SELECT COUNT(*) FROM generations g LEFT JOIN queue q ON q.id = g.queue_id"
                                        " WHERE g.sha256 = ? AND g.save_name = ? AND g.title_id = ?"
                                        " AND (q.state IN ('ready','packed') OR g.pc_acked_at > 0) AND g.gen_id = ?",
                                        x->prev_sha, r->items[i].save_name, r->title_id, row.current_gen_id);
            pthread_mutex_unlock(&g_ls_mtx);
            if (!safe || strcmp(row.state, "protected") != 0) {
                ls_restore_item_set(i, "failed", "not safely archived; not removed");
                LS_FAIL("%s would be removed but is not safely archived; nothing was replaced", r->items[i].save_name);
            }
            x->changed = 1;
            changed++;
            continue;
        }
        x->changed = !x->existed || strcmp(x->prev_sha, x->sha) != 0;
        if (x->changed) changed++;
        else ls_restore_item_set(i, "unchanged", "already this version");
    }
    if (!changed) {
        ls_op_end(op, "ok", "already identical");
        for (int i = 0; i < n; i++)
            if (!xs[i].skip)
                ls_event("RESTORE_VERIFIED", r->title_id, "%s already has this content; nothing changed", r->items[i].save_name);
        delete_recursive(work);
        rmdir(work);
        free(xs);
        ls_restore_end("done", single ? "the PS5 already had exactly this version; nothing was changed"
                                      : "the PS5 already had exactly these versions; nothing was changed");
        free(r);
        return;
    }

    /* 4. Swap, file by file. Any failure undoes everything placed so far. */
    ls_restore_step("restoring");
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        ls_restore_item_t *it = &r->items[i];
        if (x->skip || !x->changed || it->remove) continue;
        ls_event("RESTORE_STARTED", r->title_id, "%s -> %s", it->save_name, x->target);
        if (ls_title_is_running(r->title_id)) LS_FAIL("%s was started; restore aborted", r->title_id);
        recursive_mkdir(x->tdir);
        mode_t mode = x->existed ? (x->tst.st_mode & 07777) : 0644;      /* new files: same mode upstream import uses */
        if (x->is_ps4) {
            char tmp_kb[MAX_PATH_LEN], ksha[65];
            snprintf(tmp_kb, sizeof(tmp_kb), "%s.restore.tmp", x->kb_target);
            if (ls_place_copy(x->kb_staged, tmp_kb, x->kb_existed ? (x->kst.st_mode & 07777) : 0644, x->kst.st_uid, x->kst.st_gid,
                              x->kb_existed, ksha, iobuf) < 0 || strcmp(ksha, x->kb_sha) != 0 || rename(tmp_kb, x->kb_target) < 0) {
                unlink(tmp_kb);
                ls_restore_item_set(i, "failed", "could not place the PS4 key file");
                LS_FAIL("%s: could not place the PS4 key file", it->save_name);
            }
            x->kb_placed = 1;
        }
        char tmp_img[MAX_PATH_LEN], placed_sha[65];
        snprintf(tmp_img, sizeof(tmp_img), "%s.restore.tmp", x->target);
        if (ls_place_copy(x->staged, tmp_img, mode, x->tst.st_uid, x->tst.st_gid, x->existed, placed_sha, iobuf) < 0 ||
            strcmp(placed_sha, x->sha) != 0) {
            unlink(tmp_img);
            ls_restore_item_set(i, "failed", "could not write the restored image");
            LS_FAIL("%s: could not write the restored image", it->save_name);
        }
        if (rename(tmp_img, x->target) < 0) {
            unlink(tmp_img);
            ls_restore_item_set(i, "failed", "could not move the restored image into place");
            LS_FAIL("%s: could not move the restored image into place", it->save_name);
        }
        x->placed = 1;
    }
    /* Exact restore: remove the confirmed files that are not in the snapshot
     * (after every placement, so a placement failure removes nothing). */
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        ls_restore_item_t *it = &r->items[i];
        if (x->skip || !it->remove) continue;
        if (ls_title_is_running(r->title_id)) LS_FAIL("%s was started; restore aborted", r->title_id);
        if (x->is_ps4 && x->kb_existed) {
            if (unlink(x->kb_target) < 0 && errno != ENOENT) {
                ls_restore_item_set(i, "failed", "could not remove the PS4 key file");
                LS_FAIL("%s: could not remove the PS4 key file", it->save_name);
            }
            x->kb_placed = 1;
        }
        if (unlink(x->target) < 0 && errno != ENOENT) {
            ls_restore_item_set(i, "failed", "could not remove the file");
            LS_FAIL("%s: could not remove the file", it->save_name);
        }
        x->placed = 1;
        ls_event("RESTORE_REMOVED", r->title_id, "%s removed (not in the snapshot; archived as game snapshot #%lld)",
                 it->save_name, (long long)r->rollback_snap);
    }

    /* 5. Verify every placed file (and that removed files are gone). */
    ls_restore_step("verifying");
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        if (!x->placed) continue;
        if (r->items[i].remove) {
            struct stat gst;
            if (stat(x->target, &gst) == 0) {
                ls_restore_item_set(i, "failed", "still present after removal");
                LS_FAIL("%s: still present after removal", r->items[i].save_name);
            }
            continue;
        }
        char final_sha[65];
        if (ls_sha256_file(x->target, final_sha, NULL, iobuf) < 0 || strcmp(final_sha, x->sha) != 0) {
            ls_restore_item_set(i, "failed", "verification failed after placing");
            LS_FAIL("%s: verification failed after placing the image", r->items[i].save_name);
        }
    }

    /* 6. Record. */
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        if (!x->placed || r->items[i].role != LS_ROLE_PRIMARY) continue;    /* savedata.db lists primaries */
        if (r->items[i].remove) {
            /* Same as upstream's delete_save: the system DB row goes with the image. */
            char dbp[MAX_PATH_LEN];
            snprintf(dbp, sizeof(dbp), "%s/%s/%s/db/user/savedata.db", LS_SYSTEM_DATA,
                     x->is_ps4 ? "savedata" : "savedata_prospero", r->user_id);
            sqlite3 *db = NULL;
            int ok = 0;
            if (sqlite3_open_v2(dbp, &db, SQLITE_OPEN_READWRITE, NULL) == SQLITE_OK) {
                sqlite3_busy_timeout(db, 3000);
                sqlite3_stmt *st = NULL;
                if (sqlite3_prepare_v2(db, "DELETE FROM savedata WHERE title_id = ? AND dir_name = ?", -1, &st, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(st, 1, r->title_id, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(st, 2, r->items[i].save_name + 6, -1, SQLITE_TRANSIENT);
                    ok = sqlite3_step(st) == SQLITE_DONE;
                }
                sqlite3_finalize(st);
            }
            if (db) sqlite3_close(db);
            if (!ok) ls_event("RESTORE_WARNING", r->title_id, "%s removed, but its savedata.db row could not be deleted",
                              r->items[i].save_name);
            continue;
        }
        if (ls_restore_db_row(r->user_id, x->is_ps4, &x->sfo, x->aid) < 0)
            ls_event("RESTORE_WARNING", r->title_id, "%s restored, but savedata.db could not be updated", r->items[i].save_name);
    }
    /* Protection bookkeeping: identical content maps to its existing generation;
     * re-signed content is archived as a new one. Then the new game snapshot. */
    int64_t newsnap = 0;
    /* simulate: the placed older version is then seen exactly like a crash
     * leaving it on disk, so the regression check fires (stage N test aid). */
    ls_protect_title_ex(r->title_id, r->simulate ? "simulated_regression" :
                        strcmp(r->kind, "delete") == 0 ? "deleted" : "restored", iobuf, &newsnap);
    int restored = 0, objects = 0, removed = 0, removed_objects = 0;
    int64_t gen = 0;
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        if (!x->placed || !r->items[i].remove) continue;
        removed++;
        int first = 1;
        for (int k = 0; k < i; k++)
            if (xs[k].placed && r->items[k].remove && strcmp(r->items[k].object_id, r->items[i].object_id) == 0) first = 0;
        if (first) removed_objects++;
        ls_save_ref_t ref;
        memset(&ref, 0, sizeof(ref));
        snprintf(ref.user_id, sizeof(ref.user_id), "%s", r->user_id);
        snprintf(ref.title_id, sizeof(ref.title_id), "%s", r->title_id);
        snprintf(ref.save_name, sizeof(ref.save_name), "%s", r->items[i].save_name);
        ls_saves_observe(&ref, "absent", "missing", "removed by an exact restore");
        ls_restore_item_set(i, "removed", "archived in game snapshot #%lld", (long long)r->rollback_snap);
    }
    for (int i = 0; i < n; i++) {
        ls_rs_t *x = &xs[i];
        if (!x->placed || r->items[i].remove) continue;
        ls_save_ref_t ref;
        memset(&ref, 0, sizeof(ref));
        snprintf(ref.user_id, sizeof(ref.user_id), "%s", r->user_id);
        snprintf(ref.title_id, sizeof(ref.title_id), "%s", r->title_id);
        snprintf(ref.save_name, sizeof(ref.save_name), "%s", r->items[i].save_name);
        ls_save_row_t row;
        ls_saves_get(&ref, &row);
        gen = row.current_gen_id;
        restored++;
        int first_of_object = 1;
        for (int k = 0; k < i; k++)
            if (xs[k].placed && strcmp(r->items[k].object_id, r->items[i].object_id) == 0) first_of_object = 0;
        if (first_of_object) objects++;
        ls_restore_item_set(i, "restored", "now gen %lld", (long long)gen);
        ls_event("RESTORE_VERIFIED", r->title_id, "%s now = gen %lld (sha %.16s)%s%s", r->items[i].save_name, (long long)gen,
                 x->sha, x->existed ? "" : ", save was re-created", x->resign ? ", re-signed to the current account" : "");
    }
    /* Objects on the PS5 that the snapshot does not contain stay as they are. */
    int kept = 0;
    if (strcmp(r->kind, "title") == 0) {
        ls_save_list_t l = {0};
        ls_object_list_t ol = {0};
        ls_enum_saves(&l, r->title_id);
        if (ls_group_objects(&l, &ol) == 0) {
            for (int k = 0; k < ol.count; k++) {
                if (strcmp(ol.items[k].user_id, r->user_id) != 0) continue;
                int in = 0;
                for (int i = 0; i < n && !in; i++) in = strcmp(r->items[i].object_id, ol.items[k].id) == 0;
                if (!in) {
                    kept++;
                    ls_event("RESTORE_KEPT", r->title_id, "save %s is not in the snapshot; left as it is", ol.items[k].id);
                }
            }
        }
        ls_object_list_free(&ol);
        ls_save_list_free(&l);
    }
    ls_op_end(op, "ok", r->id);
    delete_recursive(work);
    rmdir(work);
    if (strcmp(r->source, "upload") == 0) unlink(r->items[0].archive_path);
    pthread_mutex_lock(&g_ls_restore_mtx);
    g_ls_restore.kept_objects = kept;
    g_ls_restore.removed_objects = removed_objects;
    pthread_mutex_unlock(&g_ls_restore_mtx);
    if (single) {
        char oid[256];
        ls_object_of(r->items[0].save_name, oid, sizeof(oid));
        notify("Shallot SaveMgr: restored save %s for %s", oid, game);
        ls_restore_end("done", "restored gen %lld; previous version kept as gen %lld", (long long)gen,
                       (long long)r->rollback_gen);
    } else if (strcmp(r->kind, "delete") == 0) {
        notify("Shallot SaveMgr: deleted save %s of %s (backed up first)", r->object_id, game);
        ls_restore_end("done", "deleted save %s (%d file(s)); it is kept in game snapshot #%lld and can be restored from Versions",
                       r->object_id, removed, (long long)r->rollback_snap);
    } else if (strcmp(r->kind, "object") == 0) {
        notify("Shallot SaveMgr: restored save %s for %s", r->object_id, game);
        ls_restore_end("done", "restored save %s (%d file(s)); the previous state is game snapshot #%lld",
                       r->object_id, restored, (long long)r->rollback_snap);
    } else {
        char extra[120] = "";
        if (removed_objects) snprintf(extra, sizeof(extra), ", removed %d save(s) not in it", removed_objects);
        else if (kept) snprintf(extra, sizeof(extra), ", other saves left as they are");
        if (removed_objects)
            notify("Shallot SaveMgr: restored %d save object(s) for %s, removed %d", objects, game, removed_objects);
        else
            notify("Shallot SaveMgr: restored %d save object(s) for %s", objects, game);
        ls_restore_end("done", "restored %d save object(s) (%d file(s)) of snapshot #%lld%s; the previous state is game snapshot #%lld",
                       objects, restored, (long long)r->snap_id, extra, (long long)r->rollback_snap);
        (void)removed;
    }
    (void)newsnap;
    free(xs);
    free(r);
    return;

fail:
    {
        /* Undo everything this restore placed, newest first. */
        int placed = 0, undo_failed = 0;
        char bad[200] = {0};
        for (int i = n - 1; i >= 0; i--) {
            if (!xs[i].placed && !xs[i].kb_placed) continue;
            placed++;
            if (ls_restore_undo_one(&xs[i], iobuf) == 0) {
                ls_restore_item_set(i, "undone", "previous version put back");
            } else {
                undo_failed++;
                ls_restore_item_set(i, "undo_failed", "COULD NOT put the previous version back (copy kept in %s)", xs[i].work);
                size_t bl = strlen(bad);
                snprintf(bad + bl, sizeof(bad) - bl, "%s%s", bl ? ", " : "", r->items[i].save_name);
            }
        }
        ls_op_end(op, undo_failed ? "partial" : "failed", err);
        if (!touched) {
            /* Refused before anything on the PS5 changed: no need to keep the staging. */
            delete_recursive(work);
            rmdir(work);
            if (strcmp(r->source, "upload") == 0) unlink(r->items[0].archive_path);
            ls_event("RESTORE_FAILED", r->title_id, "%s: %s (nothing was changed)", single ? r->items[0].save_name : r->id, err);
            notify("Shallot SaveMgr: restore for %s failed; nothing was changed", game);
            ls_restore_end("failed", "%s", err);
        } else if (undo_failed) {
            ls_event("RESTORE_PARTIAL", r->title_id, "%s; undo FAILED for %s (copies kept in %s; game snapshot #%lld has the previous state)",
                     err, bad, work, (long long)r->rollback_snap);
            notify("Shallot SaveMgr: restore for %s INCOMPLETE - check Backup & Sync", game);
            ls_restore_end("partial", "%s; could not undo %s - restore game snapshot #%lld to go back", err, bad,
                           (long long)r->rollback_snap);
        } else {
            ls_event("RESTORE_FAILED", r->title_id, "%s: %s%s (staging kept in %s)", single ? r->items[0].save_name : r->id, err,
                     placed ? "; every change was undone" : "", work);
            notify("Shallot SaveMgr: restore for %s failed%s", game, placed ? "; previous saves put back" : "");
            ls_restore_end("failed", "%s%s", err, placed ? "; every change was undone" : "");
        }
    }
    free(xs);
    free(r);
#undef LS_FAIL
}

static void ls_restore_status_json(char **buf, size_t *cap, size_t *pos) {
    pthread_mutex_lock(&g_ls_restore_mtx);
    const ls_restore_t *r = &g_ls_restore;
    char es[520], em[620], eo[520];
    json_escape(r->save_name, es, sizeof(es));
    json_escape(r->message, em, sizeof(em));
    json_escape(r->object_id, eo, sizeof(eo));
    ls_json_append(buf, cap, pos,
        "{\"active\":%s,\"id\":\"%s\",\"state\":\"%s\",\"step\":\"%s\",\"source\":\"%s\",\"kind\":\"%s\",\"user_id\":\"%s\","
        "\"title_id\":\"%s\",\"save_name\":\"%s\",\"object_id\":\"%s\",\"snap_id\":%lld,\"gen_id\":%lld,"
        "\"rollback_gen\":%lld,\"rollback_snap\":%lld,\"kept_objects\":%d,\"exact\":%s,\"removed_objects\":%d,\"started_at\":%lld,"
        "\"finished_at\":%lld,\"message\":\"%s\",\"items\":[",
        r->active ? "true" : "false", r->id, r->state[0] ? r->state : "idle", r->step, r->source,
        r->kind[0] ? r->kind : "file", r->user_id, r->title_id, es, eo, (long long)r->snap_id, (long long)r->gen_id,
        (long long)r->rollback_gen, (long long)r->rollback_snap, r->kept_objects, r->exact ? "true" : "false",
        r->removed_objects, (long long)r->started_at,
        (long long)r->finished_at, em);
    for (int i = 0; i < r->nitems; i++) {
        const ls_restore_item_t *it = &r->items[i];
        char en[520], eid[520], enote[340];
        json_escape(it->save_name, en, sizeof(en));
        json_escape(it->object_id, eid, sizeof(eid));
        json_escape(it->note, enote, sizeof(enote));
        ls_json_append(buf, cap, pos, "%s{\"save_name\":\"%s\",\"object_id\":\"%s\",\"role\":\"%s\",\"gen_id\":%lld,"
                       "\"remove\":%s,\"result\":\"%s\",\"note\":\"%s\"}", i ? "," : "", en, eid, ls_role_name(it->role),
                       (long long)it->gen_id, it->remove ? "true" : "false", it->result, enote);
    }
    ls_json_append(buf, cap, pos, "]}");
    pthread_mutex_unlock(&g_ls_restore_mtx);
}
