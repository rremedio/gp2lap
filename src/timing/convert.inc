#include "../pages.h"

#define RATL_START   0
#define RATL_BEHIND  0
#define RATL_LAPTIME 1
#define RATL_END     1

#define QATL_START   0
#define QATL_SECTORS 0
#define QATL_SECDIFF 1
#define QATL_IDEAL   2
#define QATL_END     2


#include "../gp2rmcod.h"

unsigned char *GP2_use_svga = NULL;
unsigned char *GP2_RaceMode = NULL;
struct gp2drivernames *GP2_DriverNames = NULL;
struct gp2order *GP2_LapsRunByCar = NULL;  // keine order sondern car als index
struct gp2array40 *GP2_CaridTeamTab = NULL;
struct gp2order *GP2_RaceOrder = NULL;
struct gp2order *GP2_FastestCars = NULL;

struct gp2timetable *GP2_RaceTimes = NULL;
struct gp2timetable *GP2_BestLaptimes = NULL;
struct gp2timetable *GP2_BestSplit1 = NULL;
struct gp2timetable *GP2_BestSplit2 = NULL;
struct gp2timetable *GP2_QualTimesComb = NULL;
struct gp2timetable *GP2_QualTimesSes1 = NULL;
struct gp2timetable *GP2_QualTimesSes2 = NULL;
struct singlecarstruct **pCarInView = NULL;
struct singlecarstruct **pPlayerCar = NULL;

unsigned short *GP2_NumCarsRunning = NULL;
unsigned long  *GP2_NumStarters = NULL;
unsigned short *GP2_LapsInThisRace = NULL;
struct gp2seg *GP2_MistSeg = NULL;
struct gp2cartable *GP2_Cars = NULL;
unsigned char *GP2_TranspTab = NULL;
unsigned char *pCameraViewSrc = NULL;
unsigned long *pRCRTable = NULL;

unsigned char *pRcvLnkCmd = NULL;
gp2sendslots *t_CmdsToBeSend = NULL;
unsigned long InCockpitNow = 0;         // set if in cockpit (race engine, otherwise 0)

//------ some cfg switches --------------
unsigned long SupressFastestLap = 0;    // by default: no
unsigned long atlAcrossTop = 0;    // by default: no
unsigned long PerCarTyreCompounds = 0;  // 2026: per-car tyre compounds by pit strategy (0=off)
unsigned long SoftTyresInQuali = 1;     // 2026: in qualifying, everyone runs D (softest). 0 = stock
unsigned long ShowTyreCompoundBadge = 1;// 2026: show the (X) compound badge; only if PerCarTyreCompounds=1 (0=blind)
unsigned long JamTextureExtraMB = 0;    // 2026: ADD N MB to the JAM texture buffer over stock ~2.84MB (0 = stock)
static unsigned long s_baseCsum  = 0;   // 2026: track checksum the base below was captured for
static int           s_trackBase = -1;  // 2026: genuine track base compound (0..3), -1 = not yet

extern void MyCompoundToBL(void);       // asm grip helper in lammcall.asm (patched into CalcESIsGrip?)
extern GP2CSx *pCarSetups;              // GP2's per-car setup array (osCarSetups), already resolved

//--- 2026 --- read a car's compound (GP2CSx.compound, +8) from its setup record ---
//--- player uses pPlayerSetup; AI uses pCarSetups[carnum-1]. Returns 0..3, or -1 if unavailable.
static int CarCompound(unsigned int carId)
{
  GP2CSx *s;
  if (carId & 0x80) s = (GP2CSx *)pPlayerSetup;
  else s = pCarSetups ? &pCarSetups[(carId & 0x3F) - 1] : NULL;
  return s ? (s->compound & 3) : -1;
}

//--- 2026 --- compound -> 'A'..'D' for the at-the-line badge ---
char CompoundLetter(unsigned int carId)
{
  int c = CarCompound(carId);
  return (char)(c < 0 ? '?' : 'A' + c);
}

