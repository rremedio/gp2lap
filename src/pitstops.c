#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pitstops.h"
#include "miscahf.h"        // IDAtoFlat
#include "cfgmain.h"        // GetCfgString
#include "basiclog.h"       // LogLine / strbuf

/* Pit-stop duration is max(tyre, refuel, damage); each is base + RNG (see docs/pit-stops.md). All the
   knobs are static code immediates plus one static data value (the refuel rate divisor) plus two byte
   flips for "disable refuel" -- none rebuilt at runtime, so we patch once at init, opcode-guarded.
   Read from the SeasonOverrides override file; keys absent -> stock untouched. Global (AI + player). */

unsigned long PitStopsActive = 0;

/* If left-trimmed 'p' is "name = value" (name case-insensitive, followed by ws or '='), set *val
   (decimal or 0x..) and return 1. The full-name + ws/'=' check rules out prefix collisions
   (e.g. PitRefuelSpeed vs PitRefuelBaseMs). */
static int PitKV(const char *p, const char *name, long *val)
{
  int i;
  const char *q;
  for (i = 0; name[i]; i++)
    if ((p[i] | 0x20) != (name[i] | 0x20)) return 0;
  q = p + i;
  while (*q == ' ' || *q == '\t') q++;
  if (*q != '=') return 0;
  q++;
  while (*q == ' ' || *q == '\t') q++;
  *val = strtol(q, (char **)0, 0);
  return 1;
}

/* Patch an `add eax,imm32` (op 0x05) / `cmp eax,imm32` (op 0x3D) immediate, verifying the opcode and
   the stock immediate first. Returns 1 if applied. */
static int PitPatchImm(unsigned long ida, unsigned char op, unsigned long stock,
                       long newval, const char *what)
{
  unsigned char *p = (unsigned char *)IDAtoFlat(ida);
  if (newval < 0) newval = 0;
  if (newval > 120000) newval = 120000;
  if (p[0] != op || *(unsigned long *)(p + 1) != stock) {
    sprintf(strbuf, "- PitStops: %s opcode mismatch @0x%lX; skipped\n", what, ida);
    LogLine(strbuf);
    return 0;
  }
  *(unsigned long *)(p + 1) = (unsigned long)newval;
  sprintf(strbuf, "- PitStops: %s -> %ld ms\n", what, newval);
  LogLine(strbuf);
  return 1;
}

