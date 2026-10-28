# RA3 Frame-Rate Mod — Reverse-Engineering Notes

Reference for everything learned about Red Alert 3's frame pacing, so this work never has to be redone.
All addresses are **RA3 v1.13 (EA app)** virtual addresses unless stated otherwise. Uprising addresses differ — use byte signatures.

Contents
1. Target identification
2. Key conclusions (read this first)
3. Globals
4. GameEngine object and vtable
5. Frame-pacing mechanism in detail
6. Ratio simulation: which frame rates work
7. Why `cb8514` must never be patched globally
8. Chosen mod approach (approach B: v1 cadence, v2–v4 catch-up, v4–v5 animation fix)
9. Live test results
10. Ultrawide FOV fix
11. Other strings and leads
12. User config changes already made
13. Web research summary
14. Open questions / TODO

---

## 1. Target identification

| Item | Value |
|---|---|
| Install | EA app, *Command & Conquer: The Ultimate Collection* (2024 release), `F:\EA Games\Command and Conquer Red Alert 3\` |
| RA3 binary | `Red Alert 3\Data\RA3_1.13.game` (13,307,904 bytes) |
| SHA-256 | `e4016847423869b3d4e6a5ae4c74228f2083c65e34416104635602c5526b3eee` |
| Format | PE32, x86, GUI, image base `0x400000`, entry RVA `0xd3de3` |
| Build | PE timestamp 1740089691 (Feb 2025 rebuild); not packed, no DRM wrapper |
| PDB name | `RTS-final.pdb` (GUID ac3e1d84-77d2-4d1a-8333-c247d6318704, age 1) — not available |
| RTTI | None (only 6 std:: type descriptors) — no class names to recover |
| Uprising binary | `Red Alert 3 Uprising\Data\ra3ep1_1.1.game` (13,381,632 bytes). Same engine build, same debug/timing strings; **addresses differ → signature scanning required** (all signatures in §8 match; see the address map in §8) |
| Launcher | `RA3.exe` / `RA3EP1.exe` read `*.SkuDef`; the highest version's `set-exe` line selects the `.game` (RA3 → `RA3_1.13.game`, Uprising → `ra3ep1_1.1.game`). DLL search dir = `Data\`. |
| Shipped DLL in Data | `xinput1_3.dll` (both games) |
| Static DLL imports (both games) | `d3d9.dll` (only `Direct3DCreate9`), `DSOUND.dll` by ordinal (1, 3, 6, 8, 9) — both usable for proxy injection (see §8) |

Sections (RA3 1.13; VA = RVA + 0x400000; raw offset == RVA for .text/.rdata/.data):

| Name | RVA | VSize | Raw off | Raw size |
|---|---|---|---|---|
| .text | 0x1000 | 0x7ccb0a | 0x1000 | 0x7cd000 |
| .rdata | 0x7ce000 | 0xe4c1a | 0x7ce000 | 0xe5000 |
| .data | 0x8b3000 | 0x85ec0 | 0x8b3000 | 0x2b000 (rest is BSS) |
| .tls | 0x939000 | 0x1d | 0x8de000 | 0x1000 |
| rts.ver | 0x93a000 | 0x200 | 0x8df000 | 0x1000 |
| .rsrc | 0x93b000 | 0x3d0eb4 | 0x8e0000 | 0x3d1000 |

Imports of interest: `timeGetTime`, `timeBeginPeriod`, `QueryPerformanceCounter/Frequency`, `Sleep`, `SleepEx`.

---

## 2. Key conclusions (read this first)

- The **simulation (logic) runs at 15 Hz** (`cb8510 = 15`). The **client/draw cadence max is 30 fps** (`cb8514 = 30`).
- Each logic tick is divided into **6 substeps**. Each drawn frame processes `6 / ratio` substeps, with `ratio = maxUpdateFPS / logicFPS` (vanilla 30/15 = 2). The engine natively renders interpolated frames between logic ticks.
- **Only ratios that divide 6 work correctly: 1, 2, 3, 6 → 15 / 30 / 45 / 90 fps.**
  - 60 fps (ratio 4) or 75 fps (ratio 5) → game runs in **slow motion** (60 fps ⇒ 0.67× speed).
  - Above 90 fps (ratio > 6) → simulation **speeds up** (120 fps ⇒ 1.33×).
  - **Max safe frame rate = 90 fps.** 60 fps is not achievable without rewriting the substep scheduler.
- **Never patch `cb8514` (the 30) globally.** Game-logic code and ~40 static initializers read it; changing it alters gameplay and breaks multiplayer/replay sync.
- Correct approach: redirect only the frame-cadence code sites to a mod-owned "max update FPS" variable, and clamp the adaptive (multiplayer) ratio to a divisor of 6.
- **Live-tested (§9):** at 90 fps the logic stays at exactly draw fps / 6, i.e. 15 Hz at a steady 90. Game speed is tied to *drawn frames*, so dips below 90 cause proportional slow motion. Vanilla does the same below 30. The v2 "catch-up" mode (§8) compensates offline.
- **The game is Vert-.** Horizontal FOV is fixed, so ultrawide resolutions look zoomed in. The mod hooks `Camera::setFOV` for a Hor+ correction (§10).
- **Some client systems advance by a fixed amount per *drawn* frame.** At 90 fps, unit walk cycles played 3× too fast while movement was correct.
  - The real cause is the client animation clock `cea848 += ceac2c (33 ms) × frames` in fn `0x627b90`. v5 redirects it to an elapsed-time value (§8).
  - The v4 draw-module dt patch (`ce51f4`) is a secondary consumer, not the cause.
  - `ceac2c` is also read by camera code, which may still run fast. Other readers of `ce51f4`/`ce5110` are unclassified (§3, §14).

---

## 3. Globals (RA3 1.13)

| Address | Type | Meaning |
|---|---|---|
| `0xcb8510` | int | **Logic FPS = 15** (constant, never written at runtime). Uprising: `0xccce90` |
| `0xcb8514` | int | **Max update / client FPS = 30** (constant, never written). Also read by game logic — see §7. Uprising: `0xccce94` |
| `0xce50dc` | float (BSS) | **ms per logic frame** = `1000.0 / LogicFPS` = 66.67. Set by static initializer around `0xbbb02c` from `cb8510` and constant `0xbed7c4` (= 1000.0f) |
| `0xceac2c` | int (BSS) | **ms per client frame** = `1000 / 30` = 33. Static init `0xbbbc00`; also a runtime virtual setter `0x5e2b70` (vtable slot at `0xc13764`). Readers:<br>• the client animation clock in `0x627b90`, at `0x627da1` and `0x627d5d`. **Patched in v5 (§8).**<br>• camera fn `0x615a90`, at `0x615b11`, `0x615b56` and `0x615c19`.<br>• camera-transition code (`0x5e3fa0`, `0x5ed760`, …), which computes frame counts as `ms / ceac2c`. Still unpatched (§14).<br>Uprising address: `0xcfe71c` |
| `0xcea848` | int (BSS) | **Client animation/sync time in ms.** Advances by `ceac2c × frames` per drawn frame in `0x627b90`. Reset to 0 at `0x620378`. Passed to the render engine's `Sync` at `0x963bb0` and `0x4d7750`. W3D readers: `0x935066`, `0x937bd7`, `0x938713`, `0x938b50`, `0x938b90`, `0x938c00`, `0x938df0`, `0x93ab20`. Uprising address: `0xcfe318` |
| `0xce5110` | float (BSS) | **client FPS as float = 30.0** (static init from `cb8514`). 17 refs, read by both logic and client code |
| `0xce5114` | float (BSS) | **ms per client frame = 1000/30 = 33.33**. 14 refs, e.g. `RenderingSound.cpp` `0x539460`, client init `0x56b680` |
| `0xce511c` | float (BSS) | **client frames per ms = 0.03**. 12 refs: `FxpsGPUStorageModule.cpp` `0x6ea710` (client particles) but also `SlaveWatcherBehavior` `0x7b2890` (**logic**) |
| `0xce51f4` | float (BSS) | **seconds per client frame = 1/ce5110 = 1/30**, static init `0xbbb100`. Readers: `0x555e10`, `0x614530`, `0x6145c0`, `0x6e6d70`, `0x90afa0`, `0x90d290`, `0x930e40`. Only `0x930e40` (draw-module dt; v4, a secondary consumer) is patched; the others are unclassified. Uprising: `0xcf8c98` |
| `0xcec412` | byte | FPS-limit-enabled flag (recomputed each loop iteration) |
| `0xcec420` | int | Frames skipped counter (incremented at `0x64fc01`) |
| `0xcec424` | int | Time stamp written by client update `0x64fc10` (used in main-loop limit decision) |
| `0xcec428` | int | Total ms slept |
| `0xcec42c` | int | ms slept this frame |
| `0xcec430` | int | Frame delta (ms) |
| `0xcec434` | int | Last frame time |
| `0xcec438` | int | Start time (set lazily on first loop) |
| `0xcec43c` | ptr | Limiter/options object; vfunc `+0x40` = FPS-limit getter ("max update FPS (game speed)") |
| `0xcea834` | float | FPS ratio (fraction of max); debug prints `ratio * 30.0` (hard-coded 30) |
| `0xcb8abc` | float | **SCALAR** — network game-speed scalar. Min 0.5 (`0xbed6a8`), max 1.0 (`0xbed250`), step 0.05 (`0xbf3974`) |
| `0xce21a4` | ptr | **TheGameLogic**. `+0x50` = logic frame number; vfunc `+0x34` = per-substep update(substep); `+0x15c`, `+0xa7`, `+0xa1` flags |
| `0xcecf3c` | ptr | Network interface (**null in single player**). vfunc `+0x84` returns connection info |
| `0xcecf24` | ptr | TheGameInfo |
| `0xcec458` | ptr | Object whose `+0x50` byte disables the FPS limit; `+0x80` string used for `.RA3Skirmish/.RA3Campaign/.RA3Replay` handling |
| `0xcded9c` | ptr | Subsystem; vfunc `+0x9c(frame*10 + substep)`, `+0xa0` tail-called from update |
| `0xce4c10` | ptr | Subsystem; vfunc `+0x74` = current time getter, `+0x38`; byte `+0xc4` flag |
| `0xce4c74` | ptr | vfunc `+0xa0` — part of FPS-limit enable check (`<= 1` → limit on) |
| `0xce6650` | ptr | `0x581a80` — part of FPS-limit enable check |
| `0xc16f10` | float | 1/6 (substep → interpolation fraction) |
| `0xc16f18` | double | 0.001 |
| `0xbed250` | float | 1.0 |
| `0xbed6a8` | float | 0.5 |
| `0xbf3974` | float | 0.05 |
| `0xbed7c4` | float | 1000.0 |

Frame-pacing helpers:
- `0x56f670` → `jmp 0xb84010` — millisecond timer.
- `0x9c4150` — sleep wrapper: `SleepEx(*arg, TRUE)` (called with 0 in the busy-wait loop).

---

## 4. GameEngine object and vtable

Derived GameEngine vtable **`0xc17148`** (assigned at `0x62d785`, `0x62d833`, `0x634693`). Base vtable `0xbf51a0`.

| Offset | Function | Role |
|---|---|---|
| +0x10 | `0x644360` | init/reset (computes ratio → `+0x5c`) |
| +0x14 | `0x62b920` | **update()** — advances substep counter, calls step() |
| +0x38 | `0x652880` | **execute()** — main loop + frame limiter |
| +0x44 | `0x628c90` | "is last subframe of tick" check: returns `counter == 6/ratio` when ratio < 6, else `counter == 1` |
| +0x90 | `0x63f3d0` | **step(substep)** — calls `TheGameLogic->vfunc34(substep)`; processes `6/ratio` substeps |
| +0x94 | `0x64fae0` | client update / draw, with frame-skip check (`0x62b870`) |

Engine init function `0x650ed0` (references "GameEngine", "TheDisplay", "TheGameClient", … ; contains cadence site `0x65280d`).

Engine object fields:

| Offset | Meaning |
|---|---|
| +0x28 | quit flag (loop runs while 0) |
| +0x58 | substep counter (1..6) |
| +0x5c | ratio (`maxFps / logicFps`) |
| +0x60 | interpolation fraction = counter × 1/6, clamped to [0, 1] |
| +0x64 | flag (ratio refresh) |
| +0x68 / +0x6c / +0x70 | perf counters (every 25 logic frames) |
| +0x74 … +0x173 | 64-entry frame-delta ring buffer |
| +0x174 | ring buffer sum |
| +0x178 | ring index |
| +0x17c | **frames per logic tick (ratio) used by the limiter** |

---

## 5. Frame-pacing mechanism in detail

### Main loop `execute()` @ `0x652880`
Per iteration:
1. `this->vfunc14()` → `update()`.
2. `cec412 = 0`; set to 1 if `ce4c74->vfuncA0() <= 1 && !ce6650->0x581a80()` (single-player ⇒ limit on); forced 0 if `cec458+0x50`.
3. If network present (`cecf3c`) and not paused: compare run-ahead frames of all 6 slots; adjust `SCALAR` (`cb8abc`) ±0.05 within [0.5, 1.0].
4. If `this+0x5c + cec424 > now` → no-limit path; else limit path.
5. **Limit path (`0x652c90`)**: `target_ms = (int)trunc(ce50dc / (this+0x17c × SCALAR))`; busy-wait with `SleepEx(0,1)` until `now - lastFrame >= target_ms`.
6. Ring buffer of 64 frame deltas → `avgFps = 64000 / sum`.
7. **Adaptive ratio (`0x652d34`–`0x652d88`)**: `esi = cb8514` (`8B 35` at `0x652c77`). If no network: `eax = maxFps`. Else `eax = clamp(avgFps, logicFps, maxFps)`. `ratio10 = eax*10/logicFps`; if `|ratio10 - this+0x17c*10| > 8` → `this+0x17c = ratio10/10` (store at `0x652d88`: `89 87 7C 01 00 00`).

Vanilla single player: `this+0x17c = 30*10/15/10 = 2` → `target = 66.67/2 = 33.33 → 33 ms` → ~30.3 fps.

### update() @ `0x62b920`
- `counter = this+0x58 + 1`; `this+0x60 = clamp(counter/6, 0, 1)`.
- If `counter <= 6` → `step(counter)`; else new logic tick: `counter = 1`, `step(1)`.
- When `counter == 6` and `+0x64` set: `this+0x5c = cb8514 / cb8510` (site `0x62b98d`).

### step(s) @ `0x63f3d0`
- `s == 1`: network/logic-tick bookkeeping.
- `TheGameLogic->vfunc34(s)`.
- `ratio = cb8514 / cb8510` (site `0x63f4f6`).
- If `ratio < 6`: `end = floor((floor((s-1)*ratio/6) + 1) * 6 / ratio)`; process substeps `s+1 … end` (calling `vfunc34` each, incrementing `+0x58`), then update `+0x60`.
- If `ratio >= 6`: process only substep `s` (jump to return at `0x63f5d0`).

### Client update @ `0x64fae0`
Once per drawn frame: calls `0x726e50` (with logic `+0xa1` flag set), `cf5204->vfunc14`, `0x642d50`, `ced3b4 → 0x6fadc0`, `cded9c->vfunc9c(logicFrame*10 + this+0x58)`, then frame-skip check `0x62b870` (increments `cec420`, writes `cec424`). `0x62b870` is the frame-skip decision and only applies in network games.

### Sleep / timer primitives
- `0x9c4150` = `SleepEx(*arg, TRUE)` wrapper; the limiter calls it with 0, so the wait is a busy-wait yield loop, not a real sleep.
- `0x56f670` → `0xb84010` millisecond timer.

### Timing precision
`target_ms` is truncated to an integer:
- vanilla 33.33 → 33 ms (~1% fast)
- 90 fps: 11.11 → 11 ms (same ~1%)
- 60 fps: 16.67 → 16 ms (~4% fast)

---

## 6. Ratio simulation: which frame rates work

Hand-simulation of step()'s substep math (6 substeps per logic tick):

| Ratio | FPS | Substeps per frame | Frames per tick | Result |
|---|---|---|---|---|
| 1 | 15 | 6 | 1 | correct |
| 2 | 30 | 3,3 | 2 | correct (vanilla) |
| 3 | 45 | 2,2,2 | 3 | correct |
| 4 | 60 | 1,1,1,1,1,1 | **6** | **broken — 0.67× speed** |
| 5 | 75 | 1 each | 6 | broken — slow motion |
| 6 | 90 | 1 each (ratio ≥ 6 branch) | 6 | correct |
| > 6 | e.g. 120 | 1 each | 6 | **broken — sim speeds up (120 ⇒ 1.33×)** |

Game speed = actual fps / ratio, so if the machine can't sustain the target, the game slows down (same as vanilla at < 30 fps).

---

## 7. Why `cb8514` must never be patched globally

1. **Game-logic code reads it**, e.g. `gamelogic\object\Behavior\BezierProjectileBehavior.cpp` (fn `0x7b9d60`) and `RespawnUpdate` (fn `0x7e3770`). Changing it changes simulation behaviour ⇒ desync with other players and replays.
2. **~40 static initializers** (`0xbb8ee0` … `0xbc4af0`) run at startup and store `cb8514 >> 1` (= 15) into separate globals (e.g. `cdde20`, `ce21fc`, `ce4448`, `ce4dc0`, `ce514c`, `ce668c`, `ce67d0`, `cea95c`, `cec790`, `cecfcc`, `ced31c`, `ced664`, `ceee18`, `cf0198`, `cf1de4`, `cf2204`, `cf2498`, `cf2764`, `cf2cec`, `cf2e20`, `cf31cc`, `cf5238`, `cf73a0`, `cf7424`, `cf7434`, `cf7454`, `cf74a0`, `d0aacc`, `d0abf4`, `d0aee0`, `d0b134`, `d0b288`, `d0b380`, `d0b40c`, `d0b918`, `d0b9c4`, `d0ba48`, `d0baac`, `d0bab4`). Also `0xbbbc00`: `ceac2c = 1000 / cb8514` (= 33 ms); `0xbbb080`/`0xbbb0b0`/`0xbbb0e0`/`0xbbcd70` derive floats from it (e.g. `ce5110`).
   A proxy DLL loaded as a static import would run **before** these initializers, so a global patch would change all of them too.
3. The **derived client-frame constants** `ce5110` (30.0), `ce5114` (33.33 ms), `ce511c` (0.03 frames/ms) and `ce51f4` (1/30 s) are also read by both logic and client code (see §3), so they must not be patched globally either. The same goes for `ceac2c` (33 ms int), which is read by the client animation clock and by camera code. Fix individual client readers instead, as v4 and v5 do in §8.

### Runtime readers of `cb8514` (excluding static initializers)

| Function | Usage | Identifying info / classification |
|---|---|---|
| `0x525c60` | float math with 30 | unknown |
| `0x53b5c0` | `x % (30>>1)` | unknown |
| `0x543e80` | `div [cb8514]` | unknown |
| `0x544d10` | `30 * [cb8424]` | unknown |
| `0x550650` | float divide by 30 | unknown |
| `0x553750` | float math | unknown |
| `0x568670` | stores 30 into `[obj+0x1dc]` | **`gameclient\Drawable.cpp`** (client) |
| `0x615a90` | `bed72c / 30` | unknown |
| `0x628c90` | ratio = 30/15 | **cadence site** (GameEngine +0x44) |
| `0x62b920` | ratio = 30/15 | **cadence site** (update) |
| `0x63f3d0` | ratio = 30/15 | **cadence site** (step) |
| `0x644360` | ratio = 30/15 | **cadence site** (engine init/reset) |
| `0x650ed0` | ratio = 30/15 at `0x65280d` | **cadence site** (GameEngine init; strings "GameEngine", "TheDisplay", "TheGameClient", …) |
| `0x652880` | `mov esi,[cb8514]` at `0x652c77` | **cadence site** (main loop clamp) |
| `0x6727a0` | float math | "TickerMgmt" (UI ticker, client) |
| `0x6f6df0` | `x * 30` | unknown |
| `0x704de0` | float divide | unknown |
| `0x733fb0` | float math | unknown |
| `0x768ca0` | passes 30 to `0x543ac0`/`0x543b20` | unknown |
| `0x7b9d60` | float math | **`gamelogic\...\BezierProjectileBehavior.cpp` (LOGIC)** |
| `0x7bb120` | float math | unknown (near projectile code — likely logic) |
| `0x7e3770` | float math | **"RespawnUpdate" (LOGIC)** |
| `0x7ee0f0` | `30 * [obj+0x3c]` | unknown |
| `0x7f2c40` | float math | unknown |
| `0x7f7e40` | `30>>1` | unknown |
| `0x93afb0` | stores 30 into `[obj+0x290]` | **client draw-module constructor**, same class family as the draw-module dt consumer `0x930e40` (§8, v4) |
| `0x944c60`, `0x944cf0`, `0x945a30` | ms→frames with 30 | unknown (likely client) |
| `0xa10d40`, `0xa1de90`, `0xa1eb00` | float math / `% (2*30)` | unknown (likely client) |
| `0xa2bda0`, `0xa2dfe0` | float math, `+30>>1` | unknown |
| `0xa47540`, `0xa4bae0` | float math | unknown |
| `0xa55bc0`, `0xa55d60` | compare frame delta to 30 | unknown |
| `0xaafc80`, `0xab1c30` | float math | unknown |

---

## 8. Chosen mod approach (approach B: v1 cadence, v2–v4 catch-up, v4–v5 animation fix)

Leave `cb8514` untouched. Redirect only the frame-cadence code to a mod-owned `g_maxUpdateFps` (90, or 45/30):

1. Cadence sites, pattern `A1 <&maxFps> 33 D2 F7 35 <&logicFps>` (`mov eax,[maxFps]; xor edx,edx; div [logicFps]`) — patch the first dword operand to `&g_maxUpdateFps`:
   - `0x628c90` (followed by `56 8B F0 83 FE 06`)
   - `0x62b98d` (followed by `C6 46 64 00 89 46 5C`)
   - `0x63f4f6` (followed by `8B F8 83 FF 06 0F 8D`) — **unique anchor signature**: `A1 ?? ?? ?? ?? 33 D2 F7 35 ?? ?? ?? ?? 8B F8 83 FF 06 0F 8D`; extract `&maxFps` and `&logicFps` from it.
   - `0x64439b` (followed by `89 46 5C A1`)
   - `0x65280d` (followed by `89 47 5C`)
   Exactly 5 occurrences of `A1 <&maxFps> 33 D2 F7 35 <&logicFps>` exist in RA3 1.13.
2. Main-loop clamp: `8B 35 <&maxFps>` at `0x652c77` (`mov esi,[cb8514]`, preceded by `mov ecx,[edi+0x174]; cmp ecx,ebp`) → `&g_maxUpdateFps`.
3. Adaptive ratio hook at `0x652d88` (`89 87 7C 01 00 00` = `mov [edi+0x17c], eax`): 5-byte jmp + nop to a stub that rounds `eax` down to a divisor of 6 (≥6 → 6; 4,5 → 3; 1–3 unchanged), stores it, and jumps back to `0x652d8e`.
4. Cap the config at 90; only allow 30/45/90.

### Address map (all found by the same signatures)

| Item | RA3 1.13 | Uprising 1.1 |
|---|---|---|
| LogicFPS constant | `0xcb8510` | `0xccce90` |
| MaxUpdateFPS constant | `0xcb8514` | `0xccce94` |
| Ratio sites (`A1 max 33 D2 F7 35 logic`) | `0x628c90`, `0x62b98d`, `0x63f4f6`, `0x64439b`, `0x65280d` | `0x632570`, `0x6352dd`, `0x648406`, `0x64d2fb`, `0x65b06e` |
| Main-loop `mov esi,[max]` (patched) | `0x652c77` | `0x65b4d7` |
| Other `mov esi,[max]` (unrelated, **left untouched**) | `0xa1eb96` | `0xa12346` |
| Main-loop clamp signature start | `0x652c6f` | `0x65b4cf` |
| Ratio-store signature start | `0x652d7c` | `0x65b5dc` |
| TheGameLogic signature (for stats) | `0x64fbce` | `0x6584ce` |
| TheNetwork signature `39 2D <TheNetwork> 8B 0D <&LogicFPS>` (in main loop) | `0x652d34` → `0xcecf3c` | `0x65b594` → `0xd00a08` |
| `Camera::setFOV` (signature in §10) | `0x4d96f0` | `0x4e59c0` |
| Draw-module dt `fld [SecondsPerFrame]` (v4) | `0x930e80` → `0xce51f4` | `0x8d2360` → `0xcf8c98` |
| Animation clock, normal path (v5) | `0x627da1` | `0x631761` |
| Animation clock, special path (v5) | `0x627d5d` | `0x63171d` |
| MsPerFrame / AnimTime (v5) | `0xceac2c` / `0xcea848` | `0xcfe71c` / `0xcfe318` |

### Injection
Both games statically import `d3d9.dll` and `DSOUND.dll`. **Chosen: a `dsound.dll` proxy in each game's `Data\` folder**, so the `d3d9.dll` slot stays free for ReShade/DXVK. It forwards system dsound ordinals 1–12 (listed in `src/dsound.def`). Loaded as a static import, so it runs before the game's static initializers.

### Implementation (`RA3FrameMod/src/dllmain.cpp`)
- Built by `build.bat` (VS2022 BuildTools, `vcvars32`) → `bin/dsound.dll` + `bin/RA3FrameMod.ini`.
- Anchor signature `A1 ?? ?? ?? ?? 33 D2 F7 35 ?? ?? ?? ?? 8B F8 83 FF 06 0F 8D` yields both constant addresses; sanity-checks LogicFPS == 15 and MaxUpdateFPS == 30.
- Requires **exactly** 5 ratio sites, 1 main-loop site and 1 ratio-store site; otherwise patches nothing (safe on unknown versions).
- Redirects the 5 ratio sites' operand to `g_maxUpdateFps`.
- Replaces the main-loop `mov esi,[max]` with a call to a flag-preserving stub that loads `g_maxUpdateFps` and counts drawn frames.
- Replaces the ratio store with a stub that rounds 4/5 → 3 and caps at 6.
- A stats thread logs draw fps and logic Hz (`TheGameLogic+0x50` delta) every 5 s.
- INI keys: `Enabled`, `TargetFPS` (30/45/90), `Log`, `LogStats`, `CatchUp` (v2+), `FixAnimationSpeed` (v4 draw-module dt + v5 animation clock), `WidescreenFix`, `FOVPercent` (§10).

### v2: catch-up mode (`CatchUp=1`, offline only; ratio choice superseded by v4 below)
Problem (from test 1, §9): game speed = drawn fps / 6, so any dip below 90 fps slows the game down.

Fix:
- The step() function's ratio site (the anchor hit, `0x63f4f6` in RA3) now reads a **per-frame** `g_stepFps` instead of the static `g_maxUpdateFps`.
- `OnFrame(engine)` runs from the main-loop stub (`edi` = GameEngine) and computes:
  - `done = logicFrame*6 + engine[+0x58]`: substeps actually processed.
  - `expected = anchor + elapsed_QPC * 90 / s`: substeps that should have been processed by now.
  - `behind = expected - done`.
- v2/v3 then picked the substeps for this frame:
  - `behind` 2: step ratio 3, intended 2 substeps.
  - `behind` 3–5: ratio 2, intended 3 substeps.
  - `behind >= 6`: ratio 1, intended 6 substeps (a whole logic tick).
  - Otherwise: normal ratio 6, 1 substep.
  - This under-delivered; see v4.
- If `behind > 12` or `behind < -6`, it **re-anchors**: the debt is dropped. This covers loading screens, pausing, and the ~1% fast drift from integer-ms frame timing.
- Mixed ratios are safe because step() always finishes the current aligned block (block size `6/r`).
- It is disabled whenever the TheNetwork pointer is non-null, so multiplayer cadence is unchanged.
- The stats log also prints the number of catch-up frames per 5 s.

### v4: corrected catch-up ratio selection
- **Root cause of the v3 under-delivery (test 2, §9):** step() only processes from the next sub-step to the **end of its aligned block** (block = `6/ratio`). Requesting ratio 3 in the middle of a block often yields only 1 sub-step.
- **Fix:** `OnFrame` computes `next = (counter >= 6 ? 1 : counter + 1)`.
  - For each ratio in 6/3/2/1 (only ratios ≤ maxRatio), it counts the sub-steps that ratio's block would actually process: `count = ((next-1)/block + 1)*block - next + 1`.
  - It picks the ratio that covers the most sub-steps without exceeding the debt.

### Unit animation speed (`FixAnimationSpeed=1`): v4 attempt, then the v5 real fix
**Symptom (user report):** at 90 fps, unit animations play about 3× too fast ("running in place"). Movement is correct; only the walk cycle is too fast.

#### v4: draw-module dt patch (secondary consumer, *not* the cause)
This did **not** fix the symptom: the user confirmed walk cycles still ran 3× fast after v4, playing Uprising. The patch is kept because the code is still a per-drawn-frame consumer of 1/30 s.
- **Location:** client draw-module virtual `0x930e40`, vtable pointer at `0xc6c6c8`. Same class family as the module constructed at `0x93afb0`, which stores MaxUpdateFPS at `+0x290`.
- **What it does:**
  - If the logic frame changed, it calls `0x916380`.
  - It then loops over the list `[this+0x2d4 .. +0x2d8]` (stride `0xcc`), calling `0x90c0e0(elem[+0x64], dt)` with `dt = fld [0xce51f4]`, a fixed 1/30 s.
  - It is called once per drawn frame.
- **Patch:** that one `fld` now reads `g_animDt`, which uses the same seconds-per-frame value as the v5 clock below.
- **Signature (unique in both games):** `D9 05 <&SecondsPerFrame> 51 8B 4E 64 D9 1C 24 E8 ?? ?? ?? ?? 81 C6 CC 00 00 00`. It matches at RA3 `0x930e80` (→ `0xce51f4`) and Uprising `0x8d2360` (→ `0xcf8c98`).

#### v5: the client animation clock (the real cause)
- **Location:** RA3 fn `0x627b90`, the GameClient-level per-drawn-frame update. Its vtable pointer is at `0xc14be0`. It also hosts the debug-overlay dispatch to `0x613390`, `0x602720`, etc.
- **Normal path, `0x627da1`:**
  `mov ecx,[ceac2c]; mov eax,[cea848]; imul ecx,esi; add eax,ecx; push eax; mov [cea848],eax; call 0x963bb0; … ; call 0x4d7750`
  - `cea848` is the **animation/sync time in ms** (int). It is reset to 0 at `0x620378`. Many W3D functions read it: `0x935066`, `0x937bd7`, `0x938713`, `0x938b50`, `0x938b90`, `0x938c00`, `0x938df0`, `0x93ab20`.
  - `ceac2c` is 33 (int `1000/MaxUpdateFPS`, static init `0xbbbc00`).
  - `esi` is the number of client frames elapsed since the last call: `[ce4c10]->vfunc+0x74` minus `cb891c`. It is forced to 0 when paused (`ce4c10+0xc4 == 0` → `bl`).
  - `0x963bb0` and `0x4d7750` are the render engine's (WW3D-style) `Sync(time)` calls.
  - So the clock advances a fixed 33 ms per drawn frame, which means animations run 3× fast at 90 fps.
- **Special-mode path, `0x627d5d`:** `mov eax,[cea848]; add eax,[ceac2c]; push eax; mov [cea848],eax`. It is taken when a certain flag is set and `frame % 30 != 0`, and it also calls camera fn `0x615a90`.
- **Signatures (unique in both games):**
  - normal: `8B 0D <MsPerFrame> A1 <AnimTime> 0F AF CE 03 C1 50 A3`
  - special: `A1 <AnimTime> 03 05 <MsPerFrame> 50 A3`, built from the addresses found by the normal one.
- **Address map:**

| | RA3 1.13 | Uprising 1.1 |
|---|---|---|
| Normal path | `0x627da1` | `0x631761` |
| Special path | `0x627d5d` | `0x63171d` |
| MsPerFrame (`ceac2c`) | `0xceac2c` | `0xcfe71c` |
| Anim time (`cea848`) | `0xcea848` | `0xcfe318` |

- **v5 fix:** both `MsPerFrame` reads are redirected to `g_animMs`. In `OnFrame`:
  - **Seconds this frame:** `stepped / (6 × LogicFPS)`, where `stepped` = sub-steps since the last frame (1..6).
  - **No logic running** (menus): it uses `1/MaxUpdateFPS` in real time, which matches vanilla behaviour.
  - **Ms clock:** a double-precision ms clock accumulates those seconds. `g_animMs` = the whole-ms delta, and the fraction carries over, so 11.11 ms/frame averages out exactly.
  - **Draw-module dt:** `g_animDt` (v4) uses the same seconds value.
  - **Pause:** still freezes, because the engine multiplies by `esi = 0`.

### Self-verification
The mod's log reports measured draw fps vs logic ticks/sec; logic must stay at 15/s. Replays (deterministic) are a strong desync test. Note: the main menu turned out to be a **video** (logic not running, 0 Hz), so verification needs a skirmish/campaign, not the menu.

---

## 9. Live test results

### Test 1 — RA3 1.13, mod v1, 2026-10-27
- **Setup:** user's PC (RTX 5090, 3440×1440 @ 164 Hz), `TargetFPS=90`. Full log at `docs/test1_ra3_v1.log`.
- **Patching:** every signature matched and all patches applied. No crash.
- **Main menu:** it's a video, so logic isn't running (0 Hz).
- **Skirmish:** draw rate was 84–90.8 fps, and logic was **exactly draw / 6** in every sample:

| Draw fps | Logic Hz |
|---|---|
| 90.2 | 15.00 |
| 90.8 | 15.2 |
| 76 | 12.8 |
| 71.6 | 11.8 |

- **Conclusion:** the cadence works and logic per frame is unchanged. However, game speed follows drawn frames, so a dip below 90 means proportional slow motion. Vanilla behaves the same below 30. This led to the v2 catch-up mode (§8).

### Test 2 — RA3 1.13, mod v3 (catch-up), 2026-10-27
- **Result:** catch-up works but **under-delivers**. At 84 fps logic ran at 14.4–15.0 Hz; at 66.6 fps it ran at 11.6 Hz.
- **Root cause:** the aligned-block behaviour of step(); fixed in v4 (§8).
- **Log:** `docs/test2_ra3_v3.log`. Totals: 124 samples, average 88.8 fps draw, average logic 14.98 Hz.
- **Not a bug:** one sample showed 49.6 fps and another 0 fps. That was the user alt-tabbing out, since the main loop stops while the window is unfocused.
- **User report from the same session:** unit walk animations ran about 3× too fast at 90 fps. This led to the v4 draw-module dt patch (§8).

### User report after v4 (Uprising)
- Walk animations were still 3× fast, so the v4 dt patch was not the cause.
- The real cause is the client animation clock `cea848 += ceac2c × frames`, fixed in v5 (§8).

### Test 3 — Uprising 1.1, mod v5, 2026-10-27
- **Log:** `docs/test3_uprising_v5.log`. Every patch applied:
  - draw-module dt (animation speed fix) at `0x8d2360`
  - animation clock at `0x631761`, plus the alternate path
  - FOV hook at `0x4e59c0`
  - all the FPS cadence patches
- **Numbers:** 68 in-game samples; average draw 88.6 fps; average logic **15.01 Hz**; 248 catch-up frames. Full game speed held.
- **User verdict:** "animations look normal now". The 3× walk-cycle issue is **resolved**.
- **Not yet done:** RA3 1.13 has not been run with v5; v5 has only been tested in Uprising so far.

### Pending
- RA3 1.13 run with v5.
- Camera move speed at 90 fps (`ceac2c` readers in the camera code, §14).
- Particle and effect speed at 90 fps.
- FOV/HUD feedback on the ultrawide fix (§10).
- Online play (untested).

---

## 10. Ultrawide FOV fix

### Findings
- The game is **Vert-**. Horizontal FOV is fixed, so wider aspect ratios cut the vertical view, and 3440×1440 looks "zoomed in". That was the user's complaint after the `options.ini` resolution change (§12).
- **`Camera::setFOV(float hfov, float vfov)`** is at RA3 `0x4d96f0` and Uprising `0x4e59c0`. Signature (unique in both): `D9 44 24 04 F3 0F 10 44 24 08 D9 05 ?? ?? ?? ?? 0F 2E 05 ?? ?? ?? ?? DC C9 9F F6 C4 44`.
  - If `vfov == -1`, it derives `tan(v/2) = tan(h/2) / aspect`. Aspect is at `camera+0x210`, set by `0x4d97a0` (`setAspect`).
  - It stores the frustum extents at `camera+0x200 … +0x20c`.
  - Callers: `0x4df7d3`, `0x5ed336`, `0x615928`, `0x615985`, `0x93b9ac`.
- **Main view camera** (around fn `0x5ed200`, also `0x6158f7` / `0x615951`):
  - `hfov = 50.0 × DEG2RAD × (isWidescreen ? 1.33 : 1.0) × (viewport / width)`.
  - 50.0 is const `0xbed6ac`, DEG2RAD is `0xcb8968` (0.0174533), 1.33 is `0xc13cc8`, 1.0 is `0xbed250`.
  - isWidescreen is `TheDisplay->vfunc+0x50()`. TheDisplay = `[0xce4bdc]`; its vfunc `+0x44` / `+0x48` give width / height.
- **RA3Uprising-DeFlawless (third party):**
  - It hooks the `fld [deg2rad]` after the 1.33/1.0 select and applies a Hor+ formula to the 1.33 factor as if it were an angle in radians.
  - That is mathematically wrong, so we did not use it.

### Our fix (`WidescreenFix=1`, `FOVPercent=100`; independent of the FPS patches)
- Hook the `setFOV` entry: the first 10 bytes are overwritten with a jmp plus nops.
- The stub calls `AdjustFov(args, camera)`, re-executes the overwritten `fld [esp+4]; movss xmm0,[esp+8]`, then jumps to entry+10.
- If `vfov == -1` and `aspect > 16/9`: `h' = 2·atan(tan(h/2) · (aspect / (16/9)) · FOVPercent/100)`, capped at 150°.
- At 3440×1440 the horizontal FOV goes from 66.5° to about 83°.

