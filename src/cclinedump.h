#ifndef _CCLINEDUMP_H
#define _CCLINEDUMP_H

/*
**  cc-line (racing line) compiler repro capture.
**
**  Hooks the "call sub_787E7" inside GP2's UACalcBestLine (IDA 0x78F98) via a
**  tail-call trampoline (MyReproThunk, in cclasm.asm). Each iteration captures
**  the live segment pointer (EDI) plus four cc-line globals, so the racing-line
**  compiler's intermediate state can be reverse-engineered offline.
**
**  Output: <gp2dir>\<datadir>\ccrepNN.txt (TSV, see DumpCCLineRepro), where NN
**  is the track slot 01-16 so each track load gets its own file.
*/

/* Capture buffer cap; UACalcBestLine appends ~1493 records per track load. */
#define CCREPRO_MAX 4096

/* Install the hook. Call once, early (from InitGP2Hook), before any track
** load runs UACalcBestLine. Resets the capture counter. */
void InstallCCLineHook(void);

/* Write the captured records to <base>\<datadir>\ccrepNN.txt (NN = slotsuf,
** the 2-digit track slot) and reset the counter for the next track load. Call
** from DumpTrackSegData (track.c). */
void DumpCCLineRepro(const char *base, const char *datadir, const char *slotsuf);

/* C side of the trampoline; appends one record. Called by MyReproThunk_. */
void CaptureRepro(void);

/* C side of the SECOND (in-routine) hook, patched over "mov ax, dActCCLineArg2"
** at IDA 0x78A41 inside sub_787E7 (right after lat/lon + the q30 cos/sin of the
** segment frame are computed, before the straight/curved branch). Records the
** reproject intermediates (lat, lon, cos32, sin32) onto the current ccrec, then
** RETURNS dActCCLineArg2 so MyLatLonHook_ can leave it in AX (re-doing the
** instruction it replaced). Lets the C port be validated stage-by-stage. */
unsigned long CaptureLatLon(void);

#endif
