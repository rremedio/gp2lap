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
#include "../cartex.h"   // 2026: per-car car textures
#include "../carshape.h" // 2026: per-team car shapes (nose / .dat)
#include "../pitstops.h" // 2026: configurable pit-stop timing
#include "../teamphys.h" // 2026: per-team mass + downforce
#include "../aiaccel.h"  // 2026: AI low-HP launch fix
#include "../aibank.h"   // 2026: AI banking roll (cars roll onto banked track)
#include "../override.h" // 2026: shared SeasonOverrides parser/model (per-team sections)
#include "../drvdata.h"  // 2026: per-driver data override (names/skill/range/weight/num)
#include "../magicd.h"   // 2026: per-track magic-data (.m2d) loading
#include "../sessions.h" // 2026: [Weekend] enable/disable sessions
#include "../calendar.h" // 2026: [Calendar] variable season length/order
#include "../roster.h"   // 2026: [Team 15..20] bigger roster (d_anzteams)
#include "../pitcrew.h"  // 2026: per-team pit-crew colours
#include "../gridcap.h"  // 2026: small-grid support (AllowSmallGrid)
#include "../jamidcap.h" // 2026: raise the jam-id ceiling (relocate id->descriptor map)

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
unsigned long ExtraTrackSegments = 0;   // 2026: ADD N track+pit segments over stock 1600 (0 = stock ~7km limit)
unsigned long ExtraTrackFileKB = 0;     // 2026: ADD N KB to the track file/data buffers over stock ~62KB (0 = stock)
unsigned long PitSpeedKmh = 80;         // 2026: pit-lane speed limit (km/h) for BOTH AI + player (0 = stock/untouched)
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
  if (GetCfgULong("ExtraTrackSegments"))    ExtraTrackSegments = *GetCfgULong("ExtraTrackSegments");
  if (GetCfgULong("ExtraTrackFileKB"))      ExtraTrackFileKB = *GetCfgULong("ExtraTrackFileKB");
  if (GetCfgULong("PitSpeedKmh"))           PitSpeedKmh = *GetCfgULong("PitSpeedKmh");
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

