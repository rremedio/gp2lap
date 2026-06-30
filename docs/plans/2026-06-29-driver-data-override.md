# Driver / Team Data Override Loading — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Load per-driver (name, skill qual+race, random range B + weight A, number, selected, disabled)
and per-team (race/qual power, reliability, 14 pit-crew colours) data from the `SeasonOverrides` file, and
make the engine tolerate <26 valid drivers, so a full carset can be authored without GP2Edit.

**Architecture:** Extend the existing `src/override.c` INI parser/model (host-testable via
`tests/test_override.c`) with the new `[Team N]` keys, then add apply modules that patch GP2.EXE data
tables at the `AHFAfterGp2Init` hook (and, where the per-event save reload requires it, re-apply after the
reload). Per-driver number/selected/disabled collapse into one packed `t_CaridTeamTab` byte; per-driver
name/skill/range/weight and per-team power/reliability/pit-crew are plain data-table writes via
`IDAtoFlat`. The <26-grid fix redirects six hard-`26` code immediates to `min(N,26)`.

**Tech Stack:** C89 (Watcom `wcc386` for DOS; host build with `cc` for parser tests), x86 asm
(`src/lammcall.asm` trampolines), DOS4G. Patch helpers `IDAtoFlat` / `IDACodeReftoDataRef` (`src/miscahf.c`).

**Design source:** `vaults/gp2/docs/plans/2026-06-29-gp2lap-driver-override-design.md`; RE in
`vaults/gp2/docs/driver-data.md` + `grid-team-capacity.md`.

**Commit policy:** The repo owner commits only on explicit request. Treat each "Commit" step as a
checkpoint — stage the change and **ask** before committing (or batch as the owner directs).

**Worktree:** `/home/rremedio/worktrees/gp2lap/driver-data-override` (branch `driver-data-override` off `dev`).

**Baseline:** host parser test passes today —
`cc -std=c89 -Wall -I src -DOVERRIDE_HOSTTEST tests/test_override.c src/override.c -o /tmp/t_override && /tmp/t_override` → `ALL PASS`.

**Addresses (IDA; data writes via `IDAtoFlat(addr)`):**
- names `t_DriverNames` `0x179026` (40×24), skill `word_1745E8` (40×4: qual@+0, race@+2),
  range/weight `word_174688` (40×4: B@+0, A@+2), team table `t_CaridTeamTab` `0x178F9A` (40 B).
- team power `t_TeamPerfValue` `0x174598`, qual power `word_1745C0`, reliability `t_teamwhat` `0x174728`.
- pit-crew `t_PitCrewColors` `0x183338` (14×16), expander `MakeCrewColors` `0x391ED` (call @`0x39111`).
- grid hard-26 sites: `0x2C1AD`, `0x2C1C7`, `0x14E0F`, `0x2C320`, leader-search (~`0x14F87`), results `0x82A3D`.

**Encodings:** skill 0..17016 → `rating = skill + 0x3D87`; range 0..32767 raw; weight 0..16384 raw;
power/qualpower PS 0..1579 → `value = PS + 0x8031`; reliability 0..32767 raw; pit-crew 14 bytes → ramp
bases `t_PitCrewColors[team*16 + 2..15]`; packed slot byte `disabled ? 0 : (num&0x3F)|(selected?0x80:0)`.

---

## Task 1: Verify per-event save-reload timing (gate for apply architecture)

No code yet — this decides whether driver data can be a one-time startup patch or needs a per-event
re-apply hook. Discrepancy to resolve: documented save block `0x1770E0..0x179DD7` puts skill/range/
team-perf *below* it (startup-persistent), but `grid-team-capacity.md` §6 says ratings are re-read each
event via `sub_2D71F`.

**Step 1:** In the listing `vaults/gp2/new-export-from-selection-annotated.lst`, read `sub_6B88A` (the
savegame-block loader) and confirm its destination range — does it write into `0x1745E8` / `0x174688` /
`0x174598` / `0x178F9A` / `0x179026`, or only a subset?

**Step 2:** Read `sub_2D71F` — confirm it *reads* the ratings (`word_1745E8`) to rebuild derived tables
and does not overwrite the source ratings.

