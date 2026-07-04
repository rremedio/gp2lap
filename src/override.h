#ifndef OVERRIDE_H
#define OVERRIDE_H

/* Shared parser/model for the SeasonOverrides file (INI sections [General] + [Team N]).
   Parsed once at init; cartex/carshape/teamphys/pitstops query the resolved model.
   override.c stores RAW parsed values + "set" flags; consumers keep their own unit
   conversion / clamping / patching. car1/car2/cp1/cp2 are resolved to carIds via
   t_CaridTeamTab (slot 0/1, 0x00 = empty/disabled). See
   docs/plans/2026-06-22-override-team-sections-design.md (in the vault). */

#define OV_STOCKTEAMS 14       /* stock team count (always active); 15..20 are override-added */
#define OV_TEAMS     20        /* max teams the engine supports (t_CaridTeamTab = 40 bytes = 20x2) */
#define OV_MAXCAR    64        /* carId masked to 0x3F (matches cartex CT_MAXCAR) */
#define OV_TRACKS    16        /* track pool size (magic data is per track slot 0..15) */
#define OV_MAXROUNDS 16        /* [Calendar] Rounds cap for 3a; >16 needs the 3b relocation */

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
  int  nose;       int noseSet;   /* 0/1 */
  long mass;       int massSet;   /* kg (raw) */
  long downforce;  int dfSet;     /* % (raw) */
  long dfRange;    int dfRangeSet;/* +/- % random wobble on the DF multiplier, per weekend (1c) */
  OvDriver drv[2];
  long power, qualpower; int powerSet, qualpowerSet;  /* PS 0..1579 */
  long reliability;      int reliabilitySet;          /* 0..32767 */
  unsigned char pitcrew[14]; int pitcrewSet;          /* 14 ramp bases */
  char teamName[13];   int teamNameSet;   /* constructor name, up to 12 chars + NUL */
  char engineName[13]; int engineNameSet; /* engine name, up to 12 chars + NUL */
  char livery[256];    int liverySet;     /* team base body atlas BMP (4a.2); required for teams 15..20 */
  char carLivery[2][256]; int carLiverySet[2]; /* per-seat Car1/Car2 BMP, DEFERRED for added teams
                                        15..20 (t_CaridTeamTab empty at parse time -> resolved to the
                                        seat's Num after the file is read; 4a.2b) */
  char carHelmet[2][256]; int carHelmetSet[2]; /* per-seat Helmet1/Helmet2 BMP, DEFERRED for added
                                        teams (same resolution as carLivery; 4a.3) */
} OvTeam;

typedef struct {               /* resolved per carId (1..OV_MAXCAR-1) */
  char livery[256]; int liverySet;
  char helmet[256]; int helmetSet;   /* per-driver custom helmet BMP (4a.3) */
  unsigned char cp[3]; int cpSet;
} OvCar;

typedef struct {               /* [Track N], indexed [N-1]; N = calendar slot 1..16 */
  char magicData[128];    int magicDataSet;     /* .m2d filename (1a), relative to the override file */
  char overrideFile[128]; int overrideFileSet;  /* per-track override filename (1b), relative to it */
} OvTrack;

void OverrideLoad(void);                  /* GP2Lap: resolve t_CaridTeamTab + parse the cfg file */
const OvGeneral *OverrideGeneral(void);
const OvTeam    *OverrideTeam(int team1); /* 1..14, NULL out of range */
const OvCar     *OverrideCar(int carId);  /* 1..OV_MAXCAR-1, NULL out of range */
const OvTrack   *OverrideTrack(int slot1);/* 1..16 (calendar slot), NULL out of range */
const char      *OverrideBaseDir(void);   /* dir of the loaded override file (with trailing sep), or "" */

/* Roster (4a): number of teams to field = OV_STOCKTEAMS (14, always) + a contiguous run of
   valid override-added teams 15..20. A new team is VALID only with TeamName + EngineName +
   Power + at least one non-disabled seat carrying Name/Num/Qual/Race; the first incomplete
   or absent team stops the count (later teams are ignored). Drives d_anzteams and bounds the
   driver/perf writes. OverrideTeamDefined = the team carries any [Team N] key (to warn on an
   incomplete-but-present team). */
int OverrideActiveTeams(void);
int OverrideTeamDefined(int team1);

/* [Weekend] session enable mask (2a). Starts from stock 0xF3; each per-session key
   sets/clears its bit (0 Fri-prac,1 Fri-qual,2 Sat-prac,3 Sat-qual,4 warmup,5 race).
   Returns 1 if a [Weekend] section was present (and writes the mask), else 0. */
int OverrideWeekend(unsigned char *maskOut);

/* [Weekend] sprint (2b): Sprint=1 turns the warmup slot into a 2nd (shorter) race of
   SprintLaps laps. Returns 1 if Sprint is on (and writes the lap count), else 0. */
int OverrideSprint(int *lapsOut);

/* [Calendar] variable season length (3a). Rounds=N shortens the championship to the first
   N calendar slots (1..OV_MAXROUNDS); slots N..15 are skipped. Track order/content stays
   stock (each slot's track is chosen via its track file + magic-data/per-track override).
   Returns N (>0) if a valid Rounds was parsed, else 0 (stock 16-round season). */
int OverrideCalendarRounds(void);

/* Per-track override file (1b): parse a track's own override into a SEPARATE team model
   (the season model must persist). Only physics keys are honoured (mass/downforce/power/
   qualpower/reliability); other keys are ignored with a warning. Returns the count of
   physics values set, or -1 if the file is missing. OverrideTrackTeam() reads the result. */
int OverrideParseTrackFile(const char *path);
const OvTeam    *OverrideTrackTeam(int team1); /* 1..14, NULL out of range (per-track model) */

/* pure / host-testable: parse 'path', resolve car slots via caridTeamTab (>= OV_TEAMS*2 bytes).
   Returns 0 on success, -1 if the file is missing/unset. */
int OverrideParseFile(const char *path, const unsigned char *caridTeamTab);

#endif /* OVERRIDE_H */
