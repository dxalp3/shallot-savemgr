#!/usr/bin/env python3
"""
Turns this tree into Shallot SaveMgr: the same code as the Garlic SaveMgr LAN
sync fork (branch feature/lan-save-sync), under its own name, with its own
paths on the PS5 (/data/shallot, tile SHAL00001) so it can sit next to Garlic,
and with credits to Garlic SaveMgr (earthonion), which it is a fork of.

Workflow (branch "shallot", worktree ../shallot-savemgr):
    git merge feature/lan-save-sync        # bring in new work
    py -3 tools/rebrand_shallot.py         # rename what came in (idempotent)
    git add -A && git commit
A conflict on a line that mentions Garlic (renamed here, changed there) is
resolved by taking the feature side and running the script again:
    git checkout --theirs <file> && py -3 tools/rebrand_shallot.py

Left alone on purpose: README.md and CHANGELOG* (written separately for each
fork), CLAUDE_PROGRESS.md and docs/ (development history), links to the
upstream repositories, and this script.
Needs the 'cryptography' package (test vectors) and Pillow (icon).
"""
import os
import re
import subprocess
import sys

AUTHOR = "dxalp3"                                   # GitHub handle shown in the credits
AUTHOR_URL = "https://github.com/" + AUTHOR
UPSTREAM_URL = "https://git.etawen.dev/earthonion/garlic-savemgr"

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
EXCLUDE_FILES = {"README.md", "CLAUDE_PROGRESS.md", "tools/rebrand_shallot.py"}
EXCLUDE_PREFIXES = ("docs/", "CHANGELOG")
RENAMES = [("pc/garlic_sync.py", "pc/shallot_sync.py"), ("vault/garlic_vault.py", "vault/shallot_vault.py"),
           ("tests/pc/test_garlic_sync.py", "tests/pc/test_shallot_sync.py")]
WORDS = [("GARL00001", "SHAL00001"), ("GARLIC", "SHALLOT"), ("Garlic", "Shallot"), ("garlic", "shallot")]
# never renamed: where Garlic itself lives
PROTECT = [re.compile(r"git\.etawen\.dev/[\w.-]+/garlic-savemgr"), re.compile(r"Garlic SaveMgr by earthonion"),
           re.compile(r"fork of <a [^>]*>Garlic SaveMgr</a>"), re.compile(r"(?:a )?fork of Garlic SaveMgr"),
           re.compile(r"Garlic SaveMgr \(earthonion\)"), re.compile(r">Garlic SaveMgr</a>"),
           re.compile(r"^.*'about\.[a-z]+'.*$", re.M)]          # the About card and its translations


def git(*args):
    return subprocess.run(["git"] + list(args), cwd=ROOT, check=True, capture_output=True, text=True).stdout


def rename_files():
    for old, new in RENAMES:
        if os.path.exists(os.path.join(ROOT, old)):
            git("mv", old, new)
            print("renamed", old, "->", new)


def rebrand_text(s):
    keep = []

    def hold(m):
        keep.append(m.group(0))
        return "\x00%d\x00" % (len(keep) - 1)

    for p in PROTECT:
        s = p.sub(hold, s)
    for a, b in WORDS:
        s = s.replace(a, b)
    while "\x00" in s:                                # a protected span may hold another one
        s = re.sub(r"\x00(\d+)\x00", lambda m: keep[int(m.group(1))], s)
    return s


def rebrand_tree():
    changed = 0
    for rel in git("ls-files").splitlines():
        if rel in EXCLUDE_FILES or rel.startswith(EXCLUDE_PREFIXES):
            continue
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path) or os.path.getsize(path) > 8 * 1024 * 1024:
            continue
        raw = open(path, "rb").read()
        if b"\x00" in raw:
            continue                                  # binary
        try:
            s = raw.decode("utf-8")
        except UnicodeDecodeError:
            continue
        t = rebrand_text(s)
        if t != s:
            open(path, "wb").write(t.encode("utf-8"))
            changed += 1
    print("rebranded", changed, "file(s)")


