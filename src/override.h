#ifndef OVERRIDE_H
#define OVERRIDE_H

/* Shared parser/model for the SeasonOverrides file (INI sections [General] + [Team N]).
   Parsed once at init; cartex/carshape/teamphys/pitstops query the resolved model.
   override.c stores RAW parsed values + "set" flags; consumers keep their own unit
   conversion / clamping / patching. car1/car2/cp1/cp2 are resolved to carIds via
   t_CaridTeamTab (slot 0/1, 0x00 = empty/disabled). See
   docs/plans/2026-06-22-override-team-sections-design.md (in the vault). */

#define OV_TEAMS   14
#define OV_MAXCAR  64          /* carId masked to 0x3F (matches cartex CT_MAXCAR) */

/* [General] "set" bits */
#define OVG_REFUELSPEED   (1u<<0)
#define OVG_REFUELBASE    (1u<<1)
#define OVG_REFUELCAP     (1u<<2)
#define OVG_TYRECHANGE    (1u<<3)
#define OVG_DAMAGEREPAIR  (1u<<4)
#define OVG_DISABLEREFUEL (1u<<5)

typedef struct {
  long pitRefuelSpeed;
  long pitRefuelBaseMs;
  long pitRefuelCapMs;
  long pitTyreChangeMs;
  long pitDamageRepairMs;
  long disableRefuel;
  unsigned set;                /* OVG_* bit per key present */
} OvGeneral;

typedef struct {               /* [Team N], indexed [N-1] */
  char shape[256]; int shapeSet;
  int  nose;       int noseSet;   /* 0/1 */
  long mass;       int massSet;   /* kg (raw) */
  long downforce;  int dfSet;     /* % (raw) */
} OvTeam;

typedef struct {               /* resolved per carId (1..OV_MAXCAR-1) */
  char livery[256]; int liverySet;
  unsigned char cp[3]; int cpSet;
} OvCar;

void OverrideLoad(void);                  /* GP2Lap: resolve t_CaridTeamTab + parse the cfg file */
const OvGeneral *OverrideGeneral(void);
const OvTeam    *OverrideTeam(int team1); /* 1..14, NULL out of range */
const OvCar     *OverrideCar(int carId);  /* 1..OV_MAXCAR-1, NULL out of range */

/* pure / host-testable: parse 'path', resolve car slots via caridTeamTab (>= OV_TEAMS*2 bytes).
   Returns 0 on success, -1 if the file is missing/unset. */
int OverrideParseFile(const char *path, const unsigned char *caridTeamTab);

#endif /* OVERRIDE_H */
