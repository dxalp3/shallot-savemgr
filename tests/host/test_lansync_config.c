/*
 * Host tests for sync.json handling behind GET/POST /api/settings/sync:
 * defaults, partial override, clamping of bad values, atomic save + reload.
 */
#include "host_stubs.h"

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    host_reset_base();
    ls_config_t c;

    /* No file: defaults. */
    ls_load_config(&c);
    CHECK(c.enabled && c.game_close_backup && c.stability_samples == 2 && c.idle_delay_min == 10 &&
          c.startup_protect_untracked && c.followup_rescan_s == 180 && strcmp(c.bidirectional, "ask") == 0,
          "defaults");

    /* Partial POST body overrides only the given keys; bad values are clamped. */
    ls_parse_config("{\"idle_delay_min\": 25, \"game_close_backup\": false, \"stability_samples\": 99,"
                    " \"stability_timeout_s\": 1, \"bidirectional\": \"bogus\", \"regression_recovery\": \"certain\"}", &c);
    ls_config_clamp(&c);
    CHECK(c.idle_delay_min == 25 && !c.game_close_backup, "override applied");
    CHECK(c.stability_samples == 20 && c.stability_timeout_s == 5, "numbers clamped (%d, %d)",
          c.stability_samples, c.stability_timeout_s);
    CHECK(strcmp(c.bidirectional, "ask") == 0 && strcmp(c.regression_recovery, "certain") == 0, "enums validated");
    CHECK(c.stability_interval_s == 2 && c.enabled, "untouched keys keep their value");

    /* Atomic save + reload round trip. */
    CHECK(ls_save_config(&c) == 0, "saved");
    CHECK(!file_exists(LS_CONFIG_PATH ".tmp"), "no tmp left behind");
    ls_config_t r;
    ls_load_config(&r);
    CHECK(r.idle_delay_min == 25 && !r.game_close_backup && r.stability_samples == 20 &&
          strcmp(r.regression_recovery, "certain") == 0 && r.followup_rescan_s == 180, "reloaded equal");
    char j1[2048], j2[2048];
    ls_config_to_json(&c, j1, sizeof(j1));
    ls_config_to_json(&r, j2, sizeof(j2));
    CHECK(strcmp(j1, j2) == 0, "JSON identical after round trip");

    /* Where backups go: four modes, each maps to its destinations and back. */
    struct { const char *in, *mode, *route; int ps5, pc, vps, via_pc; } m[] = {
        { "{\"storage_mode\": \"pc\"}", "pc", "ps5", 0, 1, 0, 0 },
        { "{\"storage_mode\": \"ps5\"}", "ps5", "ps5", 1, 0, 0, 0 },
        { "{\"storage_mode\": \"vps\"}", "vps", "ps5", 0, 0, 1, 0 },
        { "{\"storage_mode\": \"both\", \"vps_route\": \"ps5\"}", "both", "ps5", 0, 1, 1, 0 },
        { "{\"storage_mode\": \"both\", \"vps_route\": \"pc\"}", "both", "pc", 0, 1, 0, 1 },
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        ls_config_t x;
        ls_config_defaults(&x);
        ls_parse_config(m[i].in, &x);
        CHECK(x.storage_ps5 == m[i].ps5 && x.storage_pc == m[i].pc && x.storage_vps == m[i].vps && x.vps_via_pc == m[i].via_pc,
              "mode %s: destinations", m[i].in);
        char j[2048], want[64];
        ls_config_to_json(&x, j, sizeof(j));
        snprintf(want, sizeof(want), "\"storage_mode\": \"%s\"", m[i].mode);
        CHECK(strstr(j, want) != NULL, "mode %s written back as %s", m[i].in, m[i].mode);
        snprintf(want, sizeof(want), "\"vps_route\": \"%s\"", m[i].route);
        CHECK(strstr(j, want) != NULL, "route written back as %s", m[i].route);
        ls_config_t y;
        ls_config_defaults(&y);
        ls_parse_config(j, &y);
        CHECK(y.storage_ps5 == x.storage_ps5 && y.storage_pc == x.storage_pc && y.storage_vps == x.storage_vps &&
              y.vps_via_pc == x.vps_via_pc, "mode %s survives a round trip", m[i].mode);
    }
    ls_config_defaults(&c);
    CHECK(c.storage_pc && !c.storage_ps5 && !c.storage_vps && !c.vps_via_pc, "default: the PC");

    /* Active / blocked PC clients survive a round trip; odd characters are neutralised. */
    ls_parse_config("{\"pc_active\": \"a1b2-c3\", \"pc_blocked\": \"ip:10.0.0.5,x\\\"y\"}", &c);
    ls_config_clamp(&c);
    CHECK(strcmp(c.pc_active, "a1b2-c3") == 0, "active client kept");
    {
        char j[2048];
        ls_config_to_json(&c, j, sizeof(j));
        CHECK(strstr(j, "\"pc_active\": \"a1b2-c3\"") != NULL && strstr(j, "\"pc_blocked\": \"ip:10.0.0.5,") != NULL,
              "written back (%s)", strstr(j, "pc_blocked") ? strstr(j, "pc_blocked") : j);
    }
    return host_finish();
}
