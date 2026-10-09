/*
 * LAN save sync — PS5-side history (mode "PS5 only"): block-level dedup of
 * archives and thinning of old versions. Included from main.c after
 * lansync_worker.inc.c.
 *
 * Block store (same layout idea as the PC client's): an archive's bytes are
 * cut into blocks (the first one ends where the save image starts inside the
 * ZIP, then 4 KiB, so they line up with the image's own blocks) and stored
 * once in append-only pack files under LS_BASE_DIR/blocks; recipes list the
 * blocks of each archive. Consecutive versions of a save differ in a few
 * blocks, so old versions cost almost nothing.
 *
 *   queue.state 'packed' = the archive's bytes live in the block store and
 *   its ZIP was removed after a rebuild matched its SHA-256.
 *
 * Every reader of an archive file calls ls_archive_ensure_ready() first:
 * a packed archive is rebuilt as its exact ZIP in the queue (verified) and
 * becomes 'ready' again, so restore / PC download / checks stay unchanged.
 * The next housekeeping pass packs it again (cheap: the recipe is kept).
 *
 * Ordering: pack bytes are fsync'd before the rows pointing at them commit;
 * a crash in between leaves dead pack bytes (reclaimed by compaction), never
 * a dangling reference. A ZIP is deleted only after the rebuilt bytes match.
 */

#define LS_BLOCKS_DIR LS_BASE_DIR "/blocks"
#define LS_BLOCK_SIZE 4096
#define LS_PACK_MAX ((int64_t)256 * 1024 * 1024)

static pthread_mutex_t g_ls_blocks_mtx = PTHREAD_MUTEX_INITIALIZER;   /* all block-store work */

