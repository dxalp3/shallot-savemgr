/*
 * LAN save sync — system-state observation (stage O, probe).
 * Included from main.c after lansync_recovery.inc.c.
 *
 * Observation (stage O) plus one reaction (stage P1, config
 * power_last_chance): on a rest/shutdown/reboot signal the worker gets the
 * power phase (see ls_power_set in lansync_worker.inc.c). Nothing here
 * changes the console's state. The questions it answered on 13.60:
 *   - Does Shallot notice rest mode? A suspended process is frozen: on resume
 *     the wall clock has jumped while CLOCK_MONOTONIC barely moved.
 *     (On resume the normal startup reconciliation is queued.)
 *   - Do the system-state calls report anything for a payload?
 *     sceSystemStateMgrGetCurrentState / IsShellUIShutdownInProgress /
 *     IsStandbyModeEnabled are sampled every 2 s and every change is logged.
 *   - Are suspend notifications delivered to a payload at all?
 *     (sceSystemServiceEnableSuspendNotification + ReceiveEvent, logged.)
 *   - How did the previous session end, and how long did Shallot stay alive
 *     after a shutdown was signalled? A heartbeat (every 30 s, every 2 s
 *     while a shutdown is in progress) and kern.boottime are compared at the
 *     next start (event SESSION_PREVIOUS).
 */

#ifndef LS_HOST_TEST
#include <sys/sysctl.h>
int sceSystemStateMgrGetCurrentState(void);
int sceSystemStateMgrIsShellUIShutdownInProgress(void);
int sceSystemStateMgrIsStandbyModeEnabled(void);
int sceSystemServiceEnableSuspendNotification(void);
int sceSystemServiceReceiveEvent(void *event);

static int64_t ls_sys_boottime(void) {
    struct timeval tv;
    size_t len = sizeof(tv);
    memset(&tv, 0, sizeof(tv));
    if (sysctlbyname("kern.boottime", &tv, &len, NULL, 0) < 0) return 0;
    return (int64_t)tv.tv_sec;
}
#endif

typedef struct {
    int state, shutdown, standby;
} ls_sys_sample_t;

/* Observed on 13.60 (owner's console): 1000 running; rest: 1000 -> 500 (~7 s
 * after the signal) -> 300 (~19 s) -> frozen; shutdown/reboot: 1000 -> 100 (~6 s). */
#define LS_SYS_STATE_RUNNING 1000
#define LS_SYS_STATE_REST_ENTER 500
#define LS_SYS_STATE_REST 300
#define LS_SYS_STATE_SHUTDOWN 100

#define LS_SYS_HIST 48
typedef struct {
    int have_last;
    ls_sys_sample_t last;
    int64_t last_rt, last_mono_ms;
    int64_t last_heartbeat;
    int resumes;
    int64_t last_resume_at, last_suspend_s;
    int64_t shutdown_since;
    int64_t in_rest;              /* time the state went to 500/300 (0 = not in rest) */
    int suspend_notify_rc, event_rc, events_received;
    char previous_session[400];
    char exit_state[120];         /* last value written to meta sys_exit */
    struct { int64_t ts; char text[120]; } hist[LS_SYS_HIST];
    int nhist;
} ls_sys_t;

/* Set on every wake; main.c then re-creates the web server's listening
 * socket (owner's console: unreachable after rest, accept() never recovered). */
static volatile int g_ls_http_reopen = 0;

static pthread_mutex_t g_ls_sys_mtx = PTHREAD_MUTEX_INITIALIZER;
static ls_sys_t g_ls_sys;
static int g_ls_sys_started = 0;

static void ls_sys_hist_locked(int64_t ts, const char *fmt, ...) {
    if (g_ls_sys.nhist == LS_SYS_HIST) {
        memmove(&g_ls_sys.hist[0], &g_ls_sys.hist[1], sizeof(g_ls_sys.hist[0]) * (LS_SYS_HIST - 1));
        g_ls_sys.nhist--;
    }
    va_list ap;
    va_start(ap, fmt);
    g_ls_sys.hist[g_ls_sys.nhist].ts = ts;
    vsnprintf(g_ls_sys.hist[g_ls_sys.nhist].text, sizeof(g_ls_sys.hist[0].text), fmt, ap);
    va_end(ap);
    g_ls_sys.nhist++;
}