**DECISION (RE complete 2026-06-29 — VERIFIED):**
- `sub_6B88A` is the **saver** (block→buffer), not the loader. The real loader is **`RestoreGameState`
  @0x6BD46** (`rep movsb` into the save block `[0x1770E0, 0x179DD7)`), called from 0x6B600 / 0x6BBFB /
  0x6BE24 (.SAV restore); a network-sync path at 0x6C240 copies the same name+caridtab chunk.
- **Startup-only patch is SAFE** for: **skill** 0x1745E8, **range+weight** 0x174688, **team-perf**
  0x174598, **team-qual** 0x1745C0, **reliability** 0x174728. These are below the save block, have **no
  runtime writers**, and `sub_2D71F` only *reads* them at event entry to rebuild the derived HP/wing
  tables — so a startup patch propagates every event automatically.
- **NEEDS post-restore re-apply** for: **driver names** 0x179026 and **`t_CaridTeamTab`** 0x178F9A —
  both inside the save block, overwritten by `RestoreGameState` (and the 0x6C240 sync path) whenever a
  save/network state is restored. A pure new-event start leaves them intact, but re-apply after every
  restore to be safe.
- **Re-apply hook:** redirect the restore call sites (0x6B600 / 0x6BBFB / 0x6BE24, each a 5-byte `E8`
  call) to a trampoline = `call RestoreGameState; then re-apply names + t_CaridTeamTab` (the GP2Lap
  call-replacement pattern, cleanest). Alternative single site: hook `RestoreGameState` 0x6BD46 entry
  (stock `BF E0 70 17 00`) and replicate the block copy + re-apply. Only the name + caridtab writes need
  this; skill/range/team-perf do not.

---

## Task 2: Extend the override model (`OvDriver` + `OvTeam` fields)

**Files:** Modify `src/override.h`.

**Step 1:** Add a per-slot driver struct and extend `OvTeam`:

```c
typedef struct {                 /* one team seat (slot 0/1) */
  char name[24];   int nameSet;  /* up to 23 chars + NUL */
  long qual, race; int qualSet, raceSet;  /* skill 0..17016 */
  long range;      int rangeSet; /* B, 0..32767 */
  long weight;     int weightSet;/* A, 0..16384 */
  int  num;        int numSet;   /* carId/number 1..40 */
  int  selected;   int selectedSet;
  int  disabled;   int disabledSet;
} OvDriver;

typedef struct {               /* [Team N], indexed [N-1] */
  char shape[256]; int shapeSet;
  int  nose;       int noseSet;
  long mass;       int massSet;
  long downforce;  int dfSet;
  OvDriver drv[2];                       /* NEW: per-seat driver data */
  long power, qualpower; int powerSet, qualpowerSet;  /* NEW: PS 0..1579 */
  long reliability;      int reliabilitySet;          /* NEW: 0..32767 */
  unsigned char pitcrew[14]; int pitcrewSet;          /* NEW: 14 ramp bases */
} OvTeam;
```

**Step 2:** Build the host test to confirm it still compiles:
`cc -std=c89 -Wall -I src -DOVERRIDE_HOSTTEST tests/test_override.c src/override.c -o /tmp/t_override`
Expected: builds, `ALL PASS` (no behaviour change yet).

**Step 3 (Commit checkpoint):** `git add src/override.h` — ask before commit.

---

## Task 3: Parse the per-seat driver keys (TDD)

**Files:** Modify `tests/test_override.c` (failing test first), then `src/override.c`.

**Step 1: Write the failing test.** Add to `FILETXT` under `[Team 12]`:
```
name1 = "Jean Alesi"
qual1 = 15980
race1 = 15500
range1 = 1342
weight1 = 16384
num1 = 27
selected1 = 1
disabled2 = 1
```
and assertions in `main`:
```c
t = OverrideTeam(12);
CHECK(t->drv[0].nameSet && strcmp(t->drv[0].name, "Jean Alesi") == 0);
CHECK(t->drv[0].qualSet && t->drv[0].qual == 15980);
CHECK(t->drv[0].raceSet && t->drv[0].race == 15500);
CHECK(t->drv[0].rangeSet && t->drv[0].range == 1342);
CHECK(t->drv[0].weightSet && t->drv[0].weight == 16384);
CHECK(t->drv[0].numSet && t->drv[0].num == 27);
CHECK(t->drv[0].selectedSet && t->drv[0].selected == 1);
CHECK(t->drv[1].disabledSet && t->drv[1].disabled == 1);
```