---

## 11. Other strings and leads

Debug format strings (RA3 1.13 VAs) and code refs:

| String VA | Text | Referenced at |
|---|---|---|
| `0xc146a0` | `logic FPS: %03.1f, TheGameInfo is null` | `0x6029e7` |
| `0xc146f0` | `logic FPS: %03.1f, Frame: %d/%d -- VER=%d, DataCRC=%d, cmdCRC=%d` | `0x6029c9` |
| `0xc147d8` | `FPS:%2.2f (%3d%% max) FPSLimit:NA, SCALAR:… FSkipped… Slept… Delta…` | `0x6028fd` |
| `0xc14890` | `FPS:%2.2f (%3d%% max) FPSLimit:%d, SCALAR:%1.2f, FSkipped:%d, Slept:%02dms, Delta:%02dms` | `0x6028b0` |
| `0xc15448` | `FPS: %.2f` | `0x613618` |
| `0xc1545c` | `, FPSLock %d` | `0x6135d9` |
| `0xc15478` | `FPS: %.2f, %.2fms draws: %d (%s)` | `0x6135c8` |
| `0xc1844c` / `0xc18460` | `Instant_Net_FPS` / `Instant_Average_FPS` | `0x632ff4` / `0x632fec` |
| `0xc18978` | `* Times are in Game Seconds which are based on logic FPS: current logic FPS is %d, max update FPS (game speed) is %d` | `0x636d1c` (fn `0x636c80`) |

