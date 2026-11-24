# OutRunners Vita — handoff for nikos

This document hands the OutRunners Vita port back to you. It covers four things:

- what was done on the port
- where that work lives
- how to set up Claude Code to keep going, including running it from your phone on the couch
- what is still open

The fastest start is part 3: you give this file to your Claude and let it read the rest.

---

## 1. Where the work is

- **Repo:** `totalfear/outrunners-vita`
- **Branch:** `uc/merge`. It is **not merged into `main` yet**. Everything below lives on this branch. Merge it once you're happy after a test race.
- **How the Vita changes are made:** the source tree stays upstream-clean. The Vita build edits it at build time.
  - The edits are Python `edit(name, marker, old, new)` blocks in **`.github/workflows/vita.yml`**: they replace `old` with `new`, and `marker` tells them the edit is already in.
  - Each block patches one area: `ports/sdl2/main.c`, `console/menu.c`, `console/console.h`, the tasks, the workers, the video, and the sound.
  - Almost every Vita change is in that file, behind `#ifdef __vita__`.
- **PC is unaffected.** The PC build was checked bit-exact (same picture and sound hashes) against upstream. The one visible PC change is that the bars around the picture are black.

### Building a VPK

The build runs on GitHub Actions. Nothing needs to be installed locally to build.

```bash
gh workflow run "Build Vita VPK" --repo totalfear/outrunners-vita --ref uc/merge
```

```bash
gh run list --repo totalfear/outrunners-vita --workflow "Build Vita VPK" --limit 1
```

```bash
gh run download <run-id> --repo totalfear/outrunners-vita -n OutRunners-vita-vpk -D vpk
```

The artifact is called **`OutRunners-vita-vpk`** and contains `OutRunners.vpk`. The run page also has `vita-build-logs` and `vita-symbols` (for crash addresses).

---

## 2. What changed for the Vita

**Speed: holds 60 in races**
- The game logic and sound run on core 0. The picture's rows run on cores 1–2, as worker threads.
- Frames are timed to the screen's refresh (`sceDisplayWaitVblankStart`) instead of a timer. That was the random stutter.
- The log is kept in memory and written to the card every 30 s. A line-by-line log was holding frames.
- **ZERO INPUT LAG** is on by default: no frame overlap, so one frame less lag. Turn it off and the game draws the next frame while the current one is shown, which gives more headroom but one frame more lag. The change applies on the next start.

**Sound**
- A 150 ms cushion. The resampler drifts at most ±2% to keep the cushion full, so there is no pitch jump and no gap. If the cushion empties, it refills with silence. **AUDIO PITCH = EXACT** turns the drift off.

