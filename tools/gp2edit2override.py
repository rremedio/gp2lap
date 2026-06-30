#!/usr/bin/env python3
"""Convert a GP2Edit performance file into a GP2Lap SeasonOverrides listing.

Usage:
    python3 gp2edit2override.py INPUT.txt [OUTPUT.cfg]

If OUTPUT is omitted the listing is printed to stdout.

GP2Edit performance file (e.g. 1989m.txt) looks like:

    ** GP2Edit v1.86 - Performance File **
    [Team #00]
    Name=McLaren,Honda
    Performance=675,705,5718                         ; race power, quali power, reliability
    First Driver=1,Ayrton Senna,32123,32303,1531,0,0 ; num,name,raceSkill,qualiSkill,range,selected,disabled
    Second Driver=2,Alain Prost,32162,31865,1547,1,0
    ...
    [Spare Drivers]                                  ; ignored

Mapping to the SeasonOverrides [Team N] format (N = GP2Edit team index + 1):
    Performance      -> power, qualpower, reliability   (copied as-is)
    First Driver  -> slot 1: num1/name1/race1/qual1/range1/selected1/disabled1
    Second Driver -> slot 2: num2/name2/race2/qual2/range2/selected2/disabled2

Driver skills in the GP2Edit file are stored in the raw 15751..32767 engine range;
they are converted to GP2Lap's 0..17016 author range by subtracting 15751 (0x3D87)
and clamping. Range, power and reliability are passed through unchanged.

Fields the override format supports but the GP2Edit file does not carry
(weightN, shape, nose, mass, downforce, pitcrew, car1/2, cp1/2) are emitted as
commented placeholders so they are easy to fill in.
"""

import sys

SKILL_BIAS = 15751        # 0x3D87: file skill = our skill + bias
SKILL_MAX  = 17016        # our 0..17016 range (file 15751..32767)


def conv_skill(raw):
    """GP2Edit raw skill (15751..32767) -> GP2Lap skill (0..17016), clamped."""
    v = int(raw) - SKILL_BIAS
    if v < 0:
        v = 0
    if v > SKILL_MAX:
        v = SKILL_MAX
    return v


def split_driver(value):
    """Parse 'num,name,raceSkill,qualiSkill,range[,selected[,disabled]]'."""
    parts = [p.strip() for p in value.split(",")]
    if len(parts) < 5:
        return None
    d = {
        "num":   parts[0],
        "name":  parts[1],
        "race":  conv_skill(parts[2]),
        "qual":  conv_skill(parts[3]),
        "range": int(parts[4]),
        "selected": int(parts[5]) if len(parts) > 5 and parts[5] != "" else 0,
        "disabled": int(parts[6]) if len(parts) > 6 and parts[6] != "" else 0,
    }
    return d


def parse(path):
    """Return a list of team dicts: {idx, name, perf:(race,qual,rel), drv:[d1,d2]}."""
    teams = []
    cur = None
    in_teams = False
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            line = line.rstrip("\r\n")
            s = line.strip()
            if not s:
                continue
            if s.startswith("[Team #"):
                in_teams = True
                idx = int(s[s.index("#") + 1: s.index("]")])
                cur = {"idx": idx, "name": "", "perf": None, "drv": [None, None]}
                teams.append(cur)
                continue
            if s.startswith("["):           # any other section ([Spare Drivers], ...) ends teams
                in_teams = False
                cur = None
                continue
            if not in_teams or cur is None:
                continue
            if "=" not in s:
                continue
            key, val = s.split("=", 1)
            key = key.strip()
            val = val.strip()
            if key == "Name":
                cur["name"] = val
            elif key == "Performance":
                p = [x.strip() for x in val.split(",")]
                cur["perf"] = (p[0], p[1], p[2]) if len(p) >= 3 else None
            elif key == "First Driver":
                cur["drv"][0] = split_driver(val)
            elif key == "Second Driver":
                cur["drv"][1] = split_driver(val)
    return teams


def emit_driver(out, slot, d):
    """slot is 1 or 2; d is a driver dict or None."""
    if d is None:
        out.append("; (no %s driver in the file)" % ("first" if slot == 1 else "second"))
        out.append(";num%d =" % slot)
        out.append(";name%d =" % slot)
        out.append(";race%d =" % slot)
        out.append(";qual%d =" % slot)
        out.append(";range%d =" % slot)
        out.append(";selected%d =" % slot)
        out.append(";disabled%d =" % slot)
        out.append(";weight%d =" % slot)
        return
    out.append("num%d = %s" % (slot, d["num"]))
    out.append('name%d = "%s"' % (slot, d["name"]))
    out.append("race%d = %d" % (slot, d["race"]))
    out.append("qual%d = %d" % (slot, d["qual"]))
    out.append("range%d = %d" % (slot, d["range"]))
    out.append("selected%d = %d" % (slot, d["selected"]))
    out.append("disabled%d = %d" % (slot, d["disabled"]))
    out.append(";weight%d =        ; A distribution weight (0..16384), not in GP2Edit" % slot)


def convert(teams, src):
    out = []
    out.append("; SeasonOverrides listing generated from %s" % src)
    out.append("; skills converted to 0..17016 (file value - 15751); power/reliability/range as-is")
    out.append("")
    for t in teams:
        n = t["idx"] + 1                 # GP2Edit #00 -> [Team 1]
        title = ("Team #%02d" % t["idx"]) + ((" - " + t["name"]) if t["name"] else "")
        out.append("; ===== %s =====" % title)
        out.append("[Team %d]" % n)
        if t["perf"]:
            out.append("power = %s        ; race power (PS, 0..1579)" % t["perf"][0])
            out.append("qualpower = %s    ; quali power (PS, 0..1579)" % t["perf"][1])
            out.append("reliability = %s  ; 0..32767 (higher = more fragile)" % t["perf"][2])
        else:
            out.append(";power =")
            out.append(";qualpower =")
            out.append(";reliability =")
        out.append("")
        emit_driver(out, 1, t["drv"][0])
        out.append("")
        emit_driver(out, 2, t["drv"][1])
        out.append("")
        out.append("; --- not in the GP2Edit file; uncomment + set to override ---")
        out.append(";shape =")
        out.append(";nose =")
        out.append(";mass =")
        out.append(";downforce =")
        out.append(";pitcrew =       ; 14 palette indices, comma-separated")
        out.append(";car1 =          ; livery BMP for slot 1")
        out.append(";car2 =")
        out.append(";cp1 =           ; cockpit colour triple b0,b1,b2 for slot 1")
        out.append(";cp2 =")
        out.append("")
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    src = argv[1]
    teams = parse(src)
    if not teams:
        sys.stderr.write("error: no [Team #NN] sections found in %s\n" % src)
        return 1
    text = convert(teams, src)
    if len(argv) > 2:
        with open(argv[2], "w", encoding="latin-1") as f:
            f.write(text)
        sys.stderr.write("wrote %d teams -> %s\n" % (len(teams), argv[2]))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
