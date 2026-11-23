#!/usr/bin/env python3
"""
Differential test: replay scenarios against the reconstructed native runtime (mta_host) and the
official WASM runtime (wasm_host.mjs + @maustec/mt-runtimes), and diff the traces.

usage: difftest.py --mta-host BIN --runtimes PKG --work DIR [--plugins DIR] [scenario.json ...]

With --plugins, scenarios are generated for every */*/plugin.json found under DIR.
"""
import argparse, glob, json, os, subprocess, sys, difflib

HERE = os.path.dirname(os.path.abspath(__file__))

HOST_VALUES = {
    "millis": {"int": 12345}, "get_arousal": {"int": 300}, "get_pressure": {"int": 2100},
    "get_pressure_average": {"int": 2000}, "get_arousal_threshold": {"int": 600},
    "get_motor_speed": {"int": 77}, "get_output_mode": {"str": "plugin"},
    "get_system_config": {"str": "42"}, "random": {"int": 7},
}

def vars_of(d):
    v = d.get("variables") or []
    names = list(v.keys()) if isinstance(v, dict) else list(v)
    return [n.split("[")[0] for n in names]

def gen_scenarios(plugin_path):
    d = json.load(open(plugin_path))
    events = list(d.get("events", {}).keys())
    fns = list(d.get("functions", {}).keys())
    g = vars_of(d)
    out = []
    for variant, argsets in (("a", [[0], [50], [128], [255], []]), ("b", [[100], [99], [101], [1], [-5]])):
        steps = []
        for ev in events:
            for args in argsets:
                a = [x * 100 for x in args] if ev == "tick" else args
                steps.append(["event", ev, a])
                if g: steps.append(["globals", g])
        for fn in fns:
            for args in argsets:
                steps.append(["call", fn, args])
                if g: steps.append(["globals", g])
        out.append({"plugin": os.path.abspath(plugin_path), "host": HOST_VALUES, "steps": steps})
    return out

def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    return p.stdout + (("STDERR " + p.stderr) if p.returncode else "")

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mta-host", required=True)
    ap.add_argument("--runtimes", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--plugins")
    ap.add_argument("scenarios", nargs="*")
    a = ap.parse_args()
    os.makedirs(a.work, exist_ok=True)

    scen = list(a.scenarios)
    if a.plugins:
        for p in sorted(glob.glob(os.path.join(a.plugins, "*", "*", "plugin.json"))):
            for i, s in enumerate(gen_scenarios(p)):
                name = os.path.join(a.work, "%s_%s_%d.json" % (p.split(os.sep)[-3], p.split(os.sep)[-2], i))
                json.dump(s, open(name, "w"), indent=1)
                scen.append(name)

    fails = 0
    for s in scen:
        sp = os.path.abspath(s)
        ours = run([a.mta_host, sp])
        theirs = run(["node", os.path.join(HERE, "wasm_host.mjs"), a.runtimes, sp])
        base = os.path.splitext(sp)[0]
        open(base + ".ours.txt", "w").write(ours)
        open(base + ".official.txt", "w").write(theirs)
        lines = len(theirs.splitlines())
        if ours == theirs:
            print("PASS %-55s (%d lines)" % (os.path.basename(s), lines))
        else:
            fails += 1
            print("FAIL %-55s" % os.path.basename(s))
            diff = list(difflib.unified_diff(theirs.splitlines(), ours.splitlines(), "official", "ours", lineterm="", n=2))
            print("\n".join(diff[:40]))
    print("%d/%d scenarios identical" % (len(scen) - fails, len(scen)))
    sys.exit(1 if fails else 0)

if __name__ == "__main__":
    main()