//--- 2026 --- per-session fill: assign each car a compound and write it to its setup record (+8).
//--- Runs once per session via Hook_FillCompounds (after SetSetups?? populates setups + strategies).
//---
//--- Qualifying (RaceMode bit7 clear): SoftTyresInQuali -> everyone runs D (3); else the track base.
//--- Race: derive from the pit-strategy mix. Cars are bucketed by planned stop count
//--- (GP2CSx.num_pitstops, +0xA); buckets map to CONSECUTIVE compounds, fewer stops = harder; the run
//--- is positioned so the MOST-POPULATED bucket lands on the track base compound, then clamped to A..D.
//--- (Reproduces the design examples; the player follows their own chosen strategy. See
//--- docs/gp2lap/per-car-tyre-compounds.md.)
//--- Runs once per session from AHFSOSHook (start-of-session), the first point where
//--- bSessionMode (==*GP2_RaceMode) reflects the SESSION WE ARE ENTERING. (At GP2's
//--- car-setup time it still holds the PREVIOUS session's mode, which made qualifying's
//--- all-D leak into the race.) Modes: 0x80 = race, 0x40 = qualifying, else practice/
//--- warm-up/etc. We always (re)write every car's setup[+8] so no stale value survives.
void __near _cdecl AHFFillCompounds(void)
{
  int i, s, k, nb, mp, base, ncars, mode, q, sh, c;
  int present[4], bstop[4], bcount[4], comp[4], stop2comp[4];
  GP2CSx *psetup;

  if (!PerCarTyreCompounds) return;            /* feature off -> stock untouched */
  if (!pCarSetups) return;
  psetup = (GP2CSx *)pPlayerSetup;

  /* True track base compound. b_TireType (0x1771C0) is the SAME byte as the player's  */
  /* setup[+8] (pPlayerSetup 0x1771B8 + 8), so our own fills overwrite it: it equals    */
  /* the genuine track base only on the FIRST session of a weekend, before any fill.    */
  /* DoTrackSetup writes it at track load; snapshot it once per track (dwTrackChecksum   */
  /* change) and reuse it for every session that weekend, surviving quali->race.        */
  if (dwTrackChecksum != s_baseCsum || s_trackBase < 0) {
    s_trackBase = (*((BYTE *)pCarSetups - 0x58)) & 3;
    s_baseCsum  = dwTrackChecksum;
  }
  base = s_trackBase;

  mode = GP2_RaceMode ? (int)(*GP2_RaceMode) : 0;

  if (mode == 0x80) {
    /* --- RACE: pit-strategy -> per-car compound --- */
    ncars = GP2_NumStarters ? (int)*GP2_NumStarters : 26;
    if (ncars < 1) ncars = 1;
    if (ncars > GP2_MAX_NUMCARS) ncars = GP2_MAX_NUMCARS;

    for (s = 0; s < 4; s++) present[s] = 0;
    for (i = 0; i < ncars; i++) present[pCarSetups[i].num_pitstops & 3]++;

    nb = 0;                                    /* distinct stop-counts present, ascending */
    for (s = 0; s < 4; s++) if (present[s]) { bstop[nb] = s; bcount[nb] = present[s]; nb++; }
    if (nb == 0) return;

    mp = 0;                                    /* most-populated bucket (ties -> fewest stops) */
    for (k = 1; k < nb; k++) if (bcount[k] > bcount[mp]) mp = k;

    for (k = 0; k < nb; k++) comp[k] = base + (k - mp);   /* fewer stops harder, anchored at base */
    if (comp[0] < 0)    { sh = -comp[0];       for (k = 0; k < nb; k++) comp[k] += sh; }  /* clamp run */
    if (comp[nb-1] > 3) { sh = comp[nb-1] - 3; for (k = 0; k < nb; k++) comp[k] -= sh; } /* into A..D */

    for (s = 0; s < 4; s++) stop2comp[s] = base;
    for (k = 0; k < nb; k++) {
      c = comp[k]; if (c < 0) c = 0; if (c > 3) c = 3;
      stop2comp[bstop[k]] = c;
    }

    for (i = 0; i < GP2_MAX_NUMCARS; i++)
      pCarSetups[i].compound = (BYTE)stop2comp[pCarSetups[i].num_pitstops & 3];
    if (psetup)
      psetup->compound = (BYTE)stop2comp[psetup->num_pitstops & 3];
  }
  else if ((mode & 0x40) || mode == 0x01 || mode == 0x03) {
    /* --- QUALIFYING (0x40), or Friday(0x01)/Saturday(0x03) free practice: follow   --- */
    /* --- the qualifying tyre rule (D when SoftTyresInQuali, else the stock base).   --- */
    /* --- Warm-up (0x02) and unlimited practice (0x00) fall through unchanged below. --- */
    q = SoftTyresInQuali ? 3 : base;
    for (i = 0; i < GP2_MAX_NUMCARS; i++) pCarSetups[i].compound = (BYTE)q;
    if (psetup) psetup->compound = (BYTE)q;
  }
  else {
    /* --- Unlimited practice (0x00), pre-race warm-up (0x02), anything else: leave --- */
    /* --- every car on the stock single compound (track base) -> behaves like stock. -- */
    for (i = 0; i < GP2_MAX_NUMCARS; i++) pCarSetups[i].compound = (BYTE)base;
    if (psetup) psetup->compound = (BYTE)base;
  }
}