**Step 2: Run, verify it fails** (`drv` field missing values / 0).
`cc ... && /tmp/t_override` → FAIL.

**Step 3: Implement** the parse branches in `OverrideParseFile`, `section == 2`. Add a helper to split a
keyed slot suffix and dispatch. Insert before the final `else` (unknown key):
```c
/* per-seat driver keys: <field><1|2> */
else if (slotKey(key, "name", &slot))     { copyval24(g_team[team-1].drv[slot].name, val); g_team[team-1].drv[slot].nameSet=1; }
else if (slotKey(key, "qual", &slot))     { g_team[team-1].drv[slot].qual = strtol(val,0,0); g_team[team-1].drv[slot].qualSet=1; }
else if (slotKey(key, "race", &slot))     { g_team[team-1].drv[slot].race = strtol(val,0,0); g_team[team-1].drv[slot].raceSet=1; }
else if (slotKey(key, "range", &slot))    { g_team[team-1].drv[slot].range = strtol(val,0,0); g_team[team-1].drv[slot].rangeSet=1; }
else if (slotKey(key, "weight", &slot))   { g_team[team-1].drv[slot].weight = strtol(val,0,0); g_team[team-1].drv[slot].weightSet=1; }
else if (slotKey(key, "num", &slot))      { g_team[team-1].drv[slot].num = atoi(val); g_team[team-1].drv[slot].numSet=1; }
else if (slotKey(key, "selected", &slot)) { g_team[team-1].drv[slot].selected = (atoi(val)!=0); g_team[team-1].drv[slot].selectedSet=1; }
else if (slotKey(key, "disabled", &slot)) { g_team[team-1].drv[slot].disabled = (atoi(val)!=0); g_team[team-1].drv[slot].disabledSet=1; }
```
with helpers near the other text helpers:
```c
/* match "<base><1|2>"; on hit set *slot to 0/1 and return 1 */
static int slotKey(const char *key, const char *base, int *slot) {
  size_t n = strlen(base);
  if (strncmp(key, base, n) != 0) {       /* case-insensitive: compare via ieq on a temp */
    /* use a small ci compare instead */
  }
  if (!ci_startswith(key, base)) return 0;
  if ((key[n]=='1' || key[n]=='2') && key[n+1]==0) { *slot = key[n]-'1'; return 1; }
  return 0;
}
static int ci_startswith(const char *s, const char *pre){ while(*pre){ if(((*s)|0x20)!=((*pre)|0x20))return 0; s++; pre++; } return 1; }
static void copyval24(char *dst, const char *v){ char tmp[256]; copyval(tmp,v); strncpy(dst,tmp,23); dst[23]=0; }
```
Declare `int slot;` at the top of the loop body. **Note ordering:** `num` must be tested before any key
that could prefix-collide — none do here (`num`/`name` differ at char 1), but keep `slotKey` exact-length.

**Step 4: Run, verify pass.** `cc ... && /tmp/t_override` → `ALL PASS`.

**Step 5 (Commit checkpoint):** `git add src/override.c tests/test_override.c` — ask before commit.

---

## Task 4: Parse the per-team power/reliability/pitcrew keys (TDD)

**Files:** `tests/test_override.c`, then `src/override.c`.

**Step 1: Failing test.** Add under `[Team 12]`:
```
power = 765
qualpower = 750
reliability = 2048
pitcrew = 144,146,16,16,146,146,16,146,146,146,146,16,16,16
```
Assertions:
```c
t = OverrideTeam(12);
CHECK(t->powerSet && t->power == 765);
CHECK(t->qualpowerSet && t->qualpower == 750);
CHECK(t->reliabilitySet && t->reliability == 2048);
CHECK(t->pitcrewSet && t->pitcrew[0]==144 && t->pitcrew[13]==16);
```

**Step 2: Run, fail.**

