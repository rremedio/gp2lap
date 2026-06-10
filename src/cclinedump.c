#include "stdinc.h"
#include "miscahf.h"
#include "cclinedump.h"

/* GP2Lap's captured pointer to the live track-segment array (gp2glob.c). We
** only need its address value, so forward-declare it as void* to avoid pulling
** in gp2glob.h's full type web. Same linker symbol (_pTrackSegs). */
extern void *pTrackSegs;

/*
**  cc-line (racing line) compiler repro capture. See cclinedump.h.
**
**  Hook point: "call sub_787E7" at IDA 0x78F98 (bytes E8 4A F8 FF FF) inside
**  UACalcBestLine. We overwrite only the rel32 operand of that CALL so it
**  targets MyReproThunk (cclasm.asm) instead of sub_787E7; the thunk stashes
**  EDI, calls CaptureRepro(), then tail-jumps to the real sub_787E7. The E8
**  opcode byte is left in place.
**
**  Per iteration, at the hook point:
**    EDI            = ptr to the track segment whose racing-line offset is
**                     about to be computed.
**    dword_C94BC    = cc-line world-path point X (signed, 1/8 world-units).
**    dword_C94C0    = cc-line world-path point Y.
**    dActCCLineArg2 = current command's radius / arg2 (signed).
**    dArg3mArg2divLen = per-segment ramp slope (signed).
**  Segment index = (EDI - *pFirstSegSect1) / 0x6C.
*/

/* ---- shared with the asm thunk (cclasm.asm) -------------------------------
** g_capEDI receives EDI on every hooked iteration; g_real787E7 is the flat
** address the thunk tail-jumps to (the real sub_787E7). Decorated by Watcom
** as _g_capEDI / _g_real787E7, matching the EXTRN names in cclasm.asm. */
unsigned long g_capEDI;
unsigned long g_real787E7;

/* The asm trampolines; PUBLIC MyReproThunk_ / MyLatLonHook_ in cclasm.asm. */
extern void MyReproThunk(void);
extern void MyLatLonHook(void);

/* ---- CODE addresses (fed to IDAtoFlat, the code mapping) ----------------- */
#define IDA_HOOKCALL    0x78F98UL   /* the "call sub_787E7" we patch         */
#define IDA_SUB787E7    0x787E7UL   /* real target of that call              */
#define IDA_LATLONPATCH 0x78A41UL   /* "mov ax, dActCCLineArg2" inside sub_787E7,
                                    ** right after lat/lon + q30 cos/sin are done;
                                    ** patched to a "call MyLatLonHook" detour.  */
/* ---- DATA addresses --------------------------------------------------------
** GP2 keeps code and data at DIFFERENT flat bases, so IDAtoFlat (the CODE
** mapping via GP2_CodeStartAdr) is WRONG for data globals. Data flat address =
** ddelta + ida, where ddelta = (flat of pTrackSegs) - 0x146D14 (its IDA addr).
** This mirrors convert.inc's seg-buffer relocation code. */
#define IDA_PTRACKSEGS  0x146D14UL  /* IDA addr of the seg array (= pTrackSegs) */
#define IDA_C94BC       0xC94BCUL   /* dword_C94BC   (cc-line X)             */
#define IDA_C94C0       0xC94C0UL   /* dword_C94C0   (cc-line Y)             */
#define IDA_ARG2        0xC94B4UL   /* dActCCLineArg2                        */
#define IDA_SLOPE       0xC94B8UL   /* dArg3mArg2divLen                      */
#define IDA_SEGBASE_PTR 0xF9CE0UL   /* pFirstSegSect1 (ptr to seg-array base) */
#define SEG_STRIDE      0x6CUL      /* sizeof(track segment)                 */

/* The cc-line globals (0xC94B4..0xC94C0) live in a data section reached by
** NEITHER the code delta NOR the seg-array (pTrackSegs) delta. Instead of
** guessing the section base, we read the loader-RELOCATED flat address straight
** out of the game's code: sub_787E7's first instruction is
**   FF 35 <abs32>   push dword_C94BC
** and <abs32> is the real runtime flat address of dword_C94BC. Its neighbours
** (consecutive dwords) are dActCCLineArg2=C94BC-8, dArg3mArg2divLen=C94BC-4,
** dword_C94C0=C94BC+4. */
static long *p_arg2, *p_slope, *p_c94bc, *p_c94c0;

/* sub_787E7's 2nd instruction is "pop dword_173F0C" (8F 05 <abs32>); <abs32> is
** the relocated flat address of dword_173F0C (=0x173F0C). The reproject
** intermediates are consecutive dwords around it (same section/relocation):
**   T_TrackDataOfs (cos32 of theta') = 0x173F04 = 173F0C - 8
**   T_NumIntObjs   (sin32 of theta') = 0x173F08 = 173F0C - 4
**   dword_173F0C   (lat)             = 0x173F0C
**   d_ActSegCount  (lon)             = 0x173F10 = 173F0C + 4
** All are LIVE at the 0x78A41 detour (after the lat/lon rotation, before the
** straight/curved branch consumes/overwrites them). */
static long *p_173F0C;          /* lat   */
static long *p_lon;             /* 173F0C+4 */
static long *p_cos32;           /* 173F0C-8 */
static long *p_sin32;           /* 173F0C-4 */

