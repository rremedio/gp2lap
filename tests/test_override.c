/* Host unit test for src/override.c (the SeasonOverrides parser/resolver).
   Build + run on Linux (no DOS needed):
     cc -std=c89 -Wall -I src -DOVERRIDE_HOSTTEST tests/test_override.c src/override.c -o /tmp/t_override
     /tmp/t_override
   Exits 0 on all-pass, 1 on any failure. */

#include <stdio.h>
#include <string.h>
#include "override.h"

/* ---- stubs for basiclog (override.c logs through these) ---- */
char strbuf[2048];
void LogLine(char *s) { fputs(s, stdout); }

static int fails = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL: %s  (line %d)\n", #cond, __LINE__); fails++; } } while (0)

/* stock-ish carid/team table, 14 teams x 2 slots; team14 (Osella) slot0 EMPTY, slot1 = #37 */
static const unsigned char TAB[40] = {
  0x01,0x02, 0x03,0x04, 0x05,0x06, 0x07,0x08, 0x09,0x0A,
  0x0B,0x0C, 0x0F,0x10, 0x13,0x14, 0x15,0x16, 0x17,0x18,
  0x19,0x1A, 0x1B,0x1C, 0x1D,0x1E, 0x00,0x25, 0,0, 0,0, 0,0, 0,0, 0,0, 0,0
};

static const char *FILETXT =
  "; sample override\n"
  "[General]\n"
  "PitRefuelSpeed = 100\n"
  "DisableRefuel  = 1\n"
  "bogus = 7              ; unknown key -> ignored\n"
  "\n"
  "[Team 12]   ; Ferrari (#27/#28)\n"
  "car1 = liveries/ferrari_27.bmp\n"
  "car2 = \"liveries/ferrari_28.bmp\"\n"
  "cp1  = 0x20,0x21,0x22\n"
  "Shape = shapes/ferrari.dat\n"      /* mixed-case key */
  "nose = 1\n"
  "mass = 600\n"
  "downforce = 105\n"
  "\n"
  "[Team 14]   ; Osella - 1 driver, in slot 2\n"
  "car1 = should_be_ignored.bmp\n"    /* slot 0 empty -> ignored */
  "car2 = liveries/osella_37.bmp\n"
  "\n"
  "[Team 99]\n"                        /* out of range -> skipped */
  "car1 = nope.bmp\n";

int main(void)
{
  const char *path = "/tmp/_ov_test.cfg";
  const OvGeneral *g;
  const OvTeam *t;
  const OvCar *c;
  FILE *f = fopen(path, "wb");
  if (!f) { printf("cannot write temp file\n"); return 2; }
  fputs(FILETXT, f); fclose(f);

  CHECK(OverrideParseFile(path, TAB) == 0);

  /* [General] */
  g = OverrideGeneral();
  CHECK(g->set & OVG_REFUELSPEED);
  CHECK(g->pitRefuelSpeed == 100);
  CHECK(g->set & OVG_DISABLEREFUEL);
  CHECK(g->disableRefuel == 1);
  CHECK(!(g->set & OVG_REFUELCAP));          /* absent key stays unset */

  /* [Team 12] per-team */
  t = OverrideTeam(12);
  CHECK(t && t->shapeSet && strcmp(t->shape, "shapes/ferrari.dat") == 0);
  CHECK(t->noseSet && t->nose == 1);
  CHECK(t->massSet && t->mass == 600);
  CHECK(t->dfSet && t->downforce == 105);

  /* [Team 12] liveries/cockpit resolved to carIds 27 / 28 */
  c = OverrideCar(27);
  CHECK(c && c->liverySet && strcmp(c->livery, "liveries/ferrari_27.bmp") == 0);
  CHECK(c->cpSet && c->cp[0]==0x20 && c->cp[1]==0x21 && c->cp[2]==0x22);
  c = OverrideCar(28);
  CHECK(c && c->liverySet && strcmp(c->livery, "liveries/ferrari_28.bmp") == 0);  /* quotes stripped */

  /* [Team 14] Osella: car1 (slot 0 empty) ignored, car2 -> #37 */
  c = OverrideCar(37);
  CHECK(c && c->liverySet && strcmp(c->livery, "liveries/osella_37.bmp") == 0);

  /* unrelated car untouched */
  c = OverrideCar(1);
  CHECK(c && !c->liverySet);

  if (fails == 0) printf("\nALL PASS\n");
  else            printf("\n%d FAILURE(S)\n", fails);
  return fails ? 1 : 0;
}