def patch(rel, old, new):
    """Replaces old with new once; nothing to do if new is already there."""
    path = os.path.join(ROOT, rel)
    s = open(path, "rb").read().decode("utf-8")
    crlf = "\r\n" in s                                # Windows checkouts (core.autocrlf)
    s = s.replace("\r\n", "\n")
    if new in s:
        return
    if old not in s:
        raise SystemExit("rebrand: anchor not found in %s: %r" % (rel, old[:80]))
    s = s.replace(old, new, 1)
    open(path, "wb").write((s.replace("\n", "\r\n") if crlf else s).encode("utf-8"))


def credits():
    # Thank-you box after Quit
    patch("src/ui.html", """'<p style="color:var(--dm);margin-bottom:12px">by earthonion</p>'""",
          """'<p style="color:var(--dm);margin-bottom:12px">by %s \\u00b7 a fork of <a href="%s" target="_blank" rel="noopener" style="color:var(--ac2)">Garlic SaveMgr</a> by earthonion</p>'"""
          % (AUTHOR, UPSTREAM_URL))
    # Settings: an About card at the end (a foldable card like the others)
    patch("src/ui.html", """    /* end of the settings cards */""", """    + card(t('about.title', 'About Shallot'), '<div style="color:var(--dm);font-size:12px;line-height:1.6" id="aboutCard">'
    + esc(t('about.by', 'Shallot SaveMgr by {a}.', { a: '%s' })) + ' <a href="%s" target="_blank" rel="noopener" style="color:var(--ac2)">%s</a><br>'
    + esc(t('about.fork', 'A fork of Garlic SaveMgr by earthonion: the save decrypt/encrypt/resign/browse core and the original web UI come from Garlic.'))
    + ' <a href="%s" target="_blank" rel="noopener" style="color:var(--ac2)">Garlic SaveMgr</a></div>')   /* about.card */
    /* end of the settings cards */""" % (AUTHOR, AUTHOR_URL, AUTHOR_URL.replace("https://", ""), UPSTREAM_URL))
    patch("src/i18n/zh-CN.js", "\n};", """,
    /* Shallot: about */
    'about.title': "关于 Shallot",
    'about.by': "Shallot SaveMgr，作者 {a}。",
    'about.fork': "基于 earthonion 的 Garlic SaveMgr 分支开发：存档解密/加密/重新签名/浏览核心和最初的网页界面来自 Garlic。"
};""")
    patch("pc/shallot_sync.py", '"""\nShallot SaveMgr LAN sync — PC client (stdlib only, Python 3.8+).\n',
          '"""\nShallot SaveMgr LAN sync — PC client (stdlib only, Python 3.8+).\nShallot (%s) is a fork of Garlic SaveMgr (earthonion): %s\n' % (AUTHOR, UPSTREAM_URL))
    patch("vault/shallot_vault.py", '"""\nShallot vault — off-site store for the Shallot SaveMgr PC client (stdlib only).\n',
          '"""\nShallot vault — off-site store for the Shallot SaveMgr PC client (stdlib only).\nShallot (%s) is a fork of Garlic SaveMgr (earthonion): %s\n' % (AUTHOR, UPSTREAM_URL))
    # tray icon: a shallot's copper skin instead of garlic white
    patch("pc/shallot_sync.py", "fill=(240, 236, 226, 255), outline=(90, 80, 70, 255)",
          "fill=(178, 100, 106, 255), outline=(96, 50, 58, 255)")
    patch("CLAUDE.md", "# Claude instructions", """> **Shallot branch.** This tree is the LAN sync fork of Garlic SaveMgr, renamed to Shallot by
> `tools/rebrand_shallot.py`. Do feature work on `feature/lan-save-sync`, then here:
> `git merge feature/lan-save-sync`, `py -3 tools/rebrand_shallot.py`, commit.
> PS5 paths: /data/shallot, tile SHAL00001. README/CHANGELOG are written separately.

# Claude instructions""")


