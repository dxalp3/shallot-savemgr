# Shallot — automatic save backups for your PS5

Shallot backs up your game saves by itself and keeps older versions, so you
can go back when a save gets corrupted, overwritten or deleted. It runs on the
PS5 and you use it from a web page: in the PS5 browser (through its home-screen
tile) or on any phone or computer on the same network.

Your backups can stay on the PS5, go to a PC (or NAS) at home, go to your own
online server, or all of these. It also includes the save tools from
[Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr), which it is
based on: browse, decrypt, encrypt, resign and import saves.

**New here?** Read on for the basics. Every setting and detail is in the
[full documentation](docs/DOCUMENTATION.md).

## What you get

- **Backups without thinking about it.** Close a game and its saves are backed
  up a moment later, once the game has finished writing them. Shallot never
  touches a game while it is running.
- **Every version, not just the last one.** Go back to yesterday's save, or put
  a whole game back to how it was at any earlier point. Deleted a save? Bring
  it back. Changed your mind after a restore? Undo it.
- **A warning when something goes wrong.** If a save suddenly turns into an
  older version (a crash, a bad cloud download), Shallot notices and offers to
  put the newer one back.
- **Your choice of where backups live**, with copies kept until each place
  has them.
- **Little to set up.** A home-screen tile, optional start with the console,
  English and Chinese.

## What you need

- A PS5 that can run homebrew payloads, with a payload loader.
  Shallot is developed and tested on firmware 13.60.
- Optional, depending on where you want backups:
  - a Windows PC, or a NAS / home server / any computer with Docker;
  - your own small online server (VPS) with Docker.

## Getting started

1. Send `shallot-savemgr.elf` to your payload loader.
2. Open the **Shallot** tile on the PS5 home screen, or go to
   `http://<your PS5's IP address>:8082` on a phone or computer.
3. Open **Settings → Where backups go** and pick one (see below).
   Until you do, backups wait on the PS5 for a PC to collect them.
4. Play. Backups happen from now on.

To have Shallot running after every restart: **Settings → Start with the
console** (built in for onionHEN; with another loader, add the file to its
auto-start list).

## Where should backups go?

| Choice | Good for | What you need |
|---|---|---|
| **PS5 only** | Quick undo of mistakes; nothing else to set up. Old versions are thinned and packed to save space. | Nothing. A broken console takes the backups with it. |
| **PC** | Safe copies off the console, at home. | A Windows PC (tray app) or a NAS/server with Docker. |
| **VPS** | Copies off-site, no PC needed. | Your own server with Docker, internet on the PS5. |
| **PC and VPS** | The safest: at home and off-site. | Both of the above. |

Backups are never removed from the PS5 until every place you chose has them.
If your PC is off for a week, they simply wait.

**Setting up a PC:** click the **PC** chip at the top of the page →
*Set up a PC client*. Pick *Tray app* (Windows) or *Docker*, download the files
it offers (they already know your PS5's address) and follow the three steps.

**Setting up a VPS:** click the **VPS** chip → *Set up a vault*. You download
the files, start them with Docker on your server and enter its address and
password-like token under *Where backups go*. Everything is encrypted before it
leaves your home, so the server only ever sees scrambled data. Keep a copy of
the vault key (VPS panel → *Key*) somewhere safe: without it the online copy
cannot be read.

## Everyday use

- **See your backups:** Browse → pick a game. Each save shows when it was last
  backed up; click it for its versions.
- **Restore one save:** pick a version → *Restore*. A copy of the current save
  is made first, so you can undo it.
- **Restore a whole game:** game menu → *Restore to an earlier point*. You see
  what will change before anything happens.
- **Bring back a deleted save:** at the bottom of the game page.
- **Back up right now:** *Back up now* on a game or save, or Backup & Sync →
  *Scan Changed Saves Now*.
- **Pause backups for a while:** *Stop Shallot → Pause*. *Resume* appears at
  the top of the page.
- **Status at a glance:** the chips at the top (green = all good, orange =
  waiting or needs attention). Click one for details. Every `?` explains the
  button or setting next to it.

## Keeping it safe

- Anyone on your home network can open the Shallot page. To require a password,
  use **Settings → Web UI login**. The PS5's own browser never asks for it, and
  your PC client gets a key automatically when you download its files again.
- Shallot shows a PS5 notification if backups stop reaching your PC or server
  for a long time.

## Questions

**Does it slow down my games?** No. It works while games are closed and waits
until saves are completely written.

**What happens when I put the PS5 in rest mode or turn it off?** A backup in
progress is finished; after rest mode Shallot picks up where it left off.

**How much space does it use?** Only changed saves are backed up. Older
versions are thinned (every version for a couple of days, then one per day,
then one per week) and identical parts are stored once.

**My PC was off for days. Did I lose anything?** No. Backups waited on the PS5
and the PC collects them when it is back.

**How do I update Shallot?** *Stop Shallot → Quit*, then send the new
`shallot-savemgr.elf`.

**Can I still use the Garlic tools?** Yes: the Decrypt, Encrypt, Resign and
Import tabs, and the file browser for opened saves. Garlic's manual cloud
backups (FTP, Google Drive, USB) are under Settings → Legacy, described in
[docs/LEGACY_CLOUD.md](docs/LEGACY_CLOUD.md).

## More

- [Full documentation](docs/DOCUMENTATION.md): every setting, the PC client
  and server in detail, the API, troubleshooting, building from source.
- What has been tested on a real PS5 so far: see *Testing status* in the
  documentation.

## Credits

Shallot SaveMgr by [dxalp3](https://github.com/dxalp3). Based on
[Garlic SaveMgr](https://git.etawen.dev/earthonion/garlic-savemgr) by earthonion,
which provides the save decrypt/encrypt/resign/browse core, the PS5 integration
and the original web UI; Garlic's cloud backups include work by stamatisd.
Garlic SaveMgr has no published licence yet, so this repository stays private
until its author agrees to a public release.