- Debug-info function `0x602720` (prints FPS, SCALAR, skipped/slept, logic FPS, run-ahead, bandwidth, player status).
- Fn `0x613390` — draw-stats string ("FPS: %.2f, %.2fms draws", ", FPSLock %d").
- `EnableVSync` (`0xc17fd8`) refs: `0x631056`, `0x631095`, `0x64957a`, `0x64bb3a`.
- `GameSpeedFactor` (`0xc23c60`) refs: `0x6e7782`, `0x6e78b9`. `RULE:GameSpeed` (`0xc655b8`) ref at `0x87b250` (data).
- Camera zoom strings (future zoom mod): `LOCK_CAMERA_ZOOM`, `CAMERA_MOD_SET_FINAL_ZOOM`, `ZOOM_CAMERA`, `CAMERA_ZOOM_DISTANCE`, `MSG_NOTIFY_ZOOM_DISTANCE_UPDATED` (file offsets ~`0x7fe7f4`–`0x817778`).
- Uprising has the same strings at different offsets (e.g. logic-FPS line at file `0x8251c8`, `GameSpeedFactor` `0x82fe10`).
- Game data INI is precompiled into binary assets, so INI field names (e.g. camera height) are not strings in the exe.

---

## 12. User config changes already made

- `options.ini` locations: `%APPDATA%\Red Alert 3\Profiles\<name>\options.ini` and `%APPDATA%\Red Alert 3 Uprising\Profiles\<name>\options.ini`.
- Changed `Resolution = 2560 1440` → `Resolution = 3440 1440` in both; backups saved as `options.ini.bak` next to each.
- `EnableVSync = 1` in both.
- Side effect: the game is Vert- (§10), so 3440×1440 looked "zoomed in" to the user. That motivated the FOV fix.
- Hardware: RTX 5090, 3440×1440 @ 164 Hz.