/* extract the relocated dword_C94BC + dword_173F0C addresses from sub_787E7. */
static void MapGlobals(void)
{
	unsigned char *code = IDAtoFlat(IDA_SUB787E7);
	if (code[0] == 0xFF && code[1] == 0x35) {       /* push dword [abs32] */
		unsigned long bc = *(unsigned long *)(code + 2);
		p_c94bc = (long *)bc;
		p_c94c0 = (long *)(bc + 4);
		p_arg2  = (long *)(bc - 8);
		p_slope = (long *)(bc - 4);
	}
	if (code[6] == 0x8F && code[7] == 0x05) {       /* pop dword [abs32] */
		unsigned long f0c = *(unsigned long *)(code + 8);
		p_173F0C = (long *)f0c;
		p_lon    = (long *)(f0c + 4);
		p_cos32  = (long *)(f0c - 8);
		p_sin32  = (long *)(f0c - 4);
	}
}

/* ---- capture buffer ------------------------------------------------------ */
struct ccrec {
	unsigned long seg;      /* raw EDI (segment pointer) */
	unsigned long c94bc;
	unsigned long c94c0;
	unsigned long arg2;
	unsigned long slope;
	/* reproject intermediates, captured by the 0x78A41 in-routine hook: */
	long lat;               /* dword_173F0C   (= (cos32*relx - sin32*rely)>>30) */
	long lon;               /* d_ActSegCount  (= (sin32*relx + cos32*rely)>>30) */
	long cos32;             /* T_TrackDataOfs (Q30 cos of theta')               */
	long sin32;             /* T_NumIntObjs   (Q30 sin of theta')               */
	long f14;               /* tseg+0x14 (the θ' tilt source; signed 16-bit)    */
	long t12;               /* tseg+0x12 (neighbour, for context)               */
};

static struct ccrec g_ccrepro[CCREPRO_MAX];
static unsigned long g_ccn;

/*--------------------------------------------------------------------------
** CaptureRepro: append one record (raw EDI + the four live globals). Called
** by MyReproThunk_ with all registers/flags saved. The data globals are read
** via the DATA mapping (DataFlat), NOT IDAtoFlat (which is code-only). The
** segment index is computed later, in DumpCCLineRepro, from the raw pointer.
**--------------------------------------------------------------------------*/
void CaptureRepro(void)
{
	struct ccrec *r;
	if (g_ccn >= CCREPRO_MAX)
		return;
	if (!p_c94bc)                   /* globals not mapped yet */
		return;
	r = &g_ccrepro[g_ccn++];
	r->seg   = g_capEDI;
	r->c94bc = (unsigned long)*p_c94bc;
	r->c94c0 = (unsigned long)*p_c94c0;
	r->arg2  = (unsigned long)*p_arg2;
	r->slope = (unsigned long)*p_slope;
	/* tseg+0x14 / +0x12 read straight off the segment (EDI), signed 16-bit. */
	r->f14 = (long)*(short *)(g_capEDI + 0x14);
	r->t12 = (long)*(short *)(g_capEDI + 0x12);
	r->lat = r->lon = r->cos32 = r->sin32 = 0;   /* filled by CaptureLatLon */
}

/*--------------------------------------------------------------------------
** CaptureLatLon: the in-routine (0x78A41) hook. sub_787E7 has just finished the
** lat/lon rotation and the q30 cos/sin of the segment frame; all four live in
** consecutive globals. Record them onto the record the PRE-hook just appended
** for this same reproject (g_ccn-1), then return dActCCLineArg2 so the asm stub
** can leave it in AX (the instruction we replaced was "mov ax, dActCCLineArg2").
** MUST always return arg2 (even when not recording) so the game's branch is OK.
**--------------------------------------------------------------------------*/
unsigned long CaptureLatLon(void)
{
	if (p_173F0C && g_ccn > 0 && g_ccn <= CCREPRO_MAX) {
		struct ccrec *r = &g_ccrepro[g_ccn - 1];
		r->lat   = *p_173F0C;
		r->lon   = *p_lon;
		r->cos32 = *p_cos32;
		r->sin32 = *p_sin32;
	}
	return p_arg2 ? (unsigned long)*p_arg2 : 0;
}