/*
 * How the previous session ended, from persisted markers. Pure function.
 * hb: last heartbeat (0 = none), boot_prev / boot_now: kern.boottime then and
 * now (0 = unknown), sd: time a shutdown was first seen (0 = never), alive:
 * last heartbeat after that, exit: last work state written while powering
 * down (NULL/"" = none).
 */
static void ls_sys_classify_previous(int64_t hb, int64_t boot_prev, int64_t boot_now, int64_t now, int64_t sd,
                                     int64_t alive, const char *exit, char *out, size_t out_sz) {
    if (!hb) { snprintf(out, out_sz, "no previous session recorded"); return; }
    /* kern.boottime moves forward by the time spent in rest mode, so "changed"
     * is no proof of a reboot; a kernel booted after Shallot's last sign of
     * life is. */
    (void)boot_prev;
    int rebooted = boot_now && boot_now > hb;
    char extra[260] = "";
    if (sd) snprintf(extra, sizeof(extra), "; a shutdown was signalled and Shallot stayed alive %lld s after it%s%s",
                     (long long)(alive > sd ? alive - sd : 0),
                     exit && exit[0] ? (strcmp(exit, "idle") == 0 ? "; all work had finished" : "; still working: ") : "",
                     exit && exit[0] && strcmp(exit, "idle") != 0 ? exit : "");
    if (!boot_now)
        snprintf(out, out_sz, "previous run last alive %lld s ago (boot time unknown)%s", (long long)(now - hb), extra);
    else if (rebooted)
        snprintf(out, out_sz, "console restarted since the previous run (last alive %lld s before this boot)%s",
                 (long long)(boot_now - hb), extra);
    else
        snprintf(out, out_sz, "Shallot restarted without a console reboot (previous run last alive %lld s ago)%s",
                 (long long)(now - hb), extra);
}

/*
 * While powering down: record what the worker is doing (meta sys_exit), so
 * the next start can say whether Shallot finished. "idle" = nothing running
 * and no job allowed to run; held jobs (startup, idle sweep...) don't count.
 */
static void ls_sys_note_exit(int64_t rt, int64_t since) {
    char st[120];
    int64_t now = ls_power_clock();
    int ready = 0;
    pthread_mutex_lock(&g_ls_job_mtx);
    for (ls_job_t *j = g_ls_jobs; j; j = j->next) if (ls_power_job_allowed(j->kind, now)) ready++;
    if (g_ls_worker_current[0]) snprintf(st, sizeof(st), "%s (+%d queued)", g_ls_worker_current, ready);
    else if (ready) snprintf(st, sizeof(st), "%d backup job(s) queued", ready);
    else snprintf(st, sizeof(st), "idle");
    pthread_mutex_unlock(&g_ls_job_mtx);
    pthread_mutex_lock(&g_ls_sys_mtx);
    int same = strcmp(g_ls_sys.exit_state, st) == 0;
    if (!same) snprintf(g_ls_sys.exit_state, sizeof(g_ls_sys.exit_state), "%s", st);
    pthread_mutex_unlock(&g_ls_sys_mtx);
    if (same) return;
    ls_meta_set_text("sys_exit", st);
    if (strcmp(st, "idle") == 0)
        ls_event("POWER_DOWN_READY", NULL, "all work finished %lld s after the signal", (long long)(rt - since));
}