static void ls_sha256_final_raw(ls_sha256_t *c, uint8_t out[32]) {
    char hex[65];
    ls_sha256_final_hex(c, hex);
    for (int i = 0; i < 32; i++) {
        unsigned v = 0;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
}

static void ls_pack_path(int64_t pack, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/pack%05lld.bin", LS_BLOCKS_DIR, (long long)pack);
}

/* Offset of the save image's bytes inside a Shallot ZIP (stored entry), 0 if unknown. */
static int64_t ls_zip_image_offset(int fd) {
    int64_t off = 0;
    for (int n = 0; n < 16; n++) {
        uint8_t h[30];
        if (pread(fd, h, 30, (off_t)off) != 30 || memcmp(h, "PK\x03\x04", 4) != 0) return 0;
        unsigned flags = h[6] | (h[7] << 8), method = h[8] | (h[9] << 8);
        uint32_t csize = (uint32_t)h[18] | ((uint32_t)h[19] << 8) | ((uint32_t)h[20] << 16) | ((uint32_t)h[21] << 24);
        unsigned nlen = h[26] | (h[27] << 8), xlen = h[28] | (h[29] << 8);
        char name[512] = {0};
        unsigned rd = nlen < sizeof(name) - 1 ? nlen : (unsigned)sizeof(name) - 1;
        if (pread(fd, name, rd, (off_t)(off + 30)) != (ssize_t)rd) return 0;
        const char *base = strrchr(name, '/');
        base = base ? base + 1 : name;
        int64_t data = off + 30 + nlen + xlen;
        if (strncmp(base, "sdimg_", 6) == 0 && method == 0) return data;
        if (flags & 8) return 0;                       /* sizes after the data: cannot walk on */
        off = data + csize;
    }
    return 0;
}

/* Hashes of every block of fd (allocated, 32 bytes each); whole-file SHA-256 in sha. */
static int ls_blocks_hash_file(int fd, int64_t size, int64_t first, uint8_t **out, int *nout, char sha[65], char *iobuf) {
    int cap = (int)(size / LS_BLOCK_SIZE) + 4, n = 0;
    uint8_t *hs = malloc((size_t)cap * 32);
    if (!hs) return -1;
    ls_sha256_t whole;
    ls_sha256_init(&whole);
    int64_t pos = 0;
    while (pos < size) {
        int64_t len = (pos == 0 && first > 0) ? first : LS_BLOCK_SIZE;
        if (len > size - pos) len = size - pos;
        if (pread(fd, iobuf, (size_t)len, (off_t)pos) != (ssize_t)len) { free(hs); return -1; }
        ls_sha256_update(&whole, iobuf, (size_t)len);
        ls_sha256_t b;
        ls_sha256_init(&b);
        ls_sha256_update(&b, iobuf, (size_t)len);
        if (n >= cap) { free(hs); return -1; }
        ls_sha256_final_raw(&b, hs + (size_t)n * 32);
        n++;
        pos += len;
    }
    ls_sha256_final_hex(&whole, sha);
    *out = hs;
    *nout = n;
    return 0;
}

/* Current pack to append to (a new one once it is full). Caller holds g_ls_mtx. */
static int64_t ls_pack_current_locked(void) {
    sqlite3_stmt *st = NULL;
    int64_t p = 0, have = 0;
    if (sqlite3_prepare_v2(g_ls_db, "SELECT pack, bytes FROM packs ORDER BY pack DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) {
        p = sqlite3_column_int64(st, 0);
        have = sqlite3_column_int64(st, 1);
    }
    sqlite3_finalize(st);
    if (p == 0 || have >= LS_PACK_MAX) {
        p++;
        if (sqlite3_prepare_v2(g_ls_db, "INSERT OR IGNORE INTO packs(pack, bytes, dead) VALUES(?, 0, 0)", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)p);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    return p;
}

static int ls_recipe_exists(const char *queue_id, char sha[65]) {
    int found = 0;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "SELECT sha256 FROM recipes WHERE queue_id = ?", -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, queue_id);
        if (sqlite3_step(st) == SQLITE_ROW) { found = 1; if (sha) ls_col_text(st, 0, sha, 65); }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    return found;
}

/*
 * Rebuilds a recipe's bytes. With out_fd >= 0 they are written there.
 * Returns 0 if the result matches the recipe's SHA-256. Caller holds
 * g_ls_blocks_mtx.
 */
static int ls_blocks_rebuild_locked(const char *queue_id, int out_fd, char *iobuf, char *err, size_t err_sz) {
    char sha[65] = "";
    int64_t size = 0;
    int n = 0;
    uint8_t *hs = NULL;
    int64_t *packs = NULL, *offs = NULL, *lens = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (g_ls_db && sqlite3_prepare_v2(g_ls_db, "SELECT size, sha256, hashes FROM recipes WHERE queue_id = ?", -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, queue_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            size = sqlite3_column_int64(st, 0);
            ls_col_text(st, 1, sha, sizeof(sha));
            n = sqlite3_column_bytes(st, 2) / 32;
            hs = malloc((size_t)n * 32 + 1);
            if (hs) memcpy(hs, sqlite3_column_blob(st, 2), (size_t)n * 32);
        }
    }
    sqlite3_finalize(st);
    int ok = hs != NULL;
    if (ok) {
        packs = malloc(sizeof(int64_t) * (size_t)(n + 1));
        offs = malloc(sizeof(int64_t) * (size_t)(n + 1));
        lens = malloc(sizeof(int64_t) * (size_t)(n + 1));
        ok = packs && offs && lens;
    }
    if (ok && sqlite3_prepare_v2(g_ls_db, "SELECT pack, off, len FROM blocks WHERE hash = ?", -1, &st, NULL) == SQLITE_OK) {
        for (int i = 0; i < n && ok; i++) {
            sqlite3_reset(st);
            sqlite3_bind_blob(st, 1, hs + (size_t)i * 32, 32, SQLITE_STATIC);
            if (sqlite3_step(st) != SQLITE_ROW) { ok = 0; break; }
            packs[i] = sqlite3_column_int64(st, 0);
            offs[i] = sqlite3_column_int64(st, 1);
            lens[i] = sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
    } else ok = 0;
    pthread_mutex_unlock(&g_ls_mtx);
    if (!ok) {
        snprintf(err, err_sz, "block store has no complete copy of %s", queue_id);
        free(hs); free(packs); free(offs); free(lens);
        return -1;
    }
    int cur_pack = -1, pfd = -1;
    int64_t total = 0;
    ls_sha256_t c;
    ls_sha256_init(&c);
    for (int i = 0; i < n && ok; i++) {
        if (packs[i] != cur_pack) {
            if (pfd >= 0) close(pfd);
            char pp[MAX_PATH_LEN];
            ls_pack_path(packs[i], pp, sizeof(pp));
            pfd = open(pp, O_RDONLY);
            cur_pack = (int)packs[i];
            if (pfd < 0) { ok = 0; break; }
        }
        if (lens[i] > BUF_SIZE || pread(pfd, iobuf, (size_t)lens[i], (off_t)offs[i]) != (ssize_t)lens[i]) { ok = 0; break; }
        ls_sha256_update(&c, iobuf, (size_t)lens[i]);
        if (out_fd >= 0 && ls_write_all(out_fd, iobuf, (size_t)lens[i]) < 0) { ok = 0; break; }
        total += lens[i];
    }
    if (pfd >= 0) close(pfd);
    free(hs); free(packs); free(offs); free(lens);
    char got[65];
    ls_sha256_final_hex(&c, got);
    if (!ok || total != size || strcmp(got, sha) != 0) {
        snprintf(err, err_sz, "rebuilt copy of %s does not match its SHA-256", queue_id);
        return -1;
    }
    return 0;
}

/*
 * Moves one 'ready' archive into the block store. 1 = packed, 0 = skipped,
 * -1 = error (the ZIP stays). Caller must not hold g_ls_mtx.
 */
static int ls_blocks_pack(const char *queue_id, char *iobuf, char *err, size_t err_sz) {
    ls_queue_row_t q;
    if (!ls_queue_get(queue_id, &q) || strcmp(q.state, "ready") != 0) return 0;
    char zpath[MAX_PATH_LEN];
    snprintf(zpath, sizeof(zpath), "%s/%s", LS_QUEUE_DIR, q.filename);
    pthread_mutex_lock(&g_ls_blocks_mtx);
    int rc = -1;
    uint8_t *hs = NULL;
    int n = 0, fd = open(zpath, O_RDONLY);
    if (fd < 0) { snprintf(err, err_sz, "archive file missing"); goto out; }
    struct stat st0;
    fstat(fd, &st0);
    if ((int64_t)st0.st_size != q.size) { snprintf(err, err_sz, "archive size differs from its record"); goto out; }
    char sha[65], rsha[65] = "";
    if (ls_recipe_exists(queue_id, rsha)) {
        /* Packed before and rebuilt for a reader: the recipe still holds it. */
        if (strcmp(rsha, q.sha256) != 0 || ls_blocks_rebuild_locked(queue_id, -1, iobuf, err, err_sz) < 0) goto out;
    } else {
        int64_t first = ls_zip_image_offset(fd);
        if (ls_blocks_hash_file(fd, q.size, first, &hs, &n, sha, iobuf) < 0) { snprintf(err, err_sz, "cannot read the archive"); goto out; }
        if (strcmp(sha, q.sha256) != 0) { snprintf(err, err_sz, "archive does not match its SHA-256"); goto out; }
        /* Which blocks are new (also within this archive). */
        char *isnew = calloc((size_t)n + 1, 1);
        if (!isnew) goto out;
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *sel = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "SELECT 1 FROM blocks WHERE hash = ?", -1, &sel, NULL) == SQLITE_OK) {
            for (int i = 0; i < n; i++) {
                int dup = 0;
                sqlite3_reset(sel);
                sqlite3_bind_blob(sel, 1, hs + (size_t)i * 32, 32, SQLITE_STATIC);
                if (sqlite3_step(sel) == SQLITE_ROW) continue;
                for (int j = 0; j < i && !dup; j++) dup = isnew[j] && memcmp(hs + (size_t)j * 32, hs + (size_t)i * 32, 32) == 0;
                if (!dup) isnew[i] = 1;
            }
        }
        sqlite3_finalize(sel);
        int64_t pack = ls_pack_current_locked();
        pthread_mutex_unlock(&g_ls_mtx);
        /* Append the new blocks, fsync, then commit the rows. */
        mkdir(LS_BLOCKS_DIR, 0777);
        char pp[MAX_PATH_LEN];
        ls_pack_path(pack, pp, sizeof(pp));
        int pfd = open(pp, O_WRONLY | O_CREAT | O_APPEND, 0666);
        int64_t *noff = calloc((size_t)n + 1, sizeof(int64_t)), *nlen = calloc((size_t)n + 1, sizeof(int64_t));
        int ok = pfd >= 0 && noff && nlen;
        int64_t poff = ok ? (int64_t)lseek(pfd, 0, SEEK_END) : 0, wrote = 0, pos = 0;
        for (int i = 0; i < n && ok; i++) {
            int64_t len = (i == 0 && first > 0) ? first : LS_BLOCK_SIZE;
            if (len > q.size - pos) len = q.size - pos;
            if (isnew[i]) {
                if (pread(fd, iobuf, (size_t)len, (off_t)pos) != (ssize_t)len || ls_write_all(pfd, iobuf, (size_t)len) < 0) { ok = 0; break; }
                noff[i] = poff; nlen[i] = len;
                poff += len; wrote += len;
            }
            pos += len;
        }
        if (pfd >= 0) { if (ok && fsync(pfd) < 0) ok = 0; close(pfd); }
        if (ok) {
            pthread_mutex_lock(&g_ls_mtx);
            ls_exec_locked("BEGIN");
            sqlite3_stmt *ins = NULL, *ref = NULL, *rec = NULL, *pk = NULL;
            ok = sqlite3_prepare_v2(g_ls_db, "INSERT OR IGNORE INTO blocks(hash, pack, off, len, refs) VALUES(?, ?, ?, ?, 0)", -1, &ins, NULL) == SQLITE_OK &&
                 sqlite3_prepare_v2(g_ls_db, "UPDATE blocks SET refs = refs + 1 WHERE hash = ?", -1, &ref, NULL) == SQLITE_OK &&
                 sqlite3_prepare_v2(g_ls_db, "INSERT OR REPLACE INTO recipes(queue_id, size, sha256, nblocks, hashes, packed_at) VALUES(?, ?, ?, ?, ?, ?)", -1, &rec, NULL) == SQLITE_OK &&
                 sqlite3_prepare_v2(g_ls_db, "UPDATE packs SET bytes = bytes + ? WHERE pack = ?", -1, &pk, NULL) == SQLITE_OK;
            for (int i = 0; i < n && ok; i++) {
                if (isnew[i]) {
                    sqlite3_reset(ins);
                    sqlite3_bind_blob(ins, 1, hs + (size_t)i * 32, 32, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 2, (sqlite3_int64)pack);
                    sqlite3_bind_int64(ins, 3, (sqlite3_int64)noff[i]);
                    sqlite3_bind_int64(ins, 4, (sqlite3_int64)nlen[i]);
                    if (sqlite3_step(ins) != SQLITE_DONE) ok = 0;
                }
                sqlite3_reset(ref);
                sqlite3_bind_blob(ref, 1, hs + (size_t)i * 32, 32, SQLITE_STATIC);
                if (ok && sqlite3_step(ref) != SQLITE_DONE) ok = 0;
            }
            if (ok) {
                ls_bind_text(rec, 1, queue_id);
                sqlite3_bind_int64(rec, 2, (sqlite3_int64)q.size);
                ls_bind_text(rec, 3, q.sha256);
                sqlite3_bind_int(rec, 4, n);
                sqlite3_bind_blob(rec, 5, hs, n * 32, SQLITE_STATIC);
                sqlite3_bind_int64(rec, 6, (sqlite3_int64)time(NULL));
                ok = sqlite3_step(rec) == SQLITE_DONE;
                sqlite3_bind_int64(pk, 1, (sqlite3_int64)wrote);
                sqlite3_bind_int64(pk, 2, (sqlite3_int64)pack);
                if (ok) ok = sqlite3_step(pk) == SQLITE_DONE;
            }
            sqlite3_finalize(ins); sqlite3_finalize(ref); sqlite3_finalize(rec); sqlite3_finalize(pk);
            ls_exec_locked(ok ? "COMMIT" : "ROLLBACK");
            pthread_mutex_unlock(&g_ls_mtx);
        }
        free(isnew); free(noff); free(nlen);
        if (!ok) { snprintf(err, err_sz, "could not write to the block store"); goto out; }
        if (ls_blocks_rebuild_locked(queue_id, -1, iobuf, err, err_sz) < 0) goto out;   /* prove the copy */
    }
    /* The block copy is proven: mark the row, then drop the ZIP. */
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *up = NULL;
    int changed = 0;
    if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET state = 'packed' WHERE id = ? AND state = 'ready'", -1, &up, NULL) == SQLITE_OK) {
        ls_bind_text(up, 1, queue_id);
        changed = sqlite3_step(up) == SQLITE_DONE && sqlite3_changes(g_ls_db) == 1;
    }
    sqlite3_finalize(up);
    pthread_mutex_unlock(&g_ls_mtx);
    if (changed) unlink(zpath);
    rc = changed ? 1 : 0;
out:
    if (fd >= 0) close(fd);
    free(hs);
    pthread_mutex_unlock(&g_ls_blocks_mtx);
    return rc;
}