---

## 13. Web research summary

- **PCGamingWiki** — 30 fps cap with game speed tied to frame rate; FOV hackable via Flawless Widescreen; custom resolution via `options.ini`; BegoneCrashers for online crashes; Steam/EA app are v1.13 and no longer need the quick-loader fix. <https://www.pcgamingwiki.com/wiki/Command_%26_Conquer:_Red_Alert_3>
- **SageMetaTool** (MetaIdea) — closed/Patreon ($6 tier) launcher claiming "fluent 60 FPS with proper gamespeed", widescreen, unlimited zoom; supports RA3/Uprising Steam + EA app; WIP; advises capping at 60 in GPU driver if uncapped. <https://github.com/MetaIdea/SageMetaTool>, <https://github.com/MetaIdea/SageMetaTool/wiki>, <https://www.patreon.com/posts/sagemetatool-key-52796723>
- **Revora thread** — someone disabled the client limiter; game ran too fast because of the 30-tick limit. <https://forums.revora.net/topic/114508-30-fps-lock-disable/>
- **Flawless Widescreen** — FOV/zoom tool. <https://www.flawlesswidescreen.org>
- **BegoneCrashers** — open-source online crash fix. <https://github.com/lanyizi/BegoneCrashers>
- **CnC-Online client** — replacement multiplayer service. <https://github.com/xan105/CnC-Online>
- **RA3Uprising-DeFlawless** — small ChatGPT-authored Uprising FOV ASI. Its formula is wrong: it treats the 1.33 factor as radians (§10). Not used. <https://github.com/xatornet/RA3Uprising-DeFlawless>
- Note: the SageMetaTool "60 FPS" claim conflicts with §6 (ratio 4 is broken in vanilla code); they presumably rewrote the scheduler or meant something else.