/* One observation step (thread or tests). rt: epoch s, mono_ms: CLOCK_MONOTONIC ms. */
static void ls_sys_step(const ls_sys_sample_t *s, int64_t rt, int64_t mono_ms) {
    int resumed = 0, sd_started = 0;
    int64_t gap = 0;
    ls_sys_sample_t old = {0};
    int changed = 0;
    pthread_mutex_lock(&g_ls_sys_mtx);
    if (g_ls_sys.have_last) {
        int64_t drt = rt - g_ls_sys.last_rt, dmono = (mono_ms - g_ls_sys.last_mono_ms) / 1000;
        gap = drt - dmono;
        old = g_ls_sys.last;
        /* Two signs of a wake: Shallot was frozen (clock jump), or the state
         * returned from rest (seen on 13.60 without a clock jump). */
        int woke = g_ls_sys.in_rest && s->state == LS_SYS_STATE_RUNNING && old.state != LS_SYS_STATE_RUNNING;
        if (woke && gap <= 20) gap = rt - g_ls_sys.in_rest;
        if (gap > 20 || woke) {
            resumed = 1;
            g_ls_sys.in_rest = 0;
            g_ls_sys.resumes++;
            g_ls_sys.last_resume_at = rt;
            g_ls_sys.last_suspend_s = gap;
            ls_sys_hist_locked(rt, "resumed from rest after ~%lld s", (long long)gap);
        }
        changed = memcmp(&old, s, sizeof(*s)) != 0;
        if (!g_ls_sys.in_rest && old.state == LS_SYS_STATE_RUNNING &&
            (s->state == LS_SYS_STATE_REST_ENTER || s->state == LS_SYS_STATE_REST))
            g_ls_sys.in_rest = rt;
        if (resumed) g_ls_sys.shutdown_since = 0;           /* that signal was rest mode, and it is over */
        if (changed) {
            ls_sys_hist_locked(rt, "state %d->%d shutdown 0x%x->0x%x standby %d->%d", old.state, s->state,
                               (unsigned)old.shutdown, (unsigned)s->shutdown, old.standby, s->standby);
            /* Only 1 means "in progress": on 13.60 the idle value is an error code (0x80020000). */
            if (old.shutdown != 1 && s->shutdown == 1 && !g_ls_sys.shutdown_since) { g_ls_sys.shutdown_since = rt; sd_started = 1; }
            /* Signal withdrawn while the console kept running normally: cancelled. */
            if (g_ls_sys.shutdown_since && s->shutdown != 1 && s->state == LS_SYS_STATE_RUNNING &&
                old.state == LS_SYS_STATE_RUNNING && !sd_started)
                g_ls_sys.shutdown_since = 0;
        }
    } else {
        ls_sys_hist_locked(rt, "first sample: state %d shutdown %d standby %d", s->state, s->shutdown, s->standby);
    }
    g_ls_sys.have_last = 1;
    g_ls_sys.last = *s;
    g_ls_sys.last_rt = rt;
    g_ls_sys.last_mono_ms = mono_ms;
    int64_t since = g_ls_sys.shutdown_since;
    int beat = since || rt - g_ls_sys.last_heartbeat >= 30 || resumed;
    if (beat) g_ls_sys.last_heartbeat = rt;
    pthread_mutex_unlock(&g_ls_sys_mtx);

    /* Stage P1: tell the worker (rest: 500/300 -> it simply freezes; shutdown/reboot: 100). */
    int phase = -1;
    if (resumed || (!since && g_ls_power != LS_PWR_NONE)) phase = LS_PWR_NONE;
    else if (sd_started) {
        ls_config_t cfg;
        ls_load_config(&cfg);
        if (cfg.power_last_chance) phase = LS_PWR_SIGNALLED;
    } else if (since && changed && g_ls_power != LS_PWR_NONE) {
        if (s->state == LS_SYS_STATE_SHUTDOWN) phase = LS_PWR_SHUTDOWN;
        else if (s->state == LS_SYS_STATE_REST_ENTER || s->state == LS_SYS_STATE_REST) phase = LS_PWR_REST;
    }
    if (phase >= 0 && phase != g_ls_power) {
        ls_power_set(phase, rt);
        ls_event("POWER_PHASE", NULL, "%s%s", ls_power_name(phase),
                 phase == LS_PWR_SIGNALLED ? ": queued backups run now without the stability wait, other jobs held, restores refused" :
                 phase == LS_PWR_REST ? ": rest mode; work continues after wake" : "");
    }
    if (ls_power_down()) ls_sys_note_exit(rt, since);

    if (beat) ls_meta_set_int("sys_heartbeat", rt);
    if (resumed) ls_meta_set_int("sys_shutdown_seen", 0);
    if (sd_started) {
        ls_meta_set_int("sys_shutdown_seen", rt);
        ls_event("SHUTDOWN_REQUEST_DETECTED", NULL, "rest/shutdown/reboot in progress reported (state %d)", s->state);
    }
    if (changed && !sd_started) {
        char after[48] = "";
        if (since) snprintf(after, sizeof(after), " (%lld s after the signal)", (long long)(rt - since));
        ls_event("SYSTEM_STATE_CHANGE", NULL, "state %d->%d shutdown 0x%x->0x%x standby %d->%d%s", old.state, s->state,
                 (unsigned)old.shutdown, (unsigned)s->shutdown, old.standby, s->standby, after);
    }
    if (resumed) {
        g_ls_http_reopen = 1;
        ls_event("SYSTEM_RESUMED", NULL, "back from rest mode after ~%lld s; re-checking saves", (long long)gap);
        ls_config_t cfg;
        ls_load_config(&cfg);
        if (g_ls_worker_started && cfg.enabled)
            ls_worker_enqueue(LS_JOB_STARTUP, "", 0, 20);      /* same reconciliation as after a start */
    }
}

