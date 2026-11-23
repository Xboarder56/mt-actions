"""Generate a plugin where every test action is its own event, plus a scenario firing all of them."""
import json, os, sys
here = os.path.dirname(os.path.abspath(__file__)); work = sys.argv[1]
os.makedirs(work, exist_ok=True)
A = [
 {"add": ["$x", 1.25]}, {"sub": ["$x", 3]}, {"mul": ["$x", -2]}, {"div": ["$x", 4]}, {"div": [1, 0]}, {"div": [0, 0.0]},
 {"mod": [17, 5]}, {"mod": [17, 0]}, {"mod": [-7, 3]}, {"mod": [7.9, 2]}, {"mod": ["7", "2"]}, {"mod": ["", 2]}, {"mod": ["x", 2]},
 {"round": -2.5}, {"round": 2.4999}, {"round": "$x"}, {"round": "abc"},
 {"inc": "$cnt"}, {"dec": "$cnt"}, {"inc": "$s"}, {"inc": "$f"}, {"inc": "$nope"}, {"inc": 5}, {"inc": "$"}, {"inc": ["$cnt", 1]},
 {"set": ["$f", 3.75]}, {"set": ["$g", "$s"]}, {"set": ["$new", 1]}, {"set": [1, 2]}, {"set": 5}, {"set": "$s"}, {"set": "$buf"}, {"set": []},
 {"strlen": "hello"}, {"strlen": 5}, {"strlen": "$s"}, {"charat": ["hello", 1]}, {"charat": ["hello", 9]}, {"charat": ["hello", -1]}, {"charat": ["", 0]},
 {"chr": 65}, {"chr": 300}, {"chr": -1}, {"chr": 0}, {"to_string": 42}, {"to_string": 1.5}, {"to_string": 1e20}, {"to_string": "x"}, {"to_string": "$buf"},
 {"concat": ["a", "b", "c"]}, {"concat": ["n=", 5]}, {"concat": []}, {"concat": ["a" * 200, "b" * 200]}, {"concat": ["$s", "$s"]},
 {"strcmp": ["a", "b"]}, {"strcmp": ["b", "b"]}, {"strcmp": [1, "b"]},
 {"eq": [1, 1.0]}, {"eq": ["1", 1]}, {"neq": [1, 2]}, {"gt": [2, 1]}, {"lt": [2, 1]}, {"gte": [1, 1]}, {"lte": [2, 1]}, {"eq": ["a", 1]}, {"eq": ["A", 65]},
 {"add": ["12", "3.5"]}, {"add": ["A", 1]}, {"add": ["12abc", 1]}, {"add": ["", 1]}, {"add": [1]}, {"add": [1, 2, 3]}, {"add": ["$undef", 1]},
 {"add": {"a": 1, "b": 2}}, {"add": 5}, {"add": [1, 2], "to": "$cnt"}, {"add": [1, 2], "to": "cnt2"}, {"add": [1, 2], "to": "$_"},
 {"setbyte": ["$buf", "$x", 300]}, {"getbyte": ["$buf", "$x"]}, {"setbyte": ["$buf", 8, 1]}, {"getbyte": ["$buf", -1]}, {"getbyte": ["$g", 0]},
 {"setbyte": ["$buf", 2, "x"]}, {"getbyte": ["$buf", 7]}, {"setbyte": ["$buf", 1.9, 2.9]},
 {"get_config": "level"}, {"get_config": "on"}, {"get_config": "msg"}, {"get_config": "nope"}, {"set_config": ["level", 8]}, {"set_config": ["level", 2.7]},
 {"set_config": ["msg", "yo"]}, {"set_config": ["on", 0]}, {"set_config": ["msg", 5]}, {"set_config": ["newkey", 5]}, {"get_config": 5},
 {"nosuchfn": [1]}, {"notify": "permitted"}, {"lock_controls": "denied"}, {"set": ["$a1", "$a2"]}, {"millis": [], "to": "$g"}, {"get_arousal": [], "to": "$_"},
 {"get_output_mode": [], "to": "$s"}, {"failing": []}, {"voidfn": [], "to": "$g"}, {"voidfn": []}, {"get_motor_speed": 5},
 {"@twice": "$x", "to": "$g"}, {"twice": [3]}, {"@early": 9}, {"@early": "$x"}, {"@bump": []}, {"@noargs": []}, {"@localv": 5},
 {"@twice": "$nope"}, {"@missing": 1}, {"@twice": []}, {"@twice": [1, 2, 3]}, {"@recur": 3}, {"@globalset": 42}, {"@retstr": []}, {"@retarr": []},
 {"unless": {"eq": ["$x", 1], "then": [{"set": ["$g", 100]}], "else": [{"set": ["$g", 200]}]}},
 {"if": {"any": [{"eq": ["$x", 3]}, {"eq": ["$x", 4]}], "then": [{"inc": "$g"}], "else": [{"dec": "$g"}]}},
 {"if": {"none": [{"eq": ["$x", 3]}], "then": [{"dec": "$g"}]}},
 {"if": {"all": [{"gt": ["$x", 0]}, {"lt": ["$x", 10]}], "any": [{"eq": [1, 1]}], "then": [{"inc": "$cnt"}]}},
 {"if": {"all": [], "then": [{"inc": "$cnt"}]}}, {"if": {"any": [], "then": [{"inc": "$cnt"}], "else": [{"dec": "$cnt"}]}},
 {"if": {"eq": ["$x", 2], "gt": ["$x", 1], "then": [{"inc": "$cnt"}]}}, {"if": {"strcmp": ["$s", "str"], "then": [{"inc": "$cnt"}]}},
 {"if": {"then": [{"inc": "$cnt"}], "else": [{"dec": "$cnt"}]}}, {"if": {"all": [{"nosuch": []}], "then": [{"inc": "$cnt"}], "else": [{"dec": "$cnt"}]}},
 {"if": {"eq": ["$x", 5], "then": [{"return": 77}]}}, {"if": {"eq": ["$x", 5], "then": [{"add": ["oops", 1]}]}},
 {"if": 5}, {"while": 5}, {"unless": {"then": []}},
 {"while": {"lt": ["$cnt", "$x"], "then": [{"inc": "$cnt"}]}}, {"while": {"any": [{"lt": ["$cnt", 3]}], "then": [{"inc": "$cnt"}, {"if": {"eq": ["$cnt", 2], "then": [{"return": 5}]}}]}},
 {"while": {"none": [{"gt": ["$cnt", 5]}], "then": [{"inc": "$cnt"}]}}, {"while": {"lt": ["$cnt", 3], "then": [{"add": ["q", 1]}]}},
 {"return": "$x"}, {"return": "$nope"}, {"return": []}, {"return": "lit"}, {"return": 1.5},
 {"log": "$x"}, {"random": [1, 6]}, {"delay": 1}, {"ble_write": "hi"},
]
fns = {
 "twice": {"args": ["x"], "actions": [{"mul": ["$x", 2]}, {"return": "$_"}]},
 "early": {"args": ["x"], "actions": [{"if": {"gt": ["$x", 5], "then": [{"return": 99}]}}, {"return": 1}]},
 "bump": {"actions": [{"inc": "$g"}, {"set": ["$g", "$g"]}]},
 "noargs": {"actions": [{"set": 3}]},
 "localv": {"args": ["a"], "vars": {"tmp": 7, "arr[4]": 0, "sv": "local"}, "actions": [{"add": ["$a", "$tmp"]}, {"setbyte": ["$arr", 1, "$_"]}, {"getbyte": ["$arr", 1]}]},
 "recur": {"args": ["n"], "actions": [{"if": {"lte": ["$n", 0], "then": [{"return": 0}]}}, {"sub": ["$n", 1]}, {"@recur": "$_"}, {"add": ["$_", 1]}]},
 "globalset": {"args": ["v"], "actions": [{"set": ["$g", "$v"]}, {"set": ["$cnt", "$v"]}, {"set": ["$tmpx", 1]}]},
 "retstr": {"actions": [{"concat": ["r", "s"]}]},
 "retarr": {"actions": [{"set": "$buf"}, {"return": "$buf"}]},
}
events = {"t%03d" % i: {"args": ["x"], "actions": [a]} for i, a in enumerate(A)}
events["objform"] = {"actions": {"set": ["$g", 11], "inc": "$g", "to": "$cnt"}}
events["bare"] = ["notastring", {"add": [1, 1]}, 5, {}, {"inc": "$cnt"}]
events["direct"] = [{"inc": "$cnt"}]
events["permitted"] = {"actions": [{"inc": "$cnt"}]}
plugin = {"name": "atomic", "display_name": "", "type": "feature", "permissions": ["ui:notify", "motor:*", "ble:write"],
  "config": {"level": {"type": "int", "default": 5, "min": 0, "max": 10, "label": "Level"}, "on": {"type": "bool", "default": True},
             "msg": {"type": "string", "default": "hi"}, "bad": {"default": 1}},
  "variables": {"g": 0, "f": 1.5, "s": "str", "buf[8]": 0, "cnt": 0, "tmp": 3},
  "functions": fns, "events": events}