---

## 14. Open questions / TODO

- [ ] Do client-side systems (particles, Drawable animation `0x568670`, W3D readers in `0x93xxxx`–`0xabxxxx`, camera scroll, UI) assume 30 Hz per draw frame? Verify in testing at 90 fps; classify remaining "unknown" readers in §7.
- [ ] **Camera speed (next candidate fix, still unverified):**
  - camera fn `0x615a90` reads `ceac2c` at `0x615b11`, `0x615b56` and `0x615c19`
  - camera-transition code (`0x5e3fa0`, `0x5ed760`, …) computes frame counts as `ms / ceac2c`
  - Do camera moves and scripted transitions run 3× fast at 90 fps? If so, redirect those reads too, e.g. to `1000/target` or `g_animMs`. Don't patch `ceac2c` globally; the v5 clock already handles its own reads.
- [x] Catch-up (v4+) holds game speed: test 3 averaged 15.01 Hz logic at 88.6 fps, with 248 catch-up frames. Still worth watching heavy late-game battles for stutter.
- [x] v5 animation-clock fix confirmed in Uprising (test 3: "animations look normal now").
- [ ] Run RA3 1.13 with v5. So far only Uprising has been tested with v5.
- [x] v4 draw-module dt patch: confirmed *not* the cause of the 3× walk cycles (user, Uprising). Kept as a secondary consumer.
- [ ] Check particle/effect speed at 90 fps. If it's still fast, classify the other `ce51f4` readers (`0x555e10`, `0x614530`, `0x6145c0`, `0x6e6d70`, `0x90afa0`, `0x90d290`) and the client readers of `ce5110`/`ce5114`/`ce511c` (e.g. `FxpsGPUStorageModule` `0x6ea710`). Do not touch logic readers such as `SlaveWatcherBehavior` `0x7b2890`.
- [ ] FOV/HUD feedback still pending: HUD layout, map edges, and object culling at the screen sides at ~83° horizontal FOV.
- [x] Uprising live test (test 3, v5): all patches applied, full speed held, animations normal. FOV visual feedback is still open (item above).
- [x] Test 2 (RA3, v3): catch-up under-delivery diagnosed (aligned blocks); fixed in v4.
- [x] Ratio ≥ 6 branch of the last-subframe check: `0x628c90` returns `counter == 1`, consistent with one substep per frame.
- [x] Uprising addresses found with the same signatures (§8).
- [x] Static imports confirmed: `d3d9.dll` + `DSOUND.dll`; proxy = `dsound.dll`.
- [x] Test 1 (RA3, v1): cadence and logic-per-frame confirmed; game speed tracks draw fps (§9).
- [ ] Check timer resolution (`0xb84010`) and whether `timeBeginPeriod(1)` is active, since 11 ms frames need ms precision.
- [ ] Online play is untested. Verify the adaptive-ratio clamp and SCALAR behave, and that there's no desync with unmodded players. Catch-up is already disabled when TheNetwork is non-null.
- [ ] Camera zoom limits (future feature).
- [ ] Other graphics improvements (anisotropic filtering, etc.) via the d3d9 proxy.