def vectors():
    """tests/host/vault_vectors.h from the renamed client's VaultCrypto (key labels changed)."""
    sys.path.insert(0, os.path.join(ROOT, "pc"))
    import importlib
    vc = importlib.import_module("shallot_sync").VaultCrypto
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM
    c = vc(bytes(range(32)))
    out = ["/* generated from pc/shallot_sync.py VaultCrypto (Python cryptography), secret = 00..1f */",
           'static const char *VK_ENC = "%s";' % c.enc_key.hex(), 'static const char *VK_ID = "%s";' % c.id_key.hex(),
           'static const char *VK_BID_ABC = "%s";' % c.block_id(b"abc"), 'static const char *VK_NAME_X = "%s";' % c.name("archive:x"),
           "static const struct { int n; const char *aad, *nonce, *ct; } VK_VEC[] = {"]
    for n in (0, 1, 15, 16, 17, 4096, 5000):
        pt = bytes((i * 7 + n) & 255 for i in range(n))
        nonce = bytes((i * 3 + n) & 255 for i in range(12))
        aad = "aad-%d" % n
        ct = AESGCM(c.enc_key).encrypt(nonce, pt, aad.encode())
        out.append('    { %d, "%s", "%s", "%s" },' % (n, aad, nonce.hex(), ct.hex()))
    out.append("};")
    open(os.path.join(ROOT, "tests/host/vault_vectors.h"), "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    print("regenerated tests/host/vault_vectors.h")


def icon():
    """assets/icon0.png: a shallot with the same sync badge."""
    from PIL import Image, ImageDraw
    import math
    S = 4                                             # draw large, then scale down (anti-aliasing)
    W = 512 * S
    img = Image.new("RGB", (W, W), (22, 25, 32))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle((28 * S, 28 * S, 484 * S, 484 * S), radius=56 * S, fill=(29, 33, 41))
    cx, top, bot = 256 * S, 92 * S, 420 * S
    h, a = bot - top, 112 * S

    def teardrop(scale, n=200):
        pts = []
        for i in range(n):
            t = 2 * math.pi * i / n
            x = cx + scale * a * math.sin(t) * math.sin(t / 2) ** 0.9
            y = top + h * (1 - math.cos(t)) / 2
            pts.append((x, y))
        return pts

    d.polygon(teardrop(1.0), fill=(178, 100, 106), outline=(96, 50, 58), width=7 * S)
    for k in (0.62, 0.28):                            # skin lines
        d.line(teardrop(k)[2:100], fill=(150, 78, 86), width=5 * S)
        d.line(teardrop(-k)[2:100], fill=(150, 78, 86), width=5 * S)
    d.polygon([(cx - 13 * S, top + 8 * S), (cx + 13 * S, top + 8 * S), (cx + 6 * S, 60 * S), (cx - 6 * S, 60 * S)],
              fill=(137, 112, 78))                    # dry neck
    for dx in (-36, -20, -6, 8, 22, 36):              # roots
        d.line((cx, bot - 4 * S, cx + dx * S, bot + 30 * S), fill=(120, 96, 76), width=5 * S)
    bx, by, r = 390 * S, 390 * S, 61 * S              # sync badge (as Garlic's)
    d.ellipse((bx - r, by - r, bx + r, by + r), fill=(77, 150, 255))
    d.arc((bx - 30 * S, by - 30 * S, bx + 30 * S, by + 30 * S), 40, 330, fill=(255, 255, 255), width=12 * S)
    d.polygon([(bx + 18 * S, by - 40 * S), (bx + 44 * S, by - 4 * S), (bx + 8 * S, by - 2 * S)], fill=(255, 255, 255))
    img.resize((512, 512), Image.LANCZOS).save(os.path.join(ROOT, "assets/icon0.png"), optimize=True)
    print("drew assets/icon0.png")


def check():
    left = []
    for rel in git("ls-files").splitlines():
        if rel in EXCLUDE_FILES or rel.startswith(EXCLUDE_PREFIXES):
            continue
        path = os.path.join(ROOT, rel)
        if not os.path.isfile(path):
            continue
        raw = open(path, "rb").read()
        if b"\x00" in raw:
            continue
        s = raw.decode("utf-8", "replace")
        for p in PROTECT:
            s = p.sub("", s)
        if re.search(r"garlic", s, re.I):
            left.append(rel)
    if left:
        print("still mentions Garlic outside credits/upstream links:", ", ".join(left))
    return not left


if __name__ == "__main__":
    unmerged = git("diff", "--name-only", "--diff-filter=U").split()
    if unmerged:
        raise SystemExit("rebrand: resolve the merge first (take the feature side: git checkout --theirs <file>): "
                         + ", ".join(unmerged))
    rename_files()
    rebrand_tree()
    credits()
    vectors()
    icon()
    sys.exit(0 if check() else 1)