os.makedirs(os.path.join(work, "plugins"), exist_ok=True)
pdir = os.path.join(work, "plugins", "atomic_plugin.json"); json.dump(plugin, open(pdir, "w"), indent=1)
H = {"millis": {"int": 12345}, "get_arousal": {"int": 300}, "get_output_mode": {"str": "plugin"}, "get_motor_speed": {"int": 9},
     "failing": {"error": ["host-dispatch-failed", "device unplugged"]}, "voidfn": {"null": True}}
G = ["g", "f", "s", "cnt", "buf", "new", "tmp", "cnt2", "_"]
steps = []
for xv in (3, 5, -2, 2.5, "7"):
    for e in events:
        steps.append(["event", e, [xv]]); steps.append(["globals", G])
steps += [["call", "twice", [21]], ["call", "early", [6]], ["call", "localv", [2]], ["call", "nope", []], ["call", "recur", [5]],
          ["call", "retstr", []], ["call", "retarr", []], ["config", {"level": 3, "on": False, "msg": 7}], ["event", "t090", [0]], ["globals", G]]
for name, cfg in (("atomic_a", {"level": 9, "msg": "cfg", "on": False}), ("atomic_b", None)):
    s = {"plugin": pdir, "host": H, "steps": steps}
    if cfg is not None: s["config"] = cfg
    p = os.path.join(work, name + ".json"); json.dump(s, open(p, "w"), indent=1); print(p)
