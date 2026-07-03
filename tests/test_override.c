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
  "name1 = \"Jean Alesi\"\n"
  "qual1 = 15980\n"
  "race1 = 15500\n"
  "range1 = 1342\n"
  "weight1 = 16384\n"
  "num1 = 27\n"
  "selected1 = 1\n"
  "disabled2 = 1\n"
  "power = 765\n"
  "qualpower = 750\n"
  "reliability = 2048\n"
  "pitcrew = 144,146,16,16,146,146,16,146,146,146,146,16,16,16\n"
  "\n"
  "[Team 13]   ; over-count pitcrew (15 values) -> rejected\n"
  "pitcrew = 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15\n"
  "\n"
  "[Team 14]   ; Osella - 1 driver, in slot 2\n"
  "car1 = should_be_ignored.bmp\n"    /* slot 0 empty -> ignored */
  "car2 = liveries/osella_37.bmp\n"
  "\n"
  "[Team 99]\n"                        /* out of range -> skipped */
  "car1 = nope.bmp\n"
  "\n"
  "[Calendar]\n"                       /* shorten to 8 rounds (with an inline comment) */
  "Rounds = 8   ; eight-round season\n";

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

  /* [Team 12] per-seat driver fields (slot 0/1) */
  CHECK(t->drv[0].nameSet && strcmp(t->drv[0].name, "Jean Alesi") == 0);
  CHECK(t->drv[0].qualSet && t->drv[0].qual == 15980);
  CHECK(t->drv[0].raceSet && t->drv[0].race == 15500);
  CHECK(t->drv[0].rangeSet && t->drv[0].range == 1342);
  CHECK(t->drv[0].weightSet && t->drv[0].weight == 16384);
  CHECK(t->drv[0].numSet && t->drv[0].num == 27);
  CHECK(t->drv[0].selectedSet && t->drv[0].selected == 1);
  CHECK(t->drv[1].disabledSet && t->drv[1].disabled == 1);

  /* [Team 12] per-team perf / pitcrew */
  CHECK(t->powerSet && t->power == 765);
  CHECK(t->qualpowerSet && t->qualpower == 750);
  CHECK(t->reliabilitySet && t->reliability == 2048);
  CHECK(t->pitcrewSet && t->pitcrew[0] == 144 && t->pitcrew[13] == 16);

  /* [Team 13] over-count pitcrew (15 values) rejected -> flag stays unset */
  CHECK(OverrideTeam(13)->pitcrewSet == 0);

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

  /* [Calendar] Rounds = 8 (inline comment tolerated) */
  CHECK(OverrideCalendarRounds() == 8);

  /* invalid Rounds -> rejected (0 = stock) */
  {
    const char *p2 = "/tmp/_ov_test2.cfg";
    FILE *g2 = fopen(p2, "wb");
    fputs("[Calendar]\nRounds = 17\n", g2); fclose(g2);       /* out of 1..16 */
    CHECK(OverrideParseFile(p2, TAB) == 0);
    CHECK(OverrideCalendarRounds() == 0);

    g2 = fopen(p2, "wb");
    fputs("[Calendar]\nRounds = 0\n", g2); fclose(g2);        /* zero-length */
    CHECK(OverrideParseFile(p2, TAB) == 0);
    CHECK(OverrideCalendarRounds() == 0);
  }

  /* absent [Calendar] -> 0 (stock 16-round) */
  {
    const char *p3 = "/tmp/_ov_test3.cfg";
    FILE *g3 = fopen(p3, "wb");
    fputs("[General]\nPitRefuelSpeed = 50\n", g3); fclose(g3);
    CHECK(OverrideParseFile(p3, TAB) == 0);
    CHECK(OverrideCalendarRounds() == 0);
  }

  if (fails == 0) printf("\nALL PASS\n");
  else            printf("\n%d FAILURE(S)\n", fails);
  return fails ? 1 : 0;
}