/*--------------------------------------------------------------------------
** InstallCCLineHook: repoint the CALL at IDA 0x78F98 to our thunk. Call once
** at startup (InitGP2Hook), before any track loads. (Data globals are mapped
** lazily at capture time via DataFlat, since pTrackSegs may be NULL here.)
**--------------------------------------------------------------------------*/
void InstallCCLineHook(void)
{
	unsigned char *pcall;
	unsigned long  newrel;

	g_real787E7 = (unsigned long)IDAtoFlat(IDA_SUB787E7);

	MapGlobals();   /* extract relocated C94BC address from the game's code */

	/* Patch the rel32 of the existing "E8 <rel32>" call. The CPU computes
	** the target as (address-of-byte-after-call) + rel32, and the byte after
	** the 5-byte call is at flat(0x78F98)+5. So:
	**   newrel = thunk - (flat(0x78F98) + 5)                              */
	pcall  = IDAtoFlat(IDA_HOOKCALL);
	newrel = (unsigned long)MyReproThunk - ((unsigned long)pcall + 5);
	*(unsigned long *)(pcall + 1) = newrel;   /* leave the E8 opcode byte */

	/* SECOND hook: overwrite the 6-byte "mov ax, dActCCLineArg2" (66 A1 B4 94 0C
	** 00) at IDA 0x78A41 with a 5-byte "call MyLatLonHook" + a NOP. MyLatLonHook
	** records the live lat/lon/cos32/sin32, then returns dActCCLineArg2 in AX so
	** the replaced instruction's effect is preserved (the following
	** "or ax, dActCCLineArg2+2 / jnz" zero-test still works). */
	{
		unsigned char *pmov = IDAtoFlat(IDA_LATLONPATCH);
		unsigned long  rel  = (unsigned long)MyLatLonHook - ((unsigned long)pmov + 5);
		pmov[0] = 0xE8;                       /* call rel32 */
		*(unsigned long *)(pmov + 1) = rel;
		pmov[5] = 0x90;                       /* NOP the 6th orphaned byte */
	}

	g_ccn = 0;
}

/*--------------------------------------------------------------------------
** DumpCCLineRepro: write the run to <base>\<datadir>\ccrepNN.txt as TSV (NN =
** slotsuf, the 2-digit track slot), then reset the counter so the next track
** load starts fresh. Values are printed signed (%ld). segidx = (seg - *p_segbase) / 0x6C.
**--------------------------------------------------------------------------*/
void DumpCCLineRepro(const char *base, const char *datadir, const char *slotsuf)
{
	char path[_MAX_PATH];
	FILE *f;
	unsigned long i;
	unsigned long pts, ddelta, cdelta, edi0;
	unsigned long sb_data, sb_code;

	if (!base)
		return;

	/* slot-suffixed name (e.g. ccrep03.txt for slot 3 / F1CT03); 8.3-safe so
	** each track load dumps to its own file without manual renaming. */
	sprintf(path, "%s\\%s\\ccrep%s.txt", base, datadir, slotsuf ? slotsuf : "");
	f = fopen(path, "w");
	if (!f)
		return;

	pts    = (unsigned long)pTrackSegs;
	ddelta = pts - IDA_PTRACKSEGS;
	cdelta = GP2_CodeStartAdr - 0x10020UL;
	edi0   = g_ccn ? g_ccrepro[0].seg : 0;
	sb_data = (unsigned long)p_c94bc;                 /* extracted C94BC addr  */
	sb_code = p_arg2 ? (unsigned long)*p_arg2 : 0;    /* live arg2 end-of-load */

	/* ---- DIAGNOSTIC HEADER (lines starting with '#') ---- */
	fprintf(f, "# pTrackSegs=0x%08lX  ddelta=0x%08lX  cdelta=0x%08lX  eq=%d\n",
		pts, ddelta, cdelta, (ddelta == cdelta));
	fprintf(f, "# edi0=0x%08lX  edi0-pTrackSegs=%ld (idx %ld)\n",
		edi0, (long)(edi0 - pts), (long)((edi0 - pts) / SEG_STRIDE));
	fprintf(f, "# C94BC_flat=0x%08lX (from sub_787E7 push operand)  live_arg2=%ld\n",
		sb_data, (long)sb_code);

	/* segidx computed directly from pTrackSegs (the base bestline.txt uses).
	** Extra columns lat..sin32 are the reproject intermediates (0x78A41 hook) for
	** stage-by-stage validation of the C port. */
	fprintf(f, "idx\tsegidx\tc94bc\tc94c0\targ2\tslope\tlat\tlon\tcos32\tsin32\tf14\tt12\n");
	for (i = 0; i < g_ccn; i++) {
		struct ccrec *r = &g_ccrepro[i];
		long segidx = (long)((r->seg - pts) / SEG_STRIDE);
		fprintf(f, "%lu\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\n",
			i, segidx,
			(long)r->c94bc, (long)r->c94c0,
			(long)r->arg2,  (long)r->slope,
			r->lat, r->lon, r->cos32, r->sin32,
			r->f14, r->t12);
	}
	fclose(f);

	g_ccn = 0;
}