#ifndef LS_HOST_TEST
static void ls_sys_sample(ls_sys_sample_t *s) {
    s->state = sceSystemStateMgrGetCurrentState();
    s->shutdown = sceSystemStateMgrIsShellUIShutdownInProgress();
    s->standby = sceSystemStateMgrIsStandbyModeEnabled();
}

static void *ls_sys_thread(void *arg) {
    (void)arg;
    /* 8 KiB+ event payload on the PS4 ABI; generous buffer, contents only logged. */
    uint8_t *ev = calloc(1, 16384);
    int ev_errors = 0;
    for (;;) {
        ls_sys_sample_t s;
        ls_sys_sample(&s);
        struct timespec mono;
        clock_gettime(CLOCK_MONOTONIC, &mono);
        ls_sys_step(&s, (int64_t)time(NULL), (int64_t)mono.tv_sec * 1000 + mono.tv_nsec / 1000000);
        if (ev) {
            for (int i = 0; i < 4; i++) {           /* drain a few per tick */
                int rc = sceSystemServiceReceiveEvent(ev);
                if (rc != 0) {
                    pthread_mutex_lock(&g_ls_sys_mtx);
                    int first = g_ls_sys.event_rc != rc;
                    g_ls_sys.event_rc = rc;
                    pthread_mutex_unlock(&g_ls_sys_mtx);
                    if (first && ev_errors++ < 3)
                        ls_event("SYSTEM_EVENT_API", NULL, "ReceiveEvent returned 0x%08x (logged once per code)", (unsigned)rc);
                    break;
                }
                int type;
                memcpy(&type, ev, sizeof(type));
                pthread_mutex_lock(&g_ls_sys_mtx);
                g_ls_sys.events_received++;
                g_ls_sys.event_rc = 0;
                ls_sys_hist_locked((int64_t)time(NULL), "system event type 0x%x", type);
                pthread_mutex_unlock(&g_ls_sys_mtx);
                ls_event("SYSTEM_EVENT", NULL, "type 0x%x, first bytes %02x%02x%02x%02x %02x%02x%02x%02x", type,
                         ev[4], ev[5], ev[6], ev[7], ev[8], ev[9], ev[10], ev[11]);
            }
        }
        sleep(2);
    }
    return NULL;
}
#endif