#include "timing/gp2struc.h"
#include "timing/timinpic.h"
#include "timing/codehook.inc"


//-----------------------------------------------------------------------

void FrankSoftDone(void);

void FrankSoftInit(void)
{
  //--- 9/98 ---------
  initfont( &fntarrn6 );
  initfont( &fntverd7fk );
  initfont( &fntverd7k );
  initfont( &fntgp2_8x8 );
  initfont( &fntarial20 );


  if (GetCfgULong("atlNoFastestLap"))
    SupressFastestLap = *GetCfgULong("atlNoFastestLap");

  if (GetCfgULong("atlSplit1Color"))      SetRBehindCol0(*GetCfgULong("atlSplit1Color"));
  if (GetCfgULong("atlSplit2Color"))      SetRBehindCol1(*GetCfgULong("atlSplit2Color"));
  if (GetCfgULong("atlLineColor"))        SetRBehindCol2(*GetCfgULong("atlLineColor"));
  if (GetCfgULong("atlAcrossTop"))        atlAcrossTop = *GetCfgULong("atlAcrossTop");
  if (GetCfgULong("atlMarkTopX"))         SetLaptimeTopCount(*GetCfgULong("atlMarkTopX"));
  if (GetCfgULong("PerCarTyreCompounds")) PerCarTyreCompounds = *GetCfgULong("PerCarTyreCompounds");
  if (GetCfgULong("SoftTyresInQuali"))    SoftTyresInQuali = *GetCfgULong("SoftTyresInQuali");
  if (GetCfgULong("ShowTyreCompoundBadge")) ShowTyreCompoundBadge = *GetCfgULong("ShowTyreCompoundBadge");
  if (GetCfgULong("JamTextureExtraMB"))     JamTextureExtraMB = *GetCfgULong("JamTextureExtraMB");
  if (GetCfgULong("logDebugScreenLines")) VisLogSetStrCount(*GetCfgULong("logDebugScreenLines"));
  if (GetCfgULong("DrawLines"))           SetCopySvgaLinesNum(*GetCfgULong("DrawLines"));

  if (GetCfgULong("LogDpmiInfo"))
    SetLogDpmiInfo(*GetCfgULong("LogDpmiInfo"));

  if (GetCfgULong("UseIpxLink"))
    UseIpxLink = *GetCfgULong("UseIpxLink");
  if (GetCfgULong("LogIpxInfo"))
    LogIpxInfo = *GetCfgULong("LogIpxInfo");
  if (GetCfgULong("LogIpxOutpackets"))
    LogIpxOutpackets = *GetCfgULong("LogIpxOutpackets");
  if (GetCfgULong("LogIpxInpackets"))
    LogIpxInpackets = *GetCfgULong("LogIpxInpackets");

  if (GetCfgULong("LogLnkCmdsIn"))
    LogLnkCmdsIn = *GetCfgULong("LogLnkCmdsIn");
  if (GetCfgULong("LogLnkCmdsOut"))
    LogLnkCmdsOut = *GetCfgULong("LogLnkCmdsOut");
  if (GetCfgULong("LogLnkCmdsProcess"))
    LogLnkCmdsProcess = *GetCfgULong("LogLnkCmdsProcess");

  if (GetCfgULong("ForcePMBanking"))
    ForcePMBanking = *GetCfgULong("ForcePMBanking");
  if (GetCfgULong("ForceLFBuffering"))
    ForceLFBuffering = *GetCfgULong("ForceLFBuffering");
  if (GetCfgULong("UseOldNetworkCode"))
    UseOldNetworkCode = *GetCfgULong("UseOldNetworkCode");

  if (UseIpxLink) {
    sprintf(strbuf, "- IpxLink: using %s network code\n", UseOldNetworkCode ? "OLD" : "NEW");
    LogLine(strbuf);
  }


  #ifdef TEST
    sprintf(strbuf, "- ForceLFBuffering == %s\n", ForceLFBuffering ? "yes" : "no");
    LogLine(strbuf);
  #endif

  atexit( FrankSoftDone );

} // FrankSoftInit()