//--- 2026 --- locate a 4-byte immediate by VALUE within an instruction (encoding-agnostic),
//--- used by the ExtraTrackSegments relocation. Returns the byte offset (1..6) or -1.
static int SegFindImm(unsigned long ida, unsigned long oldv)
{
  unsigned char *p = (unsigned char *)IDAtoFlat(ida);
  int o;
  for (o = 1; o <= 6; o++)
    if (*(unsigned long *)(p + o) == oldv) return o;
  return -1;
}

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

  //--- 2026 --- raise the jam-id ceiling (788 -> 1024) by relocating the id->descriptor map
  //--- + its two per-id byte arrays, so GP2Lap can register atlases at ids 788+ (a band the
  //--- stock exe hangs on -> collision-proof vs every stock/custom track). ids 0..787 unchanged.
  JamIdCapInit();

  //--- 2026 --- lift the ~1600-segment (track+pit, ~7km) limit by relocating the segment buffer
  //--- to a larger allocation. The buffer (t_TrackSegs..t_LastSegSpace, followed by the shade LUT)
  //--- is reached by ~51 absolute code immediates; runtime pointers (pFirstSeg*, car.curSeg, the
  //--- off_0_CADC0 lookup table) are computed from it and follow automatically. We repoint the
  //--- base/pre/end refs by VALUE (encoding-agnostic) with an ALL-OR-NOTHING verify pass: if any
  //--- site doesn't hold its expected old value, disable rather than half-patch (no corruption).
  //--- Site lists reconciled from both listings; WriteHotLap's raw 0x16E362 is deliberately excluded.
  //--- See docs/track-length-limit.md.
  if (ExtraTrackSegments) {
    static const unsigned long segBase[] = {
      0x14177UL, 0x1417EUL, 0x144DDUL, 0x14512UL, 0x1456AUL, 0x1459FUL, 0x1FE9DUL, 0x2DF12UL,
      0x2DFD7UL, 0x4CE73UL, 0x4CF41UL, 0x4F255UL, 0x4F522UL, 0x4F830UL, 0x4FB19UL, 0x4FE33UL,
      0x4FF26UL, 0x5065BUL, 0x506B2UL, 0x51C0DUL, 0x52AAFUL, 0x55980UL, 0x55AAEUL, 0x63F5BUL,
      0x63F6AUL, 0x65C5BUL, 0x737C8UL, 0x73804UL, 0x73B13UL, 0x7433EUL, 0x74C15UL, 0x765D8UL,
      0x770FBUL, 0x77128UL, 0x77443UL, 0x774AAUL, 0x774C0UL, 0x774D6UL, 0x774FFUL, 0x7751AUL,
      0x7762BUL, 0x77EF1UL, 0x78224UL
    };
    static const unsigned long segPre[]     = { 0x732B0UL, 0x7346EUL, 0x777D3UL };
    static const unsigned long segEnd[]      = { 0x73787UL, 0x738CFUL, 0x77148UL, 0x77211UL };
    static const unsigned long segEndPlus[]  = { 0x732B9UL };
    /* The save-game pointer normalizer (NormaliseCarStruct / sub_0_6BE6A) converts segment
       pointers <-> indices using a SEPARATE base: osSegments = t_TrackSegs - 0x70 (IDA 0x146CA4),
       at 6 sites covering car.curSeg, car.pSeg2 and the D479C seg-ptr table. The value-matched
       patch above never touches 0x146CA4, so with the buffer relocated these normalize car.curSeg
       against the OLD base -> wrong/zero base -> the per-tick seg-tracking search (sub_0_1409A ->
       sub_0_1427D) underflows to -0x6C -> illegal read (long-track crash after a save/checkpoint).
       Relocate osSegments to newBase - 0x70 so the index math round-trips with the new buffer. */
    static const unsigned long segOs[]       = {
      0x6BE84UL, 0x6BECEUL, 0x6BF08UL, 0x6BF0FUL, 0x6BFCBUL, 0x6BFD2UL
    };
    const unsigned long *grp[5];
    unsigned long ov[5], nv[5];
    int cnt[5];
    unsigned long extra, newN, oldBase, newBase, bytes;
    unsigned char *bb;
    unsigned long ddelta;
    unsigned long *pfss, *pfsp;
    int g, i, off, ok;
    extern GP2Seg *pTrackSegs;   /* GP2Lap's own seg-buffer pointer (set once at attach from the stock base) */

    extra = ExtraTrackSegments;
    if (extra > 6400) extra = 6400;                /* clamp: newN <= 8000 segments */
    newN = 1600UL + extra;

    /* bootstrap: 0x144DD is "mov edi, offset t_TrackSegs" (BF + imm32) -> imm at +1 = runtime base */
    oldBase = *(unsigned long *)(IDAtoFlat(0x144DD) + 1);
    if (oldBase <= 0x10000UL) {
      LogLine("- ExtraTrackSegments: bootstrap read failed; DISABLED\n");
    } else {
      bytes = (newN + 2) * 0x6CUL;               /* pre-slot + newN slots + end sentinel */
      bb = (unsigned char *)malloc(bytes);
      if (!bb) {
        sprintf(strbuf, "- ExtraTrackSegments: alloc of %lu KB FAILED; stock 1600 segs\n", bytes >> 10);
        LogLine(strbuf);
      } else {
        /* Stock's seg buffer is a static, ZERO-initialized array; malloc is not. A long track
           leaves a large uninitialized tail (segs > track count) + pre-slot/sentinel = raw garbage,
           which any one-past-the-end read turns into a wild pointer (illegal read) or a zero/garbage
           divisor (divide-by-zero). Replicate stock by zeroing. (Init-time only -> covers the first
           track; a later shorter track keeps stale tail data, acceptable for now.) */
        memset(bb, 0, bytes);
        newBase = (unsigned long)bb + 0x6CUL;
        /* Static-only DATA base pointer(s) share t_TrackSegs's data delta and must move too.
           pFirstSegSect1 (IDA 0xF9CE0) is NEVER runtime-written -> it stays = oldBase unless we
           relocate it (else seg-0 base is left in the old buffer). pFirstSegPitl (0xF9CDC) is
           runtime-set, relocated here only for init consistency. */
        ddelta = oldBase - 0x146D14UL;
        pfss = (unsigned long *)(ddelta + 0xF9CE0UL);
        pfsp = (unsigned long *)(ddelta + 0xF9CDCUL);
        grp[0]=segBase;    cnt[0]=43; ov[0]=oldBase;             nv[0]=newBase;
        grp[1]=segPre;     cnt[1]=3;  ov[1]=oldBase - 0x6CUL;    nv[1]=newBase - 0x6CUL;
        grp[2]=segEnd;     cnt[2]=4;  ov[2]=oldBase + 0x2A300UL; nv[2]=newBase + newN * 0x6CUL;
        grp[3]=segEndPlus; cnt[3]=1;  ov[3]=oldBase + 0x2A36CUL; nv[3]=newBase + (newN + 1) * 0x6CUL;
        grp[4]=segOs;      cnt[4]=6;  ov[4]=oldBase - 0x70UL;    nv[4]=newBase - 0x70UL;

        ok = 1;                                   /* pass 1: verify every site holds its old value */
        for (g = 0; g < 5 && ok; g++)
          for (i = 0; i < cnt[g]; i++)
            if (SegFindImm(grp[g][i], ov[g]) < 0) {
              sprintf(strbuf, "- ExtraTrackSegments: site 0x%lX (grp %d) mismatch; DISABLED\n",
                      grp[g][i], g);
              LogLine(strbuf); ok = 0; break;
            }
        if (ok && (*pfss != oldBase || *pfsp != oldBase)) {   /* static data base pointers */
          LogLine("- ExtraTrackSegments: static seg-base pointer mismatch; DISABLED\n");
          ok = 0;
        }

        if (!ok) {
          free(bb);
        } else {                                  /* pass 2: patch */
          for (g = 0; g < 5; g++)
            for (i = 0; i < cnt[g]; i++) {
              off = SegFindImm(grp[g][i], ov[g]);
              *(unsigned long *)(IDAtoFlat(grp[g][i]) + off) = nv[g];
            }
          *pfss = newBase;   /* relocate the static-only seg-0 base pointer */
          *pfsp = newBase;
          /* GP2Lap captured pTrackSegs from the STOCK base via a data hook at attach (lammcall.asm,
             0x14178 -> 0x146D14). It is never refreshed, so the live track map (trackmap.c) + track.c
             would read the old, now-unused buffer -> empty map. Repoint it to the new base. */
          if (pTrackSegs) pTrackSegs = (GP2Seg *)newBase;
          sprintf(strbuf, "- ExtraTrackSegments: +%lu -> %lu segs buffer @0x%08lX\n",
                  extra, newN, newBase);
          LogLine(strbuf);

          /* --- decouple the race-position list from the renderer's span scratch -------------
             off_0_F9CCC -> unk_0_139228 (IDA 0x139228) is the position-ORDER list (26 car
             offsets, source for t_TrackPosOfsTab), but GP2 ALSO uses unk_0_139228 as the
             rasterizer's span scratch (writer sub_0_39838; consumers sub_0_42xxx/44xxx +
             dword_FAA24). Stock time-multiplexes them; on a long track the render side leaves
             span records in the buffer when the AI position readers consume it -> garbage car
             offsets -> a misaligned car pointer -> a real car struct (car #4) gets stomped ->
             illegal read / divide-by-zero. Give the position list its OWN buffer; the renderer
             keeps unk_0_139228. Only two refs read the list off this base: the pointer
             off_0_F9CCC (builders sub_0_2C039 / sub_0_2CD3F) and the literal at 0x2CFF2; every
             other ref to unk_0_139228 is the renderer and is left untouched. */
          {
            unsigned long oldPos = ddelta + 0x139228UL;            /* runtime unk_0_139228   */
            unsigned long *pPos  = (unsigned long *)(ddelta + 0xF9CCCUL);  /* off_0_F9CCC     */
            int po = SegFindImm(0x2CFF2UL, oldPos);                /* the one literal list ref */
            if (*pPos != oldPos || po < 0) {
              LogLine("- ExtraTrackSegments: poslist decouple SKIPPED (ref mismatch)\n");
            } else {
              unsigned char *pb = (unsigned char *)malloc(0x100UL);
              if (pb) {
                int k;
                memset(pb, 0, 0x100UL);
                for (k = 0; k < 26; k++)
                  *(unsigned short *)(pb + k * 2) = (unsigned short)(k * 0x330);
                *pPos = (unsigned long)pb;                         /* off_0_F9CCC -> new buf  */
                *(unsigned long *)(IDAtoFlat(0x2CFF2UL) + po) = (unsigned long)pb;  /* literal */
                sprintf(strbuf, "- ExtraTrackSegments: poslist decoupled @0x%08lX (was 0x%08lX)\n",
                        (unsigned long)pb, oldPos);
                LogLine(strbuf);
              }
            }
          }

          /* --- relocate+enlarge the per-32-seg-chunk buffer unk_0_CDA48 (THE crash) ----------
             unk_0_CDA48 (IDA 0xCDA48) is a spatial-acceleration array: ONE 4-byte entry per
             32-segment chunk, indexed (seg+0x1A & 0x7FF)>>5. It is EXACTLY 50 entries (0xC8 B,
             ending at 0xCDB10) and is immediately followed by byte_0_CDB10, word_0_CDB12 (=the
             t_TrackPosOfsTab[-1] 0xFFFF terminator) and t_TrackPosOfsTab itself (0xCDB14). The
             fill loop's bound is word_0_C7E32 = numTrackSegs/32; a DEAD clamp at 0x73782
             (`cmp ax,32h; jle $+2`) was meant to cap it at 50 but does nothing. On a >1600-seg
             track numTrackSegs/32 > 50, so chunks 50..63 write PAST the array -> stomp the
             terminator + t_TrackPosOfsTab (entries become 0x3F00|n etc., term lost) -> the AI
             position readers compute t_CarStructs+garbage -> a misaligned car pointer stomps a
             real car (car #4) -> teleport / illegal read / divide-by-zero. word_0_C7E32 is ALSO
             the ring modulus in the wrap math, so it can't just be clamped. Fix: move the array
             to its own 64-entry (256-byte) buffer -- 64 covers the full (&0x7FF)>>5 index range,
             so it can never overflow for any track. byte_0_CDB10/word_0_CDB12/t_TrackPosOfsTab
             stay put and are no longer reachable by chunk-array writes. 9 refs (base +0/+1/+2). */
          {
            static const unsigned long chkAddr[] = {
              0x32DA8UL, 0x32E70UL, 0x32F35UL, 0x32FB2UL, 0x330E3UL,  /* mov ebx,offset (imm@+1) */
              0x74375UL,                                              /* init clear     (disp@+3) */
              0x67499UL,                                              /* CDA49 byte     (disp@+2) */
              0x55053UL, 0x674A4UL                                    /* CDA4A word     (disp@+3) */
            };
            static const int chkImmOff[] = { 1,1,1,1,1, 3, 2, 3,3 };
            static const int chkField[]  = { 0,0,0,0,0, 0, 1, 2,2 };
            unsigned long oldChk = ddelta + 0xCDA48UL;
            int j, cok = 1;
            for (j = 0; j < 9 && cok; j++) {
              unsigned long *p = (unsigned long *)(IDAtoFlat(chkAddr[j]) + chkImmOff[j]);
              if (*p != oldChk + (unsigned long)chkField[j]) {
                sprintf(strbuf, "- ExtraTrackSegments: chunkbuf site 0x%lX mismatch (0x%08lX); SKIPPED\n",
                        chkAddr[j], *p);
                LogLine(strbuf); cok = 0;
              }
            }
            if (cok) {
              unsigned char *cb = (unsigned char *)malloc(0x100UL);   /* 64 entries * 4 = 256 B */
              if (cb) {
                memset(cb, 0, 0x100UL);
                for (j = 0; j < 9; j++)
                  *(unsigned long *)(IDAtoFlat(chkAddr[j]) + chkImmOff[j]) =
                    (unsigned long)cb + (unsigned long)chkField[j];
                sprintf(strbuf, "- ExtraTrackSegments: chunkbuf relocated @0x%08lX (was 0x%08lX)\n",
                        (unsigned long)cb, oldChk);
                LogLine(strbuf);
              }
            }
          }
        }
      }
    }
  }

  //--- 2026 --- lift the ~62KB track file/data limit. GP2 reads the file into t_TrkReadbuffer
  //--- (read capped at 62000) and copies T_TrackDataSize bytes into t_LoadedTrkData (62004 B),
  //--- which abuts the car 3D model (o_interobj00) -> a bigger track overruns it. We relocate both
  //--- buffers to larger allocations and repoint EVERY reference by value via a whole-image scan
  //--- (GP2 is a multi-object LE image with differing code/data deltas, so a single IDAtoFlat or
  //--- data-delta scan misses refs in other objects). Three non-obvious aliases the scan must catch:
  //---   * the static t_TrkReadbuffer base pointer at IDA 0xF9CC0,
  //---   * pTrkHdrStart = t_TrkReadbuffer + 0x1000 (IDA 0xF9CC4, the "real header" pointer),
  //---   * t_interobjs (IDA 0xD9FDC): the 17 default-object pointers sitting EXACTLY 0x44 B before
  //---     t_LoadedTrkData. The object render loop walks one array t_interobjs[0..NumIntObjects],
  //---     so indices >=17 alias straight into t_LoadedTrkData's file-object table. We therefore put
  //---     t_interobjs at the front of the new buffer (newLoaded = bL+0x44), copy the 17 default
  //---     pointers, and repoint t_interobjs refs too -- otherwise the render reads the file pointers
  //---     from the STALE old buffer -> missing objects + a wild byte-string walk off RAM.
  //--- Also raises the read cap (0x93DF8). See docs/track-length-limit.md.
  if (ExtraTrackFileKB) {
    static const unsigned long loadedRefs[] = {
      0x731A5UL, 0x731B2UL, 0x731D6UL, 0x731E6UL, 0x731F1UL,
      0x73203UL, 0x7322FUL, 0x73240UL, 0x73245UL, 0x77017UL
    };
    static const unsigned long readRefs[] = { 0x7AE03UL, 0x93DF3UL, 0x93E04UL, 0x93E14UL };
    unsigned long extra, cap, oldLoaded, oldRead, newLoaded, newRead, pstat;
    unsigned long oldInterobjs, newInterobjs;   /* t_interobjs (17 default-obj ptrs) sits 0x44 B before t_LoadedTrkData */
    unsigned char *bL, *bR, *c1, *c2;
    int i, off, ok;
    extern unsigned char *pTrackBuf;   /* GP2Lap's track-file pointer (set once at attach from the stock read base) */

    extra = ExtraTrackFileKB;
    if (extra > 256) extra = 256;                 /* clamp: <= ~320 KB total */
    cap = 0x10000UL + extra * 1024UL;             /* new read/file cap = 64KB + N KB (above the stock/community cap) */

    oldLoaded = *(unsigned long *)(IDAtoFlat(0x731D6) + 1);  /* mov edi, offset t_LoadedTrkData (BF,+1) */
    oldRead   = *(unsigned long *)(IDAtoFlat(0x93DF3) + 1);  /* mov edi, offset t_TrkReadbuffer  (BF,+1) */
    c1 = (unsigned char *)IDAtoFlat(0x93DF8);     /* live read cap: mov ecx,imm32 (B9, imm@+1) -- may be pre-patched */
    c2 = (unsigned char *)IDAtoFlat(0x731CE);     /* dead cmp ecx,imm32 (81 F9, imm@+2) */
    if (oldLoaded <= 0x10000UL || oldRead <= 0x10000UL) {
      LogLine("- ExtraTrackFileKB: bootstrap read failed; DISABLED\n");
    } else {
      bL = (unsigned char *)malloc(cap + 0x2000UL + 0x44UL);   /* data buffer; +0x44 front holds t_interobjs */
      bR = (unsigned char *)malloc(cap + 0x2000UL);            /* file read buffer (replaces t_TrkReadbuffer) */
      if (!bL || !bR) {
        if (bL) free(bL);
        if (bR) free(bR);
        sprintf(strbuf, "- ExtraTrackFileKB: alloc FAILED (%lu KB x2); stock ~62KB\n", (cap + 0x2000UL) >> 10);
        LogLine(strbuf);
      } else {
        newInterobjs = (unsigned long)bL;          /* t_interobjs goes at the front... */
        newLoaded    = (unsigned long)bL + 0x44UL; /* ...so t_interobjs+0x44 == t_LoadedTrkData, preserving the */
        oldInterobjs = oldLoaded - 0x44UL;         /* one-array (17 default + N file) object-pointer adjacency  */
        newRead   = (unsigned long)bR;
        pstat     = (oldRead - 0x3B9AD0UL) + 0xF9CC0UL;  /* runtime addr of the static t_TrkReadbuffer ptr */
        ok = 1;                                          /* verify pass (all-or-nothing) */
        for (i = 0; i < 10 && ok; i++) if (SegFindImm(loadedRefs[i], oldLoaded) < 0) ok = 0;
        for (i = 0; i < 4  && ok; i++) if (SegFindImm(readRefs[i],   oldRead)   < 0) ok = 0;
        if (ok && *(unsigned long *)pstat != oldRead) ok = 0;
        if (ok && (c1[0] != 0xB9 || c2[0] != 0x81 || c2[1] != 0xF9)) ok = 0;  /* cap opcodes (value may be pre-patched) */
        if (!ok) {
          free(bL); free(bR);
          LogLine("- ExtraTrackFileKB: site verify failed; DISABLED (no patch)\n");
        } else {
          for (i = 0; i < 10; i++) { off = SegFindImm(loadedRefs[i], oldLoaded); *(unsigned long *)(IDAtoFlat(loadedRefs[i]) + off) = newLoaded; }
          for (i = 0; i < 4;  i++) { off = SegFindImm(readRefs[i],   oldRead);   *(unsigned long *)(IDAtoFlat(readRefs[i])   + off) = newRead;   }
          *(unsigned long *)pstat = newRead;             /* static t_TrkReadbuffer pointer @0xF9CC0 */
          *(unsigned long *)(c1 + 1) = cap;              /* raise the live read cap (0x93DF8) */
          *(unsigned long *)(c2 + 2) = cap;              /* and the dead cmp (0x731CE) for consistency */
          memcpy((void *)newInterobjs, (void *)oldInterobjs, 0x44UL);  /* preserve the 17 default-object pointers */
          memset((void *)newLoaded, 0, cap + 0x2000UL);                /* zero only the track-data region */
          memset(bR, 0, cap + 0x2000UL);
          sprintf(strbuf, "- ExtraTrackFileKB: +%lu KB -> cap %lu KB; read@0x%08lX loaded@0x%08lX interobjs@0x%08lX\n",
                  extra, cap >> 10, newRead, newLoaded, newInterobjs);
          LogLine(strbuf);
          /* --- whole-image self-finding relocation. GP2 is a multi-object LE image: code lives in
             more than one LE object (the deep-load track compiler that crashed is in a different
             object than the loader), and the buffers + their loader-fixup'd immediate references are
             spread across objects at differing runtime deltas. So IDAtoFlat (object-1 code delta) and
             a data-delta scan each cover only part of the image and missed the real reference. Instead
             scan the whole contiguous loaded image BY VALUE (delta-independent): from the object-1 code
             start up to the read buffer, repoint every dword aimed at a buffer base, the t_interobjs
             array base, or the read-buffer +0x1000/+0x1020 header pointer. Buffer bodies are skipped;
             EXACT-value match only (every t_interobjs reference uses the array base -- the slot index
             is always a runtime register -- so a value WINDOW is unnecessary and merely false-matches
             unrelated dwords, e.g. a GP2Lap code hook's call rel32, corrupting it; exact match keeps
             false positives ~0 at 6 MB x ~e-9). The 14 known refs + 0xF9CC0 are already
             newLoaded/newRead so won't re-match. */
          {
            unsigned long flatLo  = (unsigned long)IDAtoFlat(0x10000); /* object-1 code start (mapped) */
            unsigned long flatHi  = oldRead;                           /* read buffer is last; all refs precede it */
            unsigned long lbodyLo = oldLoaded, lbodyHi = oldLoaded + 0xF234UL; /* loaded-buffer body to skip */
            unsigned long p, v;
            int hL = 0, hR = 0, hI = 0;
            for (p = flatLo; p < flatHi; p++) {
              if (p >= lbodyLo && p < lbodyHi) { p = lbodyHi - 1; continue; }  /* skip loaded-buf body */
              v = *(unsigned long *)p;
              if      (v == oldLoaded)            { *(unsigned long *)p = newLoaded; hL++; }
              else if (v == oldInterobjs)         { *(unsigned long *)p = newInterobjs; hI++; }
              else if (v == oldRead)              { *(unsigned long *)p = newRead; hR++; }
              else if (v == oldRead + 0x1000UL)   { *(unsigned long *)p = newRead + 0x1000UL; hR++; }  /* pTrkHdrStart-style header alias */
              else if (v == oldRead + 0x1020UL)   { *(unsigned long *)p = newRead + 0x1020UL; hR++; }
            }
            sprintf(strbuf, "  img-scan: %d loaded + %d interobjs + %d read ref(s) repointed (flat 0x%08lX..0x%08lX)\n",
                    hL, hI, hR, flatLo, flatHi);
            LogLine(strbuf);
          }
          /* GP2Lap captured pTrackBuf from the STOCK read base via a data hook at attach
             (lammcall.asm, 0x7AE04 -> 0x3B9AD0 = t_TrkReadbuffer). It is never refreshed, so
             TFLHook's GP2INFO/editor-track detection + custom names + split segs would read the
             old, now-unused read buffer. Repoint it to the relocated read buffer. */
          if (pTrackBuf) pTrackBuf = (unsigned char *)newRead;
        }
      }
    }
  }

  //--- 2026 --- pit-lane speed limit, matched for AI + player. The limit is the clamp value
  //--- dword_CBA78 (IDA 0xCBA78): the AI clamps its advised speed to it, and the human limiter uses
  //--- CBA78 x d_SpeedFactor1 (IDA 0xD55F0). Derive both from PitSpeedKmh so both hit the same speed:
  //---   CBA78          = T * 0x10000 / 0x464   (AI law verified in-game: km/h = CBA78 * 0x464 >> 16)
  //---   d_SpeedFactor1 = 0x4300                (cancels the player's ~5% under-read -> matches the AI)
  //--- Each data address is bootstrapped from the code operand that references it (the LE loader has
  //--- fixed the absolute operand to its runtime address). 0 = leave stock. See docs/pit-speed-limit.md.
  if (PitSpeedKmh) {
    unsigned char *r1 = (unsigned char *)IDAtoFlat(0x22CDA);  /* cmp ecx, dword_CBA78  (3B 0D <a32>) */
    unsigned char *r2 = (unsigned char *)IDAtoFlat(0x1997D);  /* imul d_SpeedFactor1   (F7 2D <a32>) */
    unsigned long t = PitSpeedKmh;
    if (t < 5UL) t = 5UL; else if (t > 1000UL) t = 1000UL;    /* sanity clamp */
    if (r1[0] == 0x3B && r1[1] == 0x0D && r2[0] == 0xF7 && r2[1] == 0x2D) {
      unsigned long *pCba = *(unsigned long **)(r1 + 2);       /* runtime &dword_CBA78    */
      unsigned long *pFac = *(unsigned long **)(r2 + 2);       /* runtime &d_SpeedFactor1 */
      unsigned long cba = (t * 0x10000UL) / 0x464UL;
      *pCba = cba;
      *pFac = 0x4300UL;
      sprintf(strbuf, "- PitSpeedKmh: %lu km/h -> CBA78=0x%lX, d_SpeedFactor1=0x4300\n", t, cba);
      LogLine(strbuf);
    } else {
      LogLine("- PitSpeedKmh: opcode mismatch; DISABLED (stock pit speed)\n");
    }
  }

  //--- 2026 --- parse the SeasonOverrides file ONCE into the shared model (resolves
  //---          car1/car2/cp1/cp2 to carIds via t_CaridTeamTab); consumers below query it ---
  OverrideLoad();
  //--- 2026 --- per-car car textures (SeasonOverrides / override.cfg) ---
  CarTexInit();
  //--- 2026 --- per-car cockpit colours ([CockpitColors] in the same file) ---
  CarCockpitInit();
  //--- 2026 --- per-team car shapes (nose choice; tier 1) ---
  CarShapeInit();
  //--- 2026 --- configurable pit-stop timing (keys in the SeasonOverrides file) ---
  PitStopsInit();
  //--- 2026 --- per-team mass + downforce (keys in the SeasonOverrides file) ---
  TeamPhysInit();
  //--- 2026 --- AI low-HP "slow launch" fix (speed-faded effective engine power) ---
  AiLaunchFixInit();
  //--- 2026 --- AI banking roll (drawn cars roll onto banked track sections) ---
  AiBankingInit();
  //--- 2026 --- per-driver data (name/skill/range/weight/number) + restore-reapply wrap ---
  DriverDataInit();
  //--- 2026 --- per-team pit-crew colours (t_PitCrewColors, re-expanded by MakeCrewColors) ---
  PitCrewColorsInit();
  //--- 2026 --- small-grid support: clamp the race field to min(N,26) (AllowSmallGrid) ---
  GridCapInit();

  //--- 2026 --- [Weekend] enable/disable sessions (patches the 0xF3 session mask) ---
  SessionsInit();

  //--- 2026 --- [Calendar] variable season length/order (sets round count + t_trackid?) ---
  CalendarInit();

  //--- 2026 --- [Team 15..20] bigger roster: set d_anzteams from the validated added teams ---
  RosterInit();

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

  //--- 2026 --- per-track magic data (.m2d) from the SeasonOverrides [Track N] sections.
  // AFTER CatchTrackInfos so a per-slot .m2d wins magic table 1 (tyre wear) for its slot.
  MagicDataInit();

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