**Picture**
- 16:9 and bilinear by default. 4:3 has black bars.
- **CRT BLEND** is on by default. Every frame is shown half-and-half with the one before it. The arcade blinks some things every other frame (road forks, the waterfall's spray), which a CRT smeared and an OLED/LCD shows as flicker.

**Menus: OPTIONS > VIDEO (Vita page)**
- PIXELS, WIDE, CRT BLEND, AUDIO PITCH, ZERO INPUT LAG.
- Saved to `outrunners.ini` with the keys `sound_cushion=`, `zero_lag=`, `crt_blend=` (plus the existing ones).

**Steering**
- The stick defaults to **SENSITIVITY 3**: the Vita's short throw hit full lock almost at once on 5.
- The d-pad ramps to full lock in about 0.3 s on purpose. OPTIONS > STEERING > SPEED makes it faster.
- An `outrunners.ini` that already exists keeps its saved sensitivity. Change it once in the menu.

**OPTIONS screen speed**
- The full-screen see-through shadow behind the OPTIONS pages doubled the picture's cost: 45 fps there.
- While an OPTIONS page is open, the picture is now drawn every other frame. The pages only change 30×/s, and the title behind them is frozen.
- The FPS counter reads about 30 there; the game and sound still run at 60.
- **Not yet confirmed on a Vita.**

---

## 3. Setting up Claude Code (desk + phone/couch)

What you need:

1. **A Claude plan with Claude Code.** Pro works; Max gives a lot more room. This port took a large share of a Max week.
2. **The Claude desktop app** (Windows or Mac). Use its **Code** tab. Sign in with your Claude account.
3. **The Claude app on your phone** (iOS/Android), signed in to the same account.
4. On the PC:
   - **Git**
   - **GitHub CLI**, signed in with `gh auth login` (as you, the repo owner)
   - **Python 3**
5. On the Vita: **VitaShell**. Press **SELECT** in VitaShell to start its FTP server (port 1337). It shows the Vita's IP address.

### First run

1. Clone the repo and check out the branch:
   ```bash
   gh repo clone totalfear/outrunners-vita
   ```
   ```bash
   git -C outrunners-vita checkout uc/merge
   ```
2. In the desktop app's Code tab, start a new session in the `outrunners-vita` folder.
3. Copy this file into the repo as **`CLAUDE.md`**, or paste it in your first message. Claude reads `CLAUDE.md` at the start of every session, so it will know the setup.
4. Tell it your Vita's FTP address (VitaShell shows it, for example `ftp://192.168.x.x:1337`).

### Phone / couch (Remote Control)

- Turn on **Remote Control** for the session; the desktop app has a Remote Control setting. Then open that session from the Claude app on your phone. You type on the phone, and the work runs on your PC.
- Also turn on **keep awake** in the app, so the PC doesn't sleep mid-build.
- Claude can send files (screenshots, logs) to your phone.
- You can send it screenshots too, like a Discord message or a photo of the Vita screen.

### A good first message to your Claude

> Read CLAUDE.md. This is my PS Vita port of OutRunners. Vita changes are applied at build time by the Python edit blocks in .github/workflows/vita.yml (branch uc/merge). My Vita's FTP is ftp://192.168.x.x:1337 (VitaShell). Build loop: dispatch "Build Vita VPK" on uc/merge, download the OutRunners-vita-vpk artifact with gh, upload it with curl to ux0:/OutRunners.vpk, I install it in VitaShell and test, then I turn FTP on and you read ux0:/data/outrunners/outrunners.log. Keep PC behaviour unchanged (Vita code behind #ifdef __vita__).

---

## 4. The debug loop (how we worked)

1. Claude edits `vita.yml`, checks that every edit still applies, commits to `uc/merge`, pushes, and starts the build.
2. When it's built, Claude downloads the VPK and uploads it to the Vita:
   ```bash
   curl -T OutRunners.vpk ftp://<vita-ip>:1337/ux0:/OutRunners.vpk
   ```
3. You install `ux0:/OutRunners.vpk` in VitaShell and race.
4. You turn FTP on and say "ftp is up". Claude pulls the log and reads it.

### Files in `ux0:/data/outrunners/`

| File | What it does |
|---|---|
| `outrunners.log` | The log. It is quiet by default: start-up and settings only. |
| `outrunners.ini` | Settings saved from the menus. |
| `timing.txt` (empty file) | Adds a timing report every 5 s: fps, frame ms, video/row pass/sprites, the sound program, refresh "held" counts. **Delete it for normal play.** |
| `frameskip.txt` | `0`–`3`: frames not drawn between drawn ones (testing only). |
| `workers.txt` | `0`–`3`: picture threads beside the game's. |
| `halfres.txt` | Half the picture's columns (testing only). |

The log reaches the card **every 30 s**. Stay in the game at least 30 s before quitting, or the timing is lost.

### Reading a timing report

```
60.1 frames a second; a frame takes 11.0 ms (its video 4.9, showing it 3.6), at most 12.7
  its row pass 4.3 ms ...
  the board's sprites 0.9 ms, the game's own code 1.4
  screen: 59.91 refreshes a second; pictures held 2+ refreshes 0 ...
```

- **A frame takes … at most**: needs to stay under 16.7 ms for 60.
- **Row pass and sprites**: the picture's cost.
- **Held 2+ refreshes**: a frame the screen showed twice, so a visible stutter. It should be about 0 in races.

---

## 5. Still open

- [ ] Confirm on a Vita that the OPTIONS screen feels smooth now (picture every other frame there).
- [ ] Merge `uc/merge` into `main` after a test race.
- [ ] Test the PC build yourself. The headless bit-exact check was done on Xphacter's machine. Those scripts (`vita_apply.py`, `check.sh`, `build.bat`) aren't in the repo; ask Xphacter if you want them added.
- [ ] Optional: ask Claude to make the OPTIONS shadow cheaper in the emulator's mixer. That's a bigger change, and PC would need re-checking.

## 6. Tips if you're new to Claude Code

- Say what you see, not only what you think the fix is: "drops to 45 in OPTIONS", "sound cuts out", plus a screenshot. Claude is good at finding the cause from a log.
- Ask it to **explain before it builds** if you want to understand a change.
- It asks before doing anything outward-facing, like pushing to `main` or posting. You can say "push to uc/merge whenever you build" once, so it doesn't ask each time.
- One session per task keeps things clear. Long sessions get summarised automatically, so you don't have to start over.
- Watch your plan's usage in the app. Builds and log reads are cheap; big rewrites aren't.