//-----------------------------------------------------------------------

void FrankSoftDone(void)
{
  if (LogIpxInfo && LinkEstablished) {
    sprintf(strbuf, "- IpxLink: pcnt: %u lecb: %P lsz: %u nerr: %u mecb: %u mpck: %u me: %u\n",
      statsaddr->packetcount,
      statsaddr->lastecb,
      statsaddr->lastsize,
      statsaddr->neterrors,
      statsaddr->missedecbs,
      statsaddr->missedpackets,
      statsaddr->packetsfromme);
    LogLine(strbuf);
  }

} // FrankSoftDone()

//-----------------------------------------------------------------------

void __near _cdecl AHFAfterGp2Init(void)
// called after general gp2 init
{
   long *tmplongp;
   ulong *crtp = GetCfgULong("CarRemoveTime");

   // 12/98 Rene
   ulong *lsp = GetCfgULong("UseLocalizedStrings");
   init_gp2_version(lsp && *lsp);

#ifdef TEST
   sprintf(strbuf, "- GP2Lap __begtext at 0x%08X\n" \
                   "- GP2Lap end       at 0x%08X (%u bytes)\n", &__begtext, &end, &end-&__begtext);
   LogLine(strbuf);
   printf(strbuf); fflush(stdout);
   sprintf(strbuf, "- GP2CodeStart at 0x%08X\n", GP2_CodeStartAdr-0x10020);
   LogLine(strbuf);
   printf(strbuf); fflush(stdout);
#endif  // !

   //--- 10/98 --- Increase car remove time -----
   if ( crtp && *crtp ) {
         if ( *(ulong *)(IDAtoFlat(0x2B101)) == 20000) { // default of 20 secs?
       *(ulong *)(IDAtoFlat(0x2B101)) = *crtp;  // 3600000 == 1h
         sprintf(strbuf, "- CarRemoveTime set to %u\n", *crtp);
         LogLine(strbuf);
     }
   }

   //--- 9/98 ---- SET RMCODE PTR WITHIN MY IPX ESR ---
   if ( LinkEstablished ) {
     if (*ppDosRmCode && !((ulong)(*ppDosRmCode) & 0xFFF00000) ) {   // gp2's rmcode ptr set and poiting to low mem?
       // set the 16:16 adr of the gp2 rmcode within my esr
       *(ulong *)((ulong)(esrrmaddr) + (ulong)(EsrRmcodeOfs)) =
                       ( (DPMI_real_segment(*ppDosRmCode) << 16) +
                          DPMI_real_offset(*ppDosRmCode) );
     }
   }

  //--- 8/98 --- Don't del times in pits ---------
  memset(IDAtoFlat(0x1634a), 0x90, 17);  // suppress reseting timeLapStart & timeLast

  //--- 10/98 --- Show splits in race ---------
  //if ( GetCfgULong("SplitsInRace") && *GetCfgULong("SplitsInRace") )
  //--- 11/99 Ignore, because ATLBehind doesn't work properly with Splits diabled
  memset(IDAtoFlat(0x1611d), 0x90, 13);

  //--- 2026 --- per-car tyre compounds (Phase 1): route CalcESIsGrip?'s compound
  //--- read through MyCompoundToBL so each car uses its own compound for grip & wear.
  //--- Replaces "mov bl, b_TireType" (6 bytes @ IDA 0x30436) with "call MyCompoundToBL" + NOP.
  if (PerCarTyreCompounds) {
    unsigned char *p = (unsigned char *)IDAtoFlat(0x30436);
    *p = 0xE8;                                                       // call rel32
    *(long *)(p + 1) = (long)((ulong)MyCompoundToBL - (ulong)(p + 5));
    p[5] = 0x90;                                                     // NOP (displaced 6th byte)
    LogLine("- PerCarTyreCompounds: ON (compounds are now per-car)\n");
  }

  //--- 2026 --- enlarge the JAM texture accumulator (lift the ~2.84MB image-area limit).
  //--- The loader stages JAM image bytes from d_vbufferptr, and sub_70B07 rebases every entry's
  //--- image pointer by +d_vbufferptr, so repointing d_vbufferptr (init imm @0x70ACB) and raising
  //--- the image guard (cmp imm @0x70C23) moves the whole texture pipeline into our buffer; the
  //--- 320x200 screen buffer (v320_640_buf) and the palette buffer are untouched.
  //--- See docs/jam-buffer-expansion-plan.md.
  if (JamTextureExtraMB) {
    unsigned long extra = JamTextureExtraMB;
    unsigned long size;
    void *jbuf;
    if (extra > 61) extra = 61;                       /* sanity clamp (well under DOS/4GW's 64MB) */
    size = 0x2D8070UL + extra * 0x100000UL;           /* stock ~2.84MB + N MB */
    jbuf = malloc(size);
    if (jbuf) {
      unsigned long base = (unsigned long)jbuf;
      *(unsigned long *)(IDAtoFlat(0x70ACB) + 6) = base;          /* d_vbufferptr init base */
      *(unsigned long *)(IDAtoFlat(0x70C23) + 1) = base + size;   /* image-area guard end   */
      sprintf(strbuf, "- JamTextureBuffer: stock +%lu MB = %lu KB @ 0x%08lX..0x%08lX\n",
              extra, size >> 10, base, base + size);
      LogLine(strbuf);
    } else {
      sprintf(strbuf, "- JamTextureBuffer: alloc of stock +%lu MB FAILED; staying on stock buffer\n", extra);
      LogLine(strbuf);
    }
  }

#ifdef TEST
  //--- 10/98 --- dump the Strings ---------
  if ( GetCfgULong("LogAllStrings") && *GetCfgULong("LogAllStrings") )
    dump_all_gp2strings();
#endif

  //--- 10/98 --- chg some unk rcr vals ---------
  if (pRCRTable) {
    if (GetCfgULong("VGARcr1")) pRCRTable[0] = *GetCfgULong("VGARcr1");
    if (GetCfgULong("SVGARcr1")) pRCRTable[1] = *GetCfgULong("SVGARcr1");
    if (GetCfgULong("VGARcr2")) pRCRTable[2] = *GetCfgULong("VGARcr2");
    if (GetCfgULong("SVGARcr2")) pRCRTable[3] = *GetCfgULong("SVGARcr2");
    if (GetCfgULong("VGARcr3")) pRCRTable[4] = *GetCfgULong("VGARcr3");
    if (GetCfgULong("SVGARcr3")) pRCRTable[5] = *GetCfgULong("SVGARcr3");
  }

  if (*GetCfgULong("CatchTrackInfos"))
    CatchTrackInfos();  // try to catch Names from custom track files

  //--- 9/99 --- set fisheye zoom for cokpit mode (default: 256) ----
  tmplongp = GetCfgLong("CockpitZoom");
  if ( tmplongp && *tmplongp ) {
    memset(IDAtoFlat(0x3829d), 0x90, 5);  // wipeout max check for 0x1F00 of ChgViewPlane
    memset(IDAtoFlat(0x382a9), 0x90, 5);  // wipeout min check for 0x80 of ChgViewPlane
    if (*tmplongp > 0x1F00) *tmplongp = 0x1F00;
    else if (*tmplongp < 64) *tmplongp = 64;
    *(long *)(IDAtoFlat(0x3856d)) = *tmplongp;
    sprintf(strbuf, "- CockpitZoom set to %d\n", *tmplongp);
    LogLine(strbuf);
  }

  //--- 9/99 --- dexor confidential strings ---------
  dexor(&securehpmsg);  // "(%u hp)"


} // AHFAfterGp2Init()