**Step 3: Implement.** Add a 14-value list parser (mirror `parsetriple`) and branches:
```c
static int parselist(const char *v, unsigned char *out, int n) {
  char *q=(char*)v; long b; int i;
  for(i=0;i<n;i++){ while(*q==' '||*q=='\t'||*q=='"')q++; b=strtol(q,&q,0);
    if(b<0||b>255)return 0; out[i]=(unsigned char)b; while(*q==' '||*q=='\t')q++;
    if(i<n-1){ if(*q!=',')return 0; q++; } } return 1; }
```
```c
else if (ieq(key,"power"))       { g_team[team-1].power = strtol(val,0,0); g_team[team-1].powerSet=1; }
else if (ieq(key,"qualpower"))   { g_team[team-1].qualpower = strtol(val,0,0); g_team[team-1].qualpowerSet=1; }
else if (ieq(key,"reliability")) { g_team[team-1].reliability = strtol(val,0,0); g_team[team-1].reliabilitySet=1; }
else if (ieq(key,"pitcrew"))     { if (parselist(val, g_team[team-1].pitcrew, 14)) g_team[team-1].pitcrewSet=1;
                                   else { sprintf(strbuf,"- Override: [Team %d] pitcrew needs 14 values; skipped\n",team); LogLine(strbuf); } }
```

**Step 4: Run, pass.** **Step 5 (Commit checkpoint).**

---

## Task 5: `drvdata.c` — apply driver data to GP2.EXE

Not host-testable (writes GP2.EXE memory) → verify by opcode/value sanity + **in-game**.

**Files:** Create `src/drvdata.c`, `src/drvdata.h`; modify `src/timing/convert.inc`, `makefile.lin`, `makefile`.

**Step 1:** Write `DriverDataInit(void)` (and, per Task 1, a re-apply entry if needed):
- For each team 1..14, slot 0/1 with any `drv` field set:
  - Resolve effective carId: `cid = numSet ? num : (stockTab[(team-1)*2+slot] & 0x3F)`.
  - If `disabled`: write `t_CaridTeamTab` byte = `0x00`; skip the rest for this slot.
  - Else write packed byte: `(cid & 0x3F) | (selectedSet && selected ? 0x80 : keep-stock-bit7)`.
    (Decide: `selected` absent → preserve stock bit7; `selected=0` → clear it.)
  - `idx = cid - 1`; if `idx < 0` → warn + skip data writes.
  - `nameSet`: `memcpy(IDAtoFlat(0x179026)+idx*24, name, 24)` (already NUL-padded to 24 in Task 3? — pad here).
  - `qualSet`: `*(unsigned short*)(IDAtoFlat(0x1745E8)+idx*4+0) = (unsigned short)(clamp(qual,0,17016)+0x3D87)`.
  - `raceSet`: `...+2 = clamp(race,0,17016)+0x3D87`.
  - `rangeSet`: `*(unsigned short*)(IDAtoFlat(0x174688)+idx*4+0) = clamp(range,0,32767)`.
  - `weightSet`: `...+2 = clamp(weight,0,16384)`.
- Validation: reject duplicate `num` across slots (warn, keep first); warn that on-car painted digits
  need the .tex edited; clamp + warn on out-of-range.
- Bootstrap `stockTab` once via `IDACodeReftoDataRef(0x65D67)` (as `override.c` does) — read it **before**
  writing it, so resolution uses the pre-override carIds.
- Keep selection's packed save copy `unk_0_4CBF7C` coherent if bit7 changes (per Task 1 timing).

**Step 2:** Verify-before-write sanity: read one stock value (e.g. an existing rating) and confirm it's in
a plausible range before patching; `LogLine` a summary (`drivers patched: N`).

**Step 3:** Wire it up. Structure the apply as two entry points (per the Task 1 decision):
- `DriverDataInit()` — full apply (packed `t_CaridTeamTab` byte + names + skill + range + weight),
  called from `convert.inc` `AHFAfterGp2Init` after `OverrideLoad();`. Skill/range/weight are
  startup-safe and need nothing more.
- `DriverDataReapply()` — re-write only **names** (0x179026) + the packed **`t_CaridTeamTab`** byte
  (0x178F9A), because `RestoreGameState` clobbers the save block. Install via an asm trampoline on the
  restore call sites **0x6B600 / 0x6BBFB / 0x6BE24** (5-byte `E8` calls → stub that calls the original
  `RestoreGameState` then `DriverDataReapply()`); or hook `RestoreGameState` 0x6BD46 entry. Verify the
  stock call/opcode bytes before patching; all-or-nothing.
Add `drvdata.obj` to `OBJS` in `makefile.lin` and `makefile`.