/* Called once from ls_worker_start: previous-session report, then the probe thread. */
static void ls_sys_probe_start(void) {
    if (g_ls_sys_started) return;
    g_ls_sys_started = 1;
    int64_t now = (int64_t)time(NULL);
#ifndef LS_HOST_TEST
    int64_t boot_now = ls_sys_boottime();
#else
    int64_t boot_now = g_test_boottime;
#endif
    char prev[400];
    char *exit = ls_meta_get_text("sys_exit");
    ls_sys_classify_previous(ls_meta_get_int("sys_heartbeat", 0), ls_meta_get_int("sys_boottime", 0), boot_now, now,
                             ls_meta_get_int("sys_shutdown_seen", 0), ls_meta_get_int("sys_heartbeat", 0), exit, prev,
                             sizeof(prev));
    free(exit);
    ls_meta_set_text("sys_exit", "");
    pthread_mutex_lock(&g_ls_sys_mtx);
    snprintf(g_ls_sys.previous_session, sizeof(g_ls_sys.previous_session), "%s", prev);
    pthread_mutex_unlock(&g_ls_sys_mtx);
    ls_event("SESSION_PREVIOUS", NULL, "%s", prev);
    ls_meta_set_int("sys_boottime", boot_now);
    ls_meta_set_int("sys_shutdown_seen", 0);
    ls_meta_set_int("sys_heartbeat", now);
#ifndef LS_HOST_TEST
    ls_config_t cfg;
    ls_load_config(&cfg);
    if (!cfg.system_probe) { ls_event("SYSTEM_PROBE", NULL, "disabled in sync.json"); return; }
    int rc = sceSystemServiceEnableSuspendNotification();
    pthread_mutex_lock(&g_ls_sys_mtx);
    g_ls_sys.suspend_notify_rc = rc;
    pthread_mutex_unlock(&g_ls_sys_mtx);
    ls_event("SYSTEM_PROBE", NULL, "started; EnableSuspendNotification returned 0x%08x; boot time %lld", (unsigned)rc,
             (long long)boot_now);
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&tid, &attr, ls_sys_thread, NULL) == 0) pthread_detach(tid);
    pthread_attr_destroy(&attr);
#endif
}

static void ls_sys_json(char **buf, size_t *cap, size_t *pos) {
    pthread_mutex_lock(&g_ls_sys_mtx);
    char ep[820];
    json_escape(g_ls_sys.previous_session, ep, sizeof(ep));
    ls_json_append(buf, cap, pos,
        "{\"started\":%s,\"state\":%d,\"shutdown_in_progress\":%d,\"standby_enabled\":%d,\"resumes\":%d,"
        "\"last_resume_at\":%lld,\"last_suspend_s\":%lld,\"shutdown_since\":%lld,\"suspend_notification_rc\":%d,"
        "\"event_rc\":%d,\"events_received\":%d,\"power_phase\":\"%s\",\"previous_session\":\"%s\",\"history\":[",
        g_ls_sys_started ? "true" : "false", g_ls_sys.last.state, g_ls_sys.last.shutdown == 1, g_ls_sys.last.standby,
        g_ls_sys.resumes, (long long)g_ls_sys.last_resume_at, (long long)g_ls_sys.last_suspend_s,
        (long long)g_ls_sys.shutdown_since, g_ls_sys.suspend_notify_rc, g_ls_sys.event_rc, g_ls_sys.events_received,
        ls_power_name(g_ls_power), ep);
    for (int i = g_ls_sys.nhist - 1; i >= 0; i--) {
        char et[260];
        json_escape(g_ls_sys.hist[i].text, et, sizeof(et));
        ls_json_append(buf, cap, pos, "%s{\"ts\":%lld,\"text\":\"%s\"}", i == g_ls_sys.nhist - 1 ? "" : ",",
                       (long long)g_ls_sys.hist[i].ts, et);
    }
    ls_json_append(buf, cap, pos, "]}");
    pthread_mutex_unlock(&g_ls_sys_mtx);
}