void PitStopsInit(void)
{
  char *cfg;
  FILE *f;
  char line[300];
  long v;
  long refuelSpeed = 0, refuelBase = 0, refuelCap = 0, tyreMs = 0, damageMs = 0, disableRefuel = 0;
  int hSpeed = 0, hBase = 0, hCap = 0, hTyre = 0, hDmg = 0, hDis = 0;
  int applied = 0;

  cfg = GetCfgString("SeasonOverrides");
  if (!cfg || !cfg[0]) return;                 /* no override file -> feature off */

  f = fopen(cfg, "rb");
  if (!f) return;
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (*p==';' || *p=='#' || *p=='[' || *p=='\r' || *p=='\n' || *p==0) continue;
    if      (PitKV(p, "PitRefuelSpeed",    &v)) { refuelSpeed = v;  hSpeed = 1; }
    else if (PitKV(p, "PitRefuelBaseMs",   &v)) { refuelBase  = v;  hBase  = 1; }
    else if (PitKV(p, "PitRefuelCapMs",    &v)) { refuelCap   = v;  hCap   = 1; }
    else if (PitKV(p, "PitTyreChangeMs",   &v)) { tyreMs      = v;  hTyre  = 1; }
    else if (PitKV(p, "PitDamageRepairMs", &v)) { damageMs    = v;  hDmg   = 1; }
    else if (PitKV(p, "DisableRefuel",     &v)) { disableRefuel = v; hDis  = 1; }
  }
  fclose(f);

  if (!(hSpeed || hBase || hCap || hTyre || hDmg || hDis)) return;   /* no pit keys present */

  if (hTyre) applied += PitPatchImm(0x2A3A5, 0x05, 0x1B58, tyreMs,    "tyre change base");
  if (hDmg)  applied += PitPatchImm(0x2A3FD, 0x05, 0x4650, damageMs,  "damage repair base");
  if (hBase) applied += PitPatchImm(0x2A2A4, 0x05, 0x0ABE, refuelBase,"refuel base");
  if (hCap)  applied += PitPatchImm(0x2A26C, 0x3D, 0x4E20, refuelCap, "refuel cap");

  /* refuel rate: dword_D5C3E is a DIVISOR (higher = faster). Bootstrap its runtime address from the
     read-site `mov ecx, dword_D5C3E` (8B 0D <addr>) @0x2A259, then set it to 120 * speed% (stock
     0x2EE0 = 12000 = 120 * 100). */
  if (hSpeed) {
    unsigned char *r = (unsigned char *)IDAtoFlat(0x2A259);
    if (refuelSpeed < 10)   refuelSpeed = 10;
    if (refuelSpeed > 1000) refuelSpeed = 1000;
    if (r[0] == 0x8B && r[1] == 0x0D) {
      unsigned long *pRate = *(unsigned long **)(r + 2);   /* runtime &dword_D5C3E */
      if (*pRate == 0x2EE0) {
        *pRate = 120UL * (unsigned long)refuelSpeed;
        sprintf(strbuf, "- PitStops: refuel speed -> %ld%% (D5C3E=0x%lX)\n", refuelSpeed, *pRate);
        LogLine(strbuf);
        applied++;
      } else LogLine("- PitStops: refuel rate value mismatch; skipped\n");
    } else LogLine("- PitStops: refuel rate opcode mismatch; skipped\n");
  }

  /* disable refuel -- three NOPs (let sub_2C58B RUN so the pit-stop index [esi+0xD7]++ still happens;
     returning early made cars re-pit every other lap). We just neutralise the fuel writes:
     (1) FULL START: SetFuelLapsESI's race branch 0x2C559 `jnz loc_2C567` (75 0C, taken when
         numPitStops != 0 -> stint fuel); the fall-through (0 stops) is the raceLaps+2 FULL path.
         NOP the jnz so every car starts full.
     (2) NO DEFUEL: sub_2C58B rewrites the fuel load `mov [esi+0x306], ax` @0x2C631 to the planned
         next-STINT amount -- less than a full tank -- so the stop would drop the car's fuel. NOP it so
         the tank stays full through the stop.
     (3) NO ADD: NOP the fuel-amount write `mov dword_D5C46, eax` @0x2C651 so D5C46 stays at its
         entry-init 0. */
  if (hDis && disableRefuel) {
    unsigned char *j = (unsigned char *)IDAtoFlat(0x2C559);   /* jnz to stint-fuel    */
    unsigned char *w = (unsigned char *)IDAtoFlat(0x2C631);   /* mov [esi+0x306], ax  */
    unsigned char *z = (unsigned char *)IDAtoFlat(0x2C651);   /* mov dword_D5C46, eax */
    if (j[0]==0x75 && j[1]==0x0C &&
        w[0]==0x66 && w[1]==0x89 && w[2]==0x86 && w[3]==0x06 && w[4]==0x03 &&
        z[0]==0xA3 && z[1]==0x46) {
      j[0] = 0x90; j[1] = 0x90;     /* full start  */
      memset(w, 0x90, 7);           /* no defuel: keep fuel load at full through the stop */
      memset(z, 0x90, 5);           /* no add:     D5C46 stays 0 */
      LogLine("- PitStops: refuel DISABLED (cars start full, no fuel change; tyre stops remain)\n");
      applied++;
    } else LogLine("- PitStops: DisableRefuel opcode mismatch; skipped\n");
  }

  if (applied) PitStopsActive = 1;
}
