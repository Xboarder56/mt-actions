import json, os, sys
here = os.path.dirname(os.path.abspath(__file__)); work = sys.argv[1]
os.makedirs(work, exist_ok=True)
H = {"millis": {"int": 12345}, "get_arousal": {"int": 300}, "get_output_mode": {"str": "plugin"},
     "failing": {"error": ["host-dispatch-failed", "device unplugged"]}, "voidfn": {"null": True}}
def S(name, plugin, steps, config=None):
    s = {"plugin": os.path.join(here, "plugins", plugin), "host": H, "steps": steps}
    if config is not None: s["config"] = config
    p = os.path.join(work, name + ".json"); json.dump(s, open(p, "w"), indent=1); print(p)
G = ["g", "f", "s", "cnt", "buf"]
ev = lambda e, a=None: ["event", e, a or []]
S("edge_builtins", "builtins.json", [ev("arith", [3]), ["globals", G], ev("arith", [2.5]), ev("strings"), ["globals", G],
  ev("arrays", [3]), ev("arrays", [9]), ["globals", G], ev("calls", [4]), ["globals", G], ev("calls", [8]), ["globals", G],
  ev("flow", [1]), ["globals", G], ev("flow", [3]), ["globals", G], ev("flow", [7]), ["globals", G], ev("flow", [-1]),
  ev("longloop"), ["globals", G], ev("config"), ev("errors"), ["globals", G], ev("obj_form"), ["globals", G],
  ev("capture"), ["globals", G], ev("hostret"), ["globals", G], ev("nonexistent"), ["call", "twice", [21]],
  ["call", "early", [6]], ["call", "localv", [2]], ["call", "nope", []], ["config", {"level": 3, "on": False, "msg": 7}], ev("config")],
  config={"level": 9, "msg": "cfg"})
S("edge_cycles", "cycles.json", [ev("go"), ["globals", ["a", "x", "y", "z"]], ev("bare"), ev("bare2"), ev("tick", [5000]), ev("go")])
