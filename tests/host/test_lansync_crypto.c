/*
 * Host tests for src/lansync_crypto.inc.c against vectors produced by the PC
 * client's own crypto (Python "cryptography"): key derivation, block ids,
 * names, AES-256-GCM seal and open, tamper detection.
 */
#include "host_worker_stubs.h"
#include "vault_vectors.h"

int main(int argc, char **argv) {
    g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    uint8_t secret[32];
    for (int i = 0; i < 32; i++) secret[i] = (uint8_t)i;
    ls_vkeys_t k;
    ls_vkeys_init(&k, secret);
    char hex[65];
    ls_hex(k.enc, 32, hex);
    CHECK(strcmp(hex, VK_ENC) == 0, "enc key derivation (%s)", hex);
    ls_hex(k.id, 32, hex);
    CHECK(strcmp(hex, VK_ID) == 0, "id key derivation");
    ls_vault_block_id(&k, "abc", 3, hex);
    CHECK(strcmp(hex, VK_BID_ABC) == 0, "block id = HMAC");
    ls_vault_name(&k, "archive:x", hex);
    CHECK(strcmp(hex, VK_NAME_X) == 0, "name = HMAC(\"name:\" + text)");

    static uint8_t pt[8192], ct[8192 + 28], back[8192], blob[8192 + 28];
    for (size_t v = 0; v < sizeof(VK_VEC) / sizeof(VK_VEC[0]); v++) {
        int n = VK_VEC[v].n;
        for (int i = 0; i < n; i++) pt[i] = (uint8_t)((i * 7 + n) & 255);
        uint8_t nonce[12];
        ls_unhex(VK_VEC[v].nonce, nonce, 12);
        uint8_t want[8192 + 16];
        size_t wl = strlen(VK_VEC[v].ct) / 2;
        ls_unhex(VK_VEC[v].ct, want, wl);
        uint8_t tag[16];
        ls_gcm(&k.aes, nonce, (const uint8_t *)VK_VEC[v].aad, strlen(VK_VEC[v].aad), pt, (size_t)n, ct, tag, 1);
        memcpy(ct + n, tag, 16);
        CHECK((size_t)n + 16 == wl && memcmp(ct, want, wl) == 0, "AES-256-GCM matches Python for %d bytes", n);
        /* open: our blob format nonce | ct | tag */
        memcpy(blob, nonce, 12);
        memcpy(blob + 12, want, wl);
        int64_t m = ls_vault_open(&k, VK_VEC[v].aad, blob, wl + 12, back);
        CHECK(m == n && (n == 0 || memcmp(back, pt, (size_t)n) == 0), "open %d bytes", n);
        if (n) {
            blob[12 + n / 2] ^= 1;
            CHECK(ls_vault_open(&k, VK_VEC[v].aad, blob, wl + 12, back) < 0, "tampered %d bytes refused", n);
        }
        CHECK(ls_vault_open(&k, "other-aad", (memcpy(blob + 12, want, wl), blob), wl + 12, back) < 0, "wrong aad refused");
    }
    /* seal -> open round trip with a random nonce */
    size_t sl = ls_vault_seal(&k, "x", pt, 4096, blob);
    CHECK(sl == 4096 + 28 && ls_vault_open(&k, "x", blob, sl, back) == 4096 && memcmp(back, pt, 4096) == 0, "seal/open round trip");
    uint8_t blob2[4096 + 28];
    ls_vault_seal(&k, "x", pt, 4096, blob2);
    CHECK(memcmp(blob, blob2, 12) != 0, "fresh nonce per seal");
    return host_finish();
}