void (__near _cdecl *fpAHFAfterGp2Init)(void) = AHFAfterGp2Init;


//-----------------------------------------------------------------------


//--- 9/99 --- before gp2 destroyes all best splittimes ----
//--- called at the end of each lap, similar to AHFCrossingSplit(0), but
//--- much earlier, before much of the calc's are done -> see IDA 0x160c6
#pragma aux AHFBefDestrBest parm [ESI] caller;
void __near _cdecl AHFBefDestrBest( struct singlecarstruct *actcar )
{
  static unsigned char id;
  id = actcar->car_id & 0x3F;

  if (actcar->lastlap_time < actcar->bestlap_time) {
    // ok, gp2 will overwrite actual best sector info's soon, so back them up
    sicprevbest[id][0] = actcar->bestlap_split1;
    sicprevbest[id][1] = actcar->bestlap_split2;
    sicprevbest[id][2] = actcar->bestlap_time;

    //-----------------------------------
    //---- stats for the currently, (but probably no longer) leading car ----
    //---- immer bezogen auf car, das gerade finishline passiert (id) ----
    //-----------------------------------
    if (!(*GP2_RaceMode & 0x80)) {
      GetBestSplitsSegs(&prevldrsplits[id][0], &prevldrsegs[id][0], QPOS2CARID(0), Carid2Carstruct(QPOS2CARID(0)));
      sicprevbest[id][3] = (unsigned long) QPOS2CARID(0);  // actual (probably previous) polesitter

/*
      if (actcar->car_id & 0x80) { // nur fuer human
        sprintf(strbuf, "prevldr: 0x%02x (%s %s %s -> %s)\n", sicprevbest[id][3],
                                                       myGetTimeString( &prevldrsegs[id][0] ),
                                                       myGetTimeString( &prevldrsegs[id][1] ),
                                                       myGetTimeString( &prevldrsegs[id][2] ),
                                                       myGetTimeString( &prevldrsplits[id][2] ));
        LogLine(strbuf);
      }
*/
    }
  }

  // die splitzeiten der eben beendeten runde retten
  prevlapsplits[id][0] = actcar->lastlap_split1;
  prevlapsplits[id][1] = actcar->lastlap_split2;
  prevlapsplits[id][2] = actcar->lastlap_time;

/*
  if (actcar->lastlap_split1 < idealsegs[id][0])
    idealsegs[id][0] = actcar->lastlap_split1;
  if (actcar->lastlap_split2 - actcar->lastlap_split1 < idealsegs[id][1])
    idealsegs[id][1] = actcar->lastlap_split2 - actcar->lastlap_split1;
  if (actcar->lastlap_time - actcar->lastlap_split2 < idealsegs[id][2])
    idealsegs[id][2] = actcar->lastlap_time - actcar->lastlap_split2;
*/

} // AHFBefDestrBest()

void (__near _cdecl *fpAHFBefDestrBest)(struct singlecarstruct *) = AHFBefDestrBest;


//-----------------------------------------------------------------------
