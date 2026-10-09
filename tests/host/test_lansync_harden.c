/*
 * Host tests for the duplicate-instance and title-ID hardening:
 *  - a second process cannot take the queue/state lock (ls_init -> -2);
 *  - staging file names carry pid + counter;
 *  - a game close also queues titles whose saves were written under another
 *    title ID during the session, and only those.
 */
#include "host_worker_stubs.h"
#include <sys/wait.h>

#define APP_DIR   LS_USER_HOME "/10000000/savedata_prospero/PPSA24701"
#define OTHER_DIR LS_USER_HOME "/10000000/savedata_prospero/PPSA30000"
#define OLD_DIR   LS_USER_HOME "/10000000/savedata_prospero/PPSA40000"

static int pending_for(int kind, const char *title) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next)
        if (j->kind == kind && strcmp(j->title_id, title) == 0) n++;
    pthread_mutex_unlock(&g_ls_job_mtx);
    return n;
}
static void drop_jobs(void) {
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
}
static void set_mtime(const char *path, time_t t) {
    struct timespec ts[2] = { { t, 0 }, { t, 0 } };
    utimensat(AT_FDCWD, path, ts, 0);
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();

    /* 1. Instance lock: parent owns it, a forked second process is refused. */
    CHECK(ls_init() == 0, "first process initializes");
    CHECK(g_ls_ready, "ready");
    pid_t pid = fork();
    if (pid == 0) {
        g_ls_ready = 0;
        g_ls_db = NULL;              /* do not touch the parent's handle */
        int rc = ls_init();
        _exit(rc == -2 && !g_ls_ready ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "second process refused (-2)");

    char *iobuf = malloc(BUF_SIZE);

    /* 2. Cross-title-ID detection on game close. */
    mkdirs(APP_DIR);                  /* the app keeps nothing under its own ID */
    mkdirs(OTHER_DIR);
    mkdirs(OLD_DIR);
    time_t started = time(NULL) - 60;
    write_file(OTHER_DIR "/SAVE", "written-during-session");
    write_file(OLD_DIR "/SAVE", "untouched");
    set_mtime(OLD_DIR "/SAVE", started - 3600);

    ls_job_t job = { LS_JOB_TITLE_CLOSED, "PPSA24701", 0, 0, (int64_t)started, "", NULL };
    ls_run_job(&job, iobuf);
    CHECK(pending_for(LS_JOB_TITLE_CLOSED, "PPSA30000") == 1, "title written during session queued");
    CHECK(pending_for(LS_JOB_TITLE_CLOSED, "PPSA40000") == 0, "untouched title not queued");
    CHECK(ev_count_code("SAVE_TITLE_REDIRECT") == 1, "redirect logged");
    CHECK(pending_for(LS_JOB_TITLE_FOLLOWUP, "PPSA24701") == 1, "app still gets its follow-up");

    /* Without a start time (Shallot loaded mid-game) nothing extra is queued. */
    drop_jobs();
    ls_job_t nojob = { LS_JOB_TITLE_CLOSED, "PPSA24701", 0, 0, 0, "", NULL };
    ls_run_job(&nojob, iobuf);
    CHECK(pending_for(LS_JOB_TITLE_CLOSED, "PPSA30000") == 0, "no since => no cross-title scan");

    /* Running the redirected job protects the other title's save (first protection). */
    drop_jobs();
    ls_job_t other = { LS_JOB_TITLE_CLOSED, "PPSA30000", 0, 0, 0, "", NULL };
    ls_run_job(&other, iobuf);
    sqlite3_stmt *st = NULL;
    int64_t n = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*) FROM queue WHERE title_id='PPSA30000'", -1, &st, NULL) == SQLITE_OK &&
        sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    CHECK(n == 1, "redirected title protected (%lld)", (long long)n);

    /* 3. Staging names carry pid and a counter. */
    char pat[64];
    snprintf(pat, sizeof(pat), "%%_p%d-%%_enc.zip", (int)getpid());
    n = -1;
    pthread_mutex_lock(&g_ls_mtx);
    if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*) FROM events WHERE code='BACKUP_PACKAGED' AND detail LIKE ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, pat, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    pthread_mutex_unlock(&g_ls_mtx);
    CHECK(n == 1, "stage name has pid+counter (%lld)", (long long)n);

    /* 4. pid in lifecycle events. */
    ls_on_game_start("PPSA24701", 0x18);
    CHECK(ev_count_code("GAME_START") == 1, "game start logged");
    {
        sqlite3_stmt *e = NULL; int named = 0;
        pthread_mutex_lock(&g_ls_mtx);
        if (sqlite3_prepare_v2(g_ls_db, "SELECT COUNT(*) FROM events WHERE code='GAME_START' AND detail LIKE 'Test Launcher app 0x18 pid=%'",
                               -1, &e, NULL) == SQLITE_OK && sqlite3_step(e) == SQLITE_ROW) named = sqlite3_column_int(e, 0);
        sqlite3_finalize(e);
        pthread_mutex_unlock(&g_ls_mtx);
        CHECK(named == 1, "game name in GAME_START");
    }

    drop_jobs();
    free(iobuf);
    return host_finish();
}