/* Packed -> its exact ZIP back in the queue ('ready'). 0 = ok. */
static int ls_blocks_unpack(const char *queue_id, char *iobuf, char *err, size_t err_sz) {
    ls_queue_row_t q;
    if (!ls_queue_get(queue_id, &q)) { snprintf(err, err_sz, "unknown archive"); return -1; }
    if (strcmp(q.state, "ready") == 0) return 0;
    if (strcmp(q.state, "packed") != 0) { snprintf(err, err_sz, "archive is %s", q.state); return -1; }
    char zpath[MAX_PATH_LEN], part[MAX_PATH_LEN];
    snprintf(zpath, sizeof(zpath), "%s/%s", LS_QUEUE_DIR, q.filename);
    snprintf(part, sizeof(part), "%s.unpack", zpath);
    pthread_mutex_lock(&g_ls_blocks_mtx);
    int fd = open(part, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    int rc = fd >= 0 ? ls_blocks_rebuild_locked(queue_id, fd, iobuf, err, err_sz) : -1;
    if (fd < 0) snprintf(err, err_sz, "cannot write to the queue");
    if (fd >= 0) { if (rc == 0 && fsync(fd) < 0) rc = -1; close(fd); }
    if (rc == 0 && rename(part, zpath) < 0) rc = -1;
    if (rc < 0) unlink(part);
    if (rc == 0) {
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *up = NULL;
        if (sqlite3_prepare_v2(g_ls_db, "UPDATE queue SET state = 'ready' WHERE id = ? AND state = 'packed'", -1, &up, NULL) == SQLITE_OK) {
            ls_bind_text(up, 1, queue_id);
            sqlite3_step(up);
        }
        sqlite3_finalize(up);
        pthread_mutex_unlock(&g_ls_mtx);
    }
    pthread_mutex_unlock(&g_ls_blocks_mtx);
    return rc;
}

/* Every reader of an archive file calls this first. 0 = the ZIP is in the queue. */
static int ls_archive_ensure_ready(const char *queue_id, char *err, size_t err_sz) {
    ls_queue_row_t q;
    if (!ls_queue_get(queue_id, &q)) { snprintf(err, err_sz, "unknown archive"); return -1; }
    if (strcmp(q.state, "ready") == 0) return 0;
    if (strcmp(q.state, "packed") != 0) { snprintf(err, err_sz, "archive is %s", q.state); return -1; }
    char *iobuf = malloc(BUF_SIZE);
    if (!iobuf) { snprintf(err, err_sz, "out of memory"); return -1; }
    int rc = ls_blocks_unpack(queue_id, iobuf, err, err_sz);
    free(iobuf);
    if (rc == 0) ls_event("ARCHIVE_UNPACKED", q.title_id, "%s rebuilt from the PS5 block store (sha256 verified)", queue_id);
    return rc;
}

/* Drops an archive's recipe (its blocks lose one reference each). */
static void ls_blocks_release(const char *queue_id) {
    pthread_mutex_lock(&g_ls_blocks_mtx);
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    uint8_t *hs = NULL;
    int n = 0;
    if (sqlite3_prepare_v2(g_ls_db, "SELECT hashes FROM recipes WHERE queue_id = ?", -1, &st, NULL) == SQLITE_OK) {
        ls_bind_text(st, 1, queue_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            n = sqlite3_column_bytes(st, 0) / 32;
            hs = malloc((size_t)n * 32 + 1);
            if (hs) memcpy(hs, sqlite3_column_blob(st, 0), (size_t)n * 32);
        }
    }
    sqlite3_finalize(st);
    if (hs) {
        ls_exec_locked("BEGIN");
        sqlite3_stmt *dec = NULL, *dead = NULL, *del = NULL, *rm = NULL;
        sqlite3_prepare_v2(g_ls_db, "UPDATE blocks SET refs = refs - 1 WHERE hash = ?", -1, &dec, NULL);
        sqlite3_prepare_v2(g_ls_db, "UPDATE packs SET dead = dead + (SELECT len FROM blocks WHERE hash = ?1)"
                                    " WHERE pack = (SELECT pack FROM blocks WHERE hash = ?1 AND refs <= 0)", -1, &dead, NULL);
        sqlite3_prepare_v2(g_ls_db, "DELETE FROM blocks WHERE hash = ? AND refs <= 0", -1, &del, NULL);
        for (int i = 0; i < n; i++) {
            const uint8_t *h = hs + (size_t)i * 32;
            sqlite3_reset(dec); sqlite3_bind_blob(dec, 1, h, 32, SQLITE_STATIC); sqlite3_step(dec);
            sqlite3_reset(dead); sqlite3_bind_blob(dead, 1, h, 32, SQLITE_STATIC); sqlite3_step(dead);
            sqlite3_reset(del); sqlite3_bind_blob(del, 1, h, 32, SQLITE_STATIC); sqlite3_step(del);
        }
        if (sqlite3_prepare_v2(g_ls_db, "DELETE FROM recipes WHERE queue_id = ?", -1, &rm, NULL) == SQLITE_OK) {
            ls_bind_text(rm, 1, queue_id);
            sqlite3_step(rm);
        }
        sqlite3_finalize(dec); sqlite3_finalize(dead); sqlite3_finalize(del); sqlite3_finalize(rm);
        ls_exec_locked("COMMIT");
        free(hs);
    }
    pthread_mutex_unlock(&g_ls_mtx);
    pthread_mutex_unlock(&g_ls_blocks_mtx);
}

/*
 * Rewrites packs that are mostly dead (after thinning) so their space comes
 * back: live blocks are copied to the current pack (fsync), rows repointed,
 * then the old pack file is removed. Returns bytes reclaimed.
 */
static int64_t ls_blocks_compact(char *iobuf) {
    int64_t reclaimed = 0;
    pthread_mutex_lock(&g_ls_blocks_mtx);
    for (int round = 0; round < 32; round++) {
        int64_t victim = 0, vbytes = 0;
        pthread_mutex_lock(&g_ls_mtx);
        sqlite3_stmt *st = NULL;
        int64_t cur = ls_pack_current_locked();
        if (sqlite3_prepare_v2(g_ls_db, "SELECT pack, bytes FROM packs WHERE pack != ? AND bytes > 0 AND dead * 2 >= bytes"
                                        " ORDER BY dead DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)cur);
            if (sqlite3_step(st) == SQLITE_ROW) { victim = sqlite3_column_int64(st, 0); vbytes = sqlite3_column_int64(st, 1); }
        }
        sqlite3_finalize(st);
        /* live blocks of the victim */
        int n = 0, cap = 0;
        struct lb { uint8_t h[32]; int64_t off, len; } *lv = NULL;
        if (victim && sqlite3_prepare_v2(g_ls_db, "SELECT hash, off, len FROM blocks WHERE pack = ?", -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, (sqlite3_int64)victim);
            while (sqlite3_step(st) == SQLITE_ROW) {
                if (n == cap) { cap = cap ? cap * 2 : 1024; struct lb *nv = realloc(lv, sizeof(*lv) * (size_t)cap); if (!nv) break; lv = nv; }
                memcpy(lv[n].h, sqlite3_column_blob(st, 0), 32);
                lv[n].off = sqlite3_column_int64(st, 1);
                lv[n].len = sqlite3_column_int64(st, 2);
                n++;
            }
            sqlite3_finalize(st);
        }
        pthread_mutex_unlock(&g_ls_mtx);
        if (!victim) { free(lv); break; }
        char vp[MAX_PATH_LEN], cp[MAX_PATH_LEN];
        ls_pack_path(victim, vp, sizeof(vp));
        ls_pack_path(cur, cp, sizeof(cp));
        int in = open(vp, O_RDONLY), out = open(cp, O_WRONLY | O_CREAT | O_APPEND, 0666);
        int ok = in >= 0 && out >= 0;
        int64_t coff = ok ? (int64_t)lseek(out, 0, SEEK_END) : 0, moved = 0;
        int64_t *newoff = calloc((size_t)n + 1, sizeof(int64_t));
        if (!newoff) ok = 0;
        for (int i = 0; i < n && ok; i++) {
            if (lv[i].len > BUF_SIZE || pread(in, iobuf, (size_t)lv[i].len, (off_t)lv[i].off) != (ssize_t)lv[i].len ||
                ls_write_all(out, iobuf, (size_t)lv[i].len) < 0) { ok = 0; break; }
            newoff[i] = coff; coff += lv[i].len; moved += lv[i].len;
        }
        if (out >= 0) { if (ok && fsync(out) < 0) ok = 0; close(out); }
        if (in >= 0) close(in);
        if (ok) {
            pthread_mutex_lock(&g_ls_mtx);
            ls_exec_locked("BEGIN");
            sqlite3_stmt *up = NULL, *pk = NULL;
            ok = sqlite3_prepare_v2(g_ls_db, "UPDATE blocks SET pack = ?, off = ? WHERE hash = ?", -1, &up, NULL) == SQLITE_OK;
            for (int i = 0; i < n && ok; i++) {
                sqlite3_reset(up);
                sqlite3_bind_int64(up, 1, (sqlite3_int64)cur);
                sqlite3_bind_int64(up, 2, (sqlite3_int64)newoff[i]);
                sqlite3_bind_blob(up, 3, lv[i].h, 32, SQLITE_STATIC);
                if (sqlite3_step(up) != SQLITE_DONE) ok = 0;
            }
            sqlite3_finalize(up);
            if (ok && sqlite3_prepare_v2(g_ls_db, "UPDATE packs SET bytes = bytes + ? WHERE pack = ?", -1, &pk, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(pk, 1, (sqlite3_int64)moved);
                sqlite3_bind_int64(pk, 2, (sqlite3_int64)cur);
                sqlite3_step(pk);
            }
            sqlite3_finalize(pk);
            if (ok && sqlite3_prepare_v2(g_ls_db, "DELETE FROM packs WHERE pack = ?", -1, &pk, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(pk, 1, (sqlite3_int64)victim);
                sqlite3_step(pk);
            }
            sqlite3_finalize(pk);
            ls_exec_locked(ok ? "COMMIT" : "ROLLBACK");
            pthread_mutex_unlock(&g_ls_mtx);
        }
        free(newoff);
        free(lv);
        if (!ok) break;
        unlink(vp);
        reclaimed += vbytes - moved;
    }
    pthread_mutex_unlock(&g_ls_blocks_mtx);
    return reclaimed;
}

/* ── Thinning (PS5-only mode) ─────────────────────────────────────── */

/* Bucket of a version by age: 0 = keep (young), else a day / week key. */
static void ls_thin_bucket(int64_t ts, int64_t now, const ls_config_t *cfg, char *out, size_t out_sz) {
    int64_t age = now - ts;
    time_t t = (time_t)ts;
    struct tm tm;
    localtime_r(&t, &tm);
    if (age < (int64_t)cfg->ps5_keep_all_days * 86400) { out[0] = 0; return; }
    if (age < (int64_t)cfg->ps5_daily_days * 86400) { strftime(out, out_sz, "d%Y-%m-%d", &tm); return; }
    if (cfg->ps5_weekly) { strftime(out, out_sz, "w%G-%V", &tm); return; }
    snprintf(out, out_sz, "drop");
}

typedef struct { int64_t ts; char key[200]; char group[400]; int keep; } ls_thin_item_t;

static int ls_thin_cmp(const void *a, const void *b) {
    const ls_thin_item_t *x = a, *y = b;
    int g = strcmp(x->group, y->group);
    if (g) return g;
    return x->ts < y->ts ? 1 : x->ts > y->ts ? -1 : 0;   /* newest first */
}

/* Marks keep on items: newest of each group, young ones, newest per bucket. */
static void ls_thin_mark(ls_thin_item_t *it, int n, int64_t now, const ls_config_t *cfg) {
    qsort(it, (size_t)n, sizeof(*it), ls_thin_cmp);
    char seen[64][24];
    int nseen = 0;
    for (int i = 0; i < n; i++) {
        if (i == 0 || strcmp(it[i].group, it[i - 1].group) != 0) {
            nseen = 0;
            it[i].keep = 1;
            char b[24];
            ls_thin_bucket(it[i].ts, now, cfg, b, sizeof(b));
            if (b[0] && nseen < 64) snprintf(seen[nseen++], sizeof(seen[0]), "%s", b);
            continue;
        }
        char b[24];
        ls_thin_bucket(it[i].ts, now, cfg, b, sizeof(b));
        if (!b[0]) { it[i].keep = 1; continue; }
        if (strcmp(b, "drop") == 0) continue;
        int dup = 0;
        for (int k = 0; k < nseen && !dup; k++) dup = strcmp(seen[k], b) == 0;
        if (!dup) {
            it[i].keep = 1;
            if (nseen < 64) snprintf(seen[nseen++], sizeof(seen[0]), "%s", b);
            else memmove(seen[0], seen[1], sizeof(seen[0]) * 63), snprintf(seen[63], sizeof(seen[0]), "%s", b);
        }
    }
}

/* Removes one archive from the PS5 (block copy and/or ZIP); row -> 'pruned'. */
static void ls_archive_drop(const ls_queue_row_t *q) {
    if (ls_recipe_exists(q->id, NULL)) ls_blocks_release(q->id);
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s", LS_QUEUE_DIR, q->filename);
    unlink(path);
    ls_queue_mark(q->id, "pruned");
}

/*
 * PS5-only mode: snapshots are thinned per (user, game, kind, save object)
 * with the same rules as the PC (keep all young ones, then one per day,
 * then one per week); archives no kept snapshot needs are dropped, except
 * the newest of each save file and young ones. Snapshots that an open
 * "older version" offer points at are kept. Returns archives dropped.
 */
static int ls_ps5_thin(int64_t now, int *snaps_dropped) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (snaps_dropped) *snaps_dropped = 0;
    int ns = 0, cap = 0;
    ls_thin_item_t *sn = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(g_ls_db, "SELECT snap_id, created_at, user_id, title_id, kind, object_id FROM snapshots", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (ns == cap) { cap = cap ? cap * 2 : 256; ls_thin_item_t *nv = realloc(sn, sizeof(*sn) * (size_t)cap); if (!nv) break; sn = nv; }
            ls_thin_item_t *x = &sn[ns++];
            memset(x, 0, sizeof(*x));
            snprintf(x->key, sizeof(x->key), "%lld", (long long)sqlite3_column_int64(st, 0));
            x->ts = sqlite3_column_int64(st, 1);
            snprintf(x->group, sizeof(x->group), "%s|%s|%s|%s", (const char *)sqlite3_column_text(st, 2) ?: "",
                     (const char *)sqlite3_column_text(st, 3) ?: "", (const char *)sqlite3_column_text(st, 4) ?: "",
                     (const char *)sqlite3_column_text(st, 5) ?: "");
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    if (ns) ls_thin_mark(sn, ns, now, &cfg);
    /* keep what open recovery offers point at */
    pthread_mutex_lock(&g_ls_mtx);
    for (int i = 0; i < ns; i++) {
        if (sn[i].keep) continue;
        if (ls_db_int_locked("SELECT COUNT(*) FROM recovery WHERE state = 'open' AND target_snap = CAST(? AS INTEGER) AND ? = ? AND 0 * ? = 0",
                             sn[i].key, "x", "x", 0) > 0)
            sn[i].keep = 1;
    }
    /* drop the thinned snapshots */
    ls_exec_locked("BEGIN");
    sqlite3_stmt *d1 = NULL, *d2 = NULL;
    sqlite3_prepare_v2(g_ls_db, "DELETE FROM snapshot_files WHERE snap_id = CAST(? AS INTEGER)", -1, &d1, NULL);
    sqlite3_prepare_v2(g_ls_db, "DELETE FROM snapshots WHERE snap_id = CAST(? AS INTEGER)", -1, &d2, NULL);
    int dropped_snaps = 0;
    for (int i = 0; i < ns; i++) {
        if (sn[i].keep) continue;
        sqlite3_reset(d1); ls_bind_text(d1, 1, sn[i].key); sqlite3_step(d1);
        sqlite3_reset(d2); ls_bind_text(d2, 1, sn[i].key); sqlite3_step(d2);
        dropped_snaps++;
    }
    sqlite3_finalize(d1); sqlite3_finalize(d2);
    ls_exec_locked("COMMIT");
    pthread_mutex_unlock(&g_ls_mtx);
    free(sn);
    if (snaps_dropped) *snaps_dropped = dropped_snaps;

    /* archives on the PS5: needed by a remaining snapshot, newest of their save, young, or (no snapshot) thinned per save */
    int na = 0, acap = 0;
    ls_thin_item_t *ar = NULL;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db,
            "SELECT q.id, q.created_at, q.user_id, q.title_id, q.save_name"
            " FROM queue q WHERE q.state IN ('ready', 'packed')", -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (na == acap) { acap = acap ? acap * 2 : 256; ls_thin_item_t *nv = realloc(ar, sizeof(*ar) * (size_t)acap); if (!nv) break; ar = nv; }
            ls_thin_item_t *x = &ar[na++];
            memset(x, 0, sizeof(*x));
            snprintf(x->key, sizeof(x->key), "%s", (const char *)sqlite3_column_text(st, 0) ?: "");
            x->ts = sqlite3_column_int64(st, 1);
            snprintf(x->group, sizeof(x->group), "%s|%s|%s", (const char *)sqlite3_column_text(st, 2) ?: "",
                     (const char *)sqlite3_column_text(st, 3) ?: "", (const char *)sqlite3_column_text(st, 4) ?: "");
        }
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    int dropped = 0;
    if (na) {
        /* archives a remaining snapshot needs stay; the rest are thinned per save file */
        ls_thin_mark(ar, na, now, &cfg);           /* also marks the newest of each save */
        for (int i = 0; i < na; i++) {
            if (ar[i].keep) continue;
            pthread_mutex_lock(&g_ls_mtx);
            int64_t refs = ls_db_int_locked("SELECT COUNT(*) FROM snapshot_files f JOIN queue q ON q.gen_id = f.gen_id"
                                            " WHERE q.id = ? AND q.gen_id > 0 AND ? = ? AND 0 * ? = 0", ar[i].key, "x", "x", 0);
            pthread_mutex_unlock(&g_ls_mtx);
            if (refs > 0) continue;
            ls_queue_row_t q;
            if (!ls_queue_get(ar[i].key, &q)) continue;
            ls_archive_drop(&q);
            dropped++;
        }
    }
    free(ar);
    return dropped;
}

/* Status numbers for the UI: archives on the PS5, bytes as ZIPs, pack bytes, logical bytes. */
static void ls_ps5_store_json(char *out, size_t out_sz) {
    int64_t zips = 0, zbytes = 0, packed = 0, plogical = 0, pbytes = 0, pdead = 0;
    pthread_mutex_lock(&g_ls_mtx);
    zips = ls_db_int_locked("SELECT COUNT(*) FROM queue WHERE state = 'ready' AND ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    zbytes = ls_db_int_locked("SELECT COALESCE(SUM(size), 0) FROM queue WHERE state = 'ready' AND ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    packed = ls_db_int_locked("SELECT COUNT(*) FROM queue WHERE state = 'packed' AND ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    plogical = ls_db_int_locked("SELECT COALESCE(SUM(size), 0) FROM queue WHERE state = 'packed' AND ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    pbytes = ls_db_int_locked("SELECT COALESCE(SUM(bytes), 0) FROM packs WHERE ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    pdead = ls_db_int_locked("SELECT COALESCE(SUM(dead), 0) FROM packs WHERE ? = ? AND ? IS NOT NULL AND 0 * ? = 0", "x", "x", "x", 0);
    pthread_mutex_unlock(&g_ls_mtx);
    ls_config_t cfg;
    ls_load_config(&cfg);
    snprintf(out, out_sz,
             "{\"mode\":\"%s\",\"zip_archives\":%lld,\"zip_bytes\":%lld,\"packed_archives\":%lld,\"packed_logical_bytes\":%lld,"
             "\"pack_bytes\":%lld,\"pack_dead_bytes\":%lld,\"last_housekeeping\":%lld}",
             ls_storage_mode_name(&cfg), (long long)zips, (long long)zbytes, (long long)packed, (long long)plogical,
             (long long)pbytes, (long long)pdead, (long long)ls_meta_get_int("last_housekeeping", 0));
}

/*
 * Housekeeping in PS5-only mode (worker job, console idle): thin old
 * versions, pack archives that are not the newest of their save, compact
 * mostly-dead packs. limit = archives packed per run.
 */
static void ls_ps5_housekeeping(int limit) {
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.storage_ps5) return;
    int64_t now = (int64_t)time(NULL);
    int snaps = 0;
    int dropped = ls_ps5_thin(now, &snaps);
    char *iobuf = malloc(BUF_SIZE);
    if (!iobuf) return;
    /* candidates: ready, not the newest of their save file, older than 10 min */
    char (*ids)[200] = malloc(sizeof(*ids) * (size_t)(limit > 0 ? limit : 1));
    int n = 0;
    pthread_mutex_lock(&g_ls_mtx);
    sqlite3_stmt *st = NULL;
    if (ids && sqlite3_prepare_v2(g_ls_db,
            "SELECT q.id FROM queue q WHERE q.state = 'ready' AND q.created_at < ?"
            " AND EXISTS (SELECT 1 FROM queue n WHERE n.user_id = q.user_id AND n.title_id = q.title_id"
            "             AND n.save_name = q.save_name AND n.state IN ('ready','packed') AND n.created_at > q.created_at)"
            " ORDER BY q.created_at LIMIT ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, (sqlite3_int64)(now - 600));
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW && n < limit)
            snprintf(ids[n++], sizeof(ids[0]), "%s", (const char *)sqlite3_column_text(st, 0) ?: "");
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    int packed = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        if (ls_power_down()) break;
        char err[200] = "";
        int r = ls_blocks_pack(ids[i], iobuf, err, sizeof(err));
        if (r == 1) packed++;
        else if (r < 0) { failed++; ls_event("PACK_FAILED", NULL, "%s: %s (the ZIP is kept)", ids[i], err); }
    }
    free(ids);
    int64_t reclaimed = ls_blocks_compact(iobuf);
    free(iobuf);
    ls_meta_set_int("last_housekeeping", now);
    if (dropped || snaps || packed || failed || reclaimed)
        ls_event("PS5_HOUSEKEEPING", NULL, "thinned %d old version(s) and %d restore point(s); deduplicated %d archive(s)%s; %lld MB reclaimed",
                 dropped, snaps, packed, failed ? " (some failed)" : "", (long long)(reclaimed / (1024 * 1024)));
}
