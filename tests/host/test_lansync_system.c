/*
 * Host tests for the system-state probe (stage O): rest-mode detection from
 * a wall-clock jump, state-change and shutdown logging, heartbeat, and the
 * previous-session classification.
 */
#include "host_worker_stubs.h"

static ls_sys_sample_t S(int state, int sd, int sb) { ls_sys_sample_t s = { state, sd, sb }; return s; }
static int queued(int kind) {
    int n = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next) if (j->kind == kind) n++;
    pthread_mutex_unlock(&g_ls_job_mtx);
    return n;
}

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    CHECK(host_open_state_db(), "db open");
    g_ls_worker_started = 1;
    char out[300];

    /* Previous-session classification. */
    ls_sys_classify_previous(0, 0, 1000, 5000, 0, 0, NULL, out, sizeof(out));
    CHECK(strstr(out, "no previous session") != NULL, "first run (%s)", out);
    ls_sys_classify_previous(4900, 1000, 1000, 5000, 0, 4900, NULL, out, sizeof(out));
    CHECK(strstr(out, "without a console reboot") && strstr(out, "100 s ago"), "payload restart (%s)", out);
    ls_sys_classify_previous(4900, 1000, 6000, 6100, 4890, 4896, "idle", out, sizeof(out));
    CHECK(strstr(out, "console restarted") && strstr(out, "1100 s before this boot") && strstr(out, "alive 6 s after") && strstr(out, "all work had finished"),
          "reboot after a signalled shutdown (%s)", out);

    /* Start: report + markers. */
    ls_meta_set_int("sys_heartbeat", (int64_t)time(NULL) - 50);
    ls_meta_set_int("sys_boottime", 1000);
    g_test_boottime = 1000;
    ls_sys_probe_start();
    CHECK(ev_count_code("SESSION_PREVIOUS") == 1 && strstr(g_ls_sys.previous_session, "without a console reboot"),
          "previous session reported (%s)", g_ls_sys.previous_session);

    /* Normal ticks: nothing logged after the first sample. */
    int64_t rt = 100000, mono = 5000000;
    ls_sys_step(&(ls_sys_sample_t){ 3, 0, 1 }, rt, mono);
    for (int i = 0; i < 5; i++) { rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 3, 0, 1 }, rt, mono); }
    CHECK(ev_count_code("SYSTEM_STATE_CHANGE") == 0 && ev_count_code("SYSTEM_RESUMED") == 0, "quiet while nothing changes");

    /* Rest mode: wall clock +600 s, monotonic +2 s. */
    rt += 600; mono += 2000;
    ls_sys_step(&(ls_sys_sample_t){ 3, 0, 1 }, rt, mono);
    CHECK(ev_count_code("SYSTEM_RESUMED") == 1 && g_ls_sys.resumes == 1 && g_ls_sys.last_suspend_s >= 590,
          "resume detected (%lld s)", (long long)g_ls_sys.last_suspend_s);
    CHECK(queued(LS_JOB_STARTUP) == 1, "reconciliation queued after resume");

    /* State change and shutdown signal. */
    rt += 2; mono += 2000;
    ls_sys_step(&(ls_sys_sample_t){ 5, 0, 1 }, rt, mono);
    CHECK(ev_count_code("SYSTEM_STATE_CHANGE") == 1, "state change logged");
    rt += 2; mono += 2000;
    ls_sys_step(&(ls_sys_sample_t){ 5, 1, 1 }, rt, mono);
    CHECK(ev_count_code("SHUTDOWN_REQUEST_DETECTED") == 1 && ls_meta_get_int("sys_shutdown_seen", 0) == rt, "shutdown detected");
    rt += 2; mono += 2000;
    ls_sys_step(&(ls_sys_sample_t){ 5, 1, 1 }, rt, mono);
    CHECK(ls_meta_get_int("sys_heartbeat", 0) == rt, "heartbeat every tick while shutting down");

    /* The 13.60 sequence: idle flag is an error code, 1 = in progress.
     * Rest: signal, 1000->500, ->300, frozen, resume. */
    const int IDLE = (int)0x80020000;
    g_ls_sys.shutdown_since = 0;
    ls_meta_set_int("sys_shutdown_seen", 0);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, IDLE, 1 }, rt, mono);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, 1, 1 }, rt, mono);
    CHECK(ev_count_code("SHUTDOWN_REQUEST_DETECTED") == 2 && g_ls_sys.shutdown_since == rt, "signal from error-code idle value");
    int64_t sig = rt;
    rt += 6; mono += 6000; ls_sys_step(&(ls_sys_sample_t){ 500, IDLE, 1 }, rt, mono);
    CHECK(g_ls_sys.shutdown_since == sig && ls_meta_get_int("sys_heartbeat", 0) == rt, "still in transition at state 500");
    rt += 12; mono += 12000; ls_sys_step(&(ls_sys_sample_t){ 300, IDLE, 1 }, rt, mono);
    rt += 170; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, IDLE, 1 }, rt, mono);
    CHECK(g_ls_sys.resumes == 2 && g_ls_sys.shutdown_since == 0 && ls_meta_get_int("sys_shutdown_seen", -1) == 0,
          "resume clears the rest signal");
    /* Cancelled signal: flag drops while still running. */
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, 1, 1 }, rt, mono);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, IDLE, 1 }, rt, mono);
    CHECK(g_ls_sys.shutdown_since == 0, "cancelled signal cleared");
    /* Shutdown: signal, 1000->100; heartbeat keeps the last alive time. */
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, 1, 1 }, rt, mono);
    sig = rt;
    rt += 6; mono += 6000; ls_sys_step(&(ls_sys_sample_t){ 100, IDLE, 1 }, rt, mono);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 100, IDLE, 1 }, rt, mono);
    CHECK(g_ls_sys.shutdown_since == sig && ls_meta_get_int("sys_heartbeat", 0) == rt, "heartbeat continues at state 100");
    ls_sys_classify_previous(ls_meta_get_int("sys_heartbeat", 0), 1000, 2000, rt + 60,
                             ls_meta_get_int("sys_shutdown_seen", 0), ls_meta_get_int("sys_heartbeat", 0), "manual PPSA01234",
                             out, sizeof(out));
    CHECK(strstr(out, "alive 8 s after") && strstr(out, "still working: manual PPSA01234"),
          "alive time after the signal (%s)", out);

    /* ── Stage P1: reaction to the signal ── */
    CHECK(g_ls_power == LS_PWR_SHUTDOWN, "phase shutdown after state 100 (%s)", ls_power_name(g_ls_power));
    char *ex = ls_meta_get_text("sys_exit");
    CHECK(ex && strcmp(ex, "idle") == 0 && ev_count_code("POWER_DOWN_READY") >= 1, "exit marker idle (%s)", ex ? ex : "-");
    free(ex);
    /* Resume from a rest-like freeze clears it. */
    rt += 300; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, IDLE, 1 }, rt, mono);
    CHECK(g_ls_power == LS_PWR_NONE, "phase cleared");

    /* Queue: a close job delayed 60 s, a startup job, a follow-up. */
    pthread_mutex_lock(&g_ls_job_mtx);
    while (g_ls_jobs) { ls_job_t *j = g_ls_jobs; g_ls_jobs = j->next; free(j); }
    pthread_mutex_unlock(&g_ls_job_mtx);
    ls_worker_enqueue(LS_JOB_TITLE_CLOSED, "PPSA01234", 0, 60);
    ls_worker_enqueue(LS_JOB_STARTUP, "", 0, 0);
    ls_worker_enqueue(LS_JOB_TITLE_FOLLOWUP, "PPSA01234", 0, 180);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, 1, 1 }, rt, mono);
    g_test_power_now = rt;
    CHECK(g_ls_power == LS_PWR_SIGNALLED && ev_count_code("POWER_PHASE") >= 3, "signalled");
    int64_t nowms = ls_io_now_ms(NULL);
    int close_now = 0, startup_ok = 1, follow_ok = 1;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next) {
        if (j->kind == LS_JOB_TITLE_CLOSED) close_now = j->not_before_ms <= nowms && ls_power_job_allowed(j->kind, rt);
        if (j->kind == LS_JOB_STARTUP) startup_ok = ls_power_job_allowed(j->kind, rt);
        if (j->kind == LS_JOB_TITLE_FOLLOWUP) follow_ok = ls_power_job_allowed(j->kind, rt);
    }
    pthread_mutex_unlock(&g_ls_job_mtx);
    CHECK(close_now && !startup_ok && !follow_ok, "close job pulled forward; startup and follow-up held");
    char rerr[200] = "";
    CHECK(ls_restore_claim("12229390", "PPSA01234", rerr, sizeof(rerr)) < 0 && strstr(rerr, "powering off"),
          "restore refused while powering off (%s)", rerr);
    ls_sys_step(&(ls_sys_sample_t){ 1000, 1, 1 }, rt, mono);
    ex = ls_meta_get_text("sys_exit");
    CHECK(ex && strstr(ex, "1 backup job(s) queued"), "exit marker: queued backup (%s)", ex ? ex : "-");
    free(ex);
    /* Budget: from 4 s after the signal nothing new may start. */
    CHECK(!ls_power_out_of_time(rt + 3) && ls_power_out_of_time(rt + 4) && !ls_power_job_allowed(LS_JOB_TITLE_CLOSED, rt + 4),
          "4 s budget");
    g_test_power_now = rt + 4;
    rt += 4; mono += 4000; ls_sys_step(&(ls_sys_sample_t){ 1000, 1, 1 }, rt, mono);
    ex = ls_meta_get_text("sys_exit");
    CHECK(ex && strcmp(ex, "idle") == 0, "out of budget: nothing left to run counts as idle (%s)", ex ? ex : "-");
    free(ex);
    /* Rest: worker released. */
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 500, IDLE, 1 }, rt, mono);
    CHECK(g_ls_power == LS_PWR_REST && ls_power_job_allowed(LS_JOB_STARTUP, rt), "rest releases held jobs");
    g_test_power_now = 0;
    /* Wake seen only as a state change (13.60, short rest: no clock jump). */
    int before = g_ls_sys.resumes, sr = ev_count_code("SYSTEM_RESUMED");
    rt += 10; mono += 10000; ls_sys_step(&(ls_sys_sample_t){ 300, IDLE, 1 }, rt, mono);
    rt += 45; mono += 45000; ls_sys_step(&(ls_sys_sample_t){ 1000, IDLE, 1 }, rt, mono);
    CHECK(g_ls_sys.resumes == before + 1 && ev_count_code("SYSTEM_RESUMED") == sr + 1 && g_ls_power == LS_PWR_NONE &&
          g_ls_sys.shutdown_since == 0, "wake from the state change");
    CHECK(g_ls_http_reopen == 1, "wake asks for a new listening socket");
    /* Clock jump while the state still reads 300, then 300->1000: one wake. */
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 500, IDLE, 1 }, rt, mono);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 300, IDLE, 1 }, rt, mono);
    rt += 600; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 300, IDLE, 1 }, rt, mono);
    rt += 2; mono += 2000; ls_sys_step(&(ls_sys_sample_t){ 1000, IDLE, 1 }, rt, mono);
    CHECK(g_ls_sys.resumes == before + 2, "one wake per rest (%d)", g_ls_sys.resumes - before);
    /* Rest shifts kern.boottime forward: not a reboot while it is still before the last heartbeat. */
    ls_sys_classify_previous(5000, 1000, 1026, 5100, 0, 5000, NULL, out, sizeof(out));
    CHECK(strstr(out, "without a console reboot"), "boottime shifted by rest (%s)", out);
    (void)S;

    /* JSON. */
    size_t cap = 4096, pos = 0;
    char *buf = malloc(cap); buf[0] = 0;
    ls_sys_json(&buf, &cap, &pos);
    CHECK(strstr(buf, "\"resumes\":5") && strstr(buf, "\"power_phase\":\"none\"") && strstr(buf, "\"shutdown_in_progress\":0") && strstr(buf, "\"shutdown_since\":0") && strstr(buf, "resumed from rest"),
          "probe JSON (%.200s)", buf);
    free(buf);
    return host_finish();
}