**Step 4: Build (DOS toolchain).** `wmake -f makefile.lin clean && wmake -f makefile.lin` (the `.inc`/`.h`
edits require `clean` — no dependency tracking). Expected: links, `gp2lap` built.

**Step 5: In-game test.** Author an `override.cfg` renaming/re-skilling one driver; run a quick race;
confirm the new name in standings and altered pace; confirm GP2LAP.LOG shows "drivers patched". Verify it
survives an event change (validates Task 1 timing).

**Step 6 (Commit checkpoint).**

---

## Task 6: Team power / qual-power / reliability apply

**Files:** Extend `src/teamphys.c` (already the per-team patcher) or create `src/teamperf.c`; `convert.inc`,
`makefile*`.

**Step 1:** For each team with `powerSet`/`qualpowerSet`/`reliabilitySet`:
- `power`: `*(unsigned short*)(IDAtoFlat(0x174598)+(team-1)*2) = (unsigned short)(clamp(power,0,1579)+0x8031)`.
- `qualpower`: `*(unsigned short*)(IDAtoFlat(0x1745C0)+(team-1)*2) = clamp(qualpower,0,1579)+0x8031`.
- `reliability`: `*(unsigned short*)(IDAtoFlat(0x174728)+(team-1)*2) = clamp(reliability,0,32767)`.
  (Stride is 2 bytes/team, team index 0..13; verified vs driver-data.md worked examples.)

**Step 2:** Wire init + makefile. **Step 3: Build.** **Step 4: In-game** — verify a team's pace/failure
rate changes. **Step 5 (Commit checkpoint).**

---

## Task 7: Pit-crew colours apply

**Files:** Create `src/pitcrew.c`/`.h`; `convert.inc`, `makefile*`.

**Step 1:** `PitCrewColorsInit`: for each team with `pitcrewSet`, verify the two fixed bases first
(`base[0]==0x00 && base[1]==0x10` at `IDAtoFlat(0x183338)+team*16`); if mismatch, `LogLine` + skip that
team. Then write the 14 values to `IDAtoFlat(0x183338)+team*16+2 .. +15`.

**Step 2:** Force the expander to rebuild: simplest = trampoline the `call MakeCrewColors` at `0x39111`
so our patch is applied just before each expansion; or call our own re-expansion replicating
`NewCrew[team*256+c] = base[team*16+(c>>4)] + (c&0x0F)` after patching. Verify the call opcode at `0x39111`
before patching (follow the `cartex.c` verify-then-patch pattern).

**Step 3:** Wire + build. **Step 4: In-game** — pit during a stop, confirm recoloured crew sprites.
**Step 5 (Commit checkpoint).**

---

## Task 8: Grid-shrink fix (tolerate <26 valid drivers)

Isolated, riskiest — its own test phase. Enables >2 disabled.

**Files:** Create `src/gridcap.c`/`.h` (or a block in `convert.inc`); `convert.inc`, `makefile*`.

**Step 1:** Implement a hook that computes `count = min(validCount, 26)` where `validCount` = the value
`sub_14EB4` already wrote to `w_NumCars_26_` before finalize. Redirect the six hard-`26` sites
(`0x2C1AD` copy loop, `0x2C1C7` field-size, `0x14E0F` `InitCarStructs`, `0x2C320` `CopyWhatTable`,
leader-search ~`0x14F87`, results `0x82A3D`) to honour `count`. Recommended: trampoline in/after
`sub_2C19D` to write `min(N,26)` and drive the copy; patch the remaining immediates to read
`w_NumCars_26_`. Zero `t_GridTable[count..25]` defensively. **Verify each stock immediate before
patching; all-or-nothing.**

**Step 2: Build.** **Step 3: In-game** — disable 3+ drivers; confirm a clean smaller grid (no phantom/
duplicate cars, no crash) AND that a stock 0-disabled race is unchanged (fastest-26, 2 DNQ).
**Step 4 (Commit checkpoint).**

---

## Notes / out of scope
- >26-car grids (needs `t_CarStructs` relocation); remapping baked on-car number artwork; mid-season
  championship accounting — all out of scope.
- If Task 1 shows names/`t_CaridTeamTab` are reloaded per event, the per-event re-apply hook is shared by
  Tasks 5 (and any field found to be in the save block).
</content>
