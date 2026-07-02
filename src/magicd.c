#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magicd.h"
#include "override.h"       /* OverrideTrack / OverrideBaseDir / OV_TRACKS */
#include "miscahf.h"        /* IDACodeReftoDataRef */
#include "basiclog.h"       /* LogLine / strbuf */

/* Code op whose disp32 operand = &magic table 1 (IDA 0xD57F4 = the magic block base).
   GP2Lap already resolves this same ref as arTrackTyreWear. */
#define MD_BASEREF 0x76F84UL

/* (offset-from-block-base, per-slot stride) for the 24 tables, ported verbatim from
   gp2-workshop's MAGIC_LAYOUT (base_file_offset - 1280584; file offset = IDA + 0x63254).
   Every value is a little-endian u16 at base + off + slot*stride. The strides encode the
   dword/word split (2,3,4,9,10,24 = stride 4) and the legacy stride-6 layout of tables
   14-17/19-21 exactly as the .m2d format stores them -> byte-exact round-trip with
   gp2-workshop / the old GP2 magic-data editors. (Table 6 is dead data, still written
   for fidelity. The stride-6 tables carry the community's interleaved pit-geometry layout,
   which docs/magic-data.md notes is semantically scrambled but is what .m2d files hold.) */
static const struct { unsigned short off; unsigned char stride; } MD_LAYOUT[24] = {
  {    0, 2 }, {   32, 4 }, {   96, 4 }, {  160, 4 }, {  256, 2 }, {  288, 2 },
  {  320, 2 }, {  352, 2 }, {  392, 4 }, {  394, 4 }, {  456, 2 }, {  488, 2 },
  {  550, 2 }, {  646, 6 }, {  648, 6 }, {  742, 6 }, {  744, 6 }, {  838, 2 },
  {  870, 6 }, {  872, 6 }, {  874, 6 }, {  966, 2 }, {  998, 2 }, { 1030, 4 }
};

/* read a .m2d: 24 decimal u16 lines (table order 1..24), blank lines skipped.
   Returns 1 only if exactly 24 values were read. */
static int ReadM2D(const char *path, unsigned short *vals)
{
  FILE *f = fopen(path, "rb");
  char  line[64];
  int   n = 0;
  if (!f) return 0;
  while (n < 24 && fgets(line, sizeof(line), f)) {
    char *p = line;
    long  v;
    while (*p==' ' || *p=='\t') p++;
    if (*p=='\r' || *p=='\n' || *p==0) continue;     /* skip blank lines */
    v = strtol(p, 0, 10);
    if (v < 0) v = 0; else if (v > 65535L) v = 65535L;
    vals[n++] = (unsigned short)v;
  }
  fclose(f);
  return n == 24;
}

void MagicDataInit(void)
{
  unsigned char *base = (unsigned char *)IDACodeReftoDataRef(MD_BASEREF);  /* &magic table 1 (0xD57F4) */
  int trk, applied = 0;

  if (!base) { LogLine("- MagicData: base table unresolved; DISABLED\n"); return; }

  for (trk = 1; trk <= OV_TRACKS; trk++) {
    const OvTrack *t = OverrideTrack(trk);
    const char    *dir;
    unsigned short vals[24];
    char           path[512];
    int            i, slot = trk - 1;

    if (!t || !t->magicDataSet) continue;

    dir = OverrideBaseDir();
    if (dir && dir[0]) sprintf(path, "%s%s", dir, t->magicData);
    else               strcpy(path, t->magicData);

    if (!ReadM2D(path, vals)) {
      sprintf(strbuf, "- MagicData: [Track %d] '%s' unreadable or not 24 values; skipped\n", trk, path);
      LogLine(strbuf);
      continue;
    }
    for (i = 0; i < 24; i++)
      *(unsigned short *)(base + MD_LAYOUT[i].off + slot * MD_LAYOUT[i].stride) = vals[i];

    sprintf(strbuf, "- MagicData: [Track %d] slot %d <- '%s'\n", trk, slot, path); LogLine(strbuf);
    applied++;
  }
  if (applied) { sprintf(strbuf, "- MagicData: %d track(s) applied\n", applied); LogLine(strbuf); }
}
