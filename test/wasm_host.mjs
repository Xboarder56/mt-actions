// Replays a mta_host scenario against the official mtp-core WASM runtime (@maustec/mt-runtimes)
// and prints the same trace format as test/mta_host.c.
//
// usage: node wasm_host.mjs <path-to-mt-runtimes-package> <scenario.json>
import { readFileSync } from "node:fs";
import { resolve, dirname } from "node:path";

const [pkg, scenarioPath] = process.argv.slice(2);
const { instantiateMtpCore } = await import(resolve(pkg, "dist/loader.js"));
const scenario = JSON.parse(readFileSync(scenarioPath, "utf8"));
const host = scenario.host || {};
const out = (s) => process.stdout.write(s + "\n");
const f4 = (v) => Math.fround(v).toFixed(4);
let loaded = false;
let clock = 0, motor = 0; // simulated millis() and motor speed

const fmtArg = (a) =>
  a.tag === "int-val" ? `i:${a.val}` :
  a.tag === "float-val" ? `f:${f4(a.val)}` :
  a.tag === "str-val" ? `s:"${a.val}"` : "null";

const ERR = ["unknown", "var-not-set", "cycle-detected", "missing-return", "arg-count-mismatch",
  "missing-arg", "unknown-arg", "type-mismatch", "host-dispatch-failed"];

const root = await instantiateMtpCore({
  host: {
    hostDispatch(slot, name, args) {
      out(`HOST ${name}(${args.map(fmtArg).join(", ")})`);
      if (name === "millis") return { value: { tag: "int-val", val: clock } };
      if (name === "get_motor_speed") return { value: { tag: "int-val", val: motor } };
      if (name === "set_motor_speed" && args.length) {
        motor = args[0].tag === "float-val" ? Math.trunc(args[0].val) : args[0].val;
        return { value: { tag: "null-val" } };
      }
      const r = host[name];
      if (!r) return { value: { tag: "int-val", val: 0 } };
      if (r.error) return { value: { tag: "null-val" }, error: { kind: r.error[0], message: r.error[1] } };
      if ("int" in r) return { value: { tag: "int-val", val: r.int } };
      if ("float" in r) return { value: { tag: "float-val", val: r.float } };
      if ("str" in r) return { value: { tag: "str-val", val: r.str } };
      return { value: { tag: "null-val" } };
    },
    hostResultFree() {},
    configSave(slot) { if (!loaded) return false; out("CONFIG_SAVE"); return true; },
    traceEvent(slot, kind, name, ret) { out(`TRACE ${kind} ${name} ${ret}`); },
    errorReport(slot, msg, code) { if (loaded) out(`ERROR ${code} ${msg}`); },
  },
});

const bridge = root.bridge ?? root["mtp:core/bridge@0.2.0"];
bridge.init();
const pluginText = readFileSync(resolve(dirname(scenarioPath), scenario.plugin), "utf8");
let slot;
try { slot = bridge.loadPlugin(pluginText); } catch (e) { out("LOAD failed"); process.exit(1); }
loaded = true;
out(`LOAD ${bridge.getPluginName(slot) ?? ""}`);
if (scenario.config) bridge.setPluginConfig(slot, JSON.stringify(scenario.config));
bridge.setTracing(true);

const toArg = (v) => typeof v === "number"
  ? (Number.isInteger(v) ? { tag: "int-val", val: v } : { tag: "float-val", val: v })
  : typeof v === "string" ? { tag: "str-val", val: v } : { tag: "int-val", val: 0 };

const printResult = (r) => {
  const v = r.value;
  if (v.tag === "null-val") out(`RESULT null error=${r.error}`);
  else if (v.tag === "int-val") out(`RESULT int ${v.val} error=${r.error}`);
  else if (v.tag === "float-val") out(`RESULT float ${f4(v.val)} error=${r.error}`);
  else out(`RESULT str "${v.val}" error=${r.error}`);
};

for (const step of scenario.steps) {
  const [op, a, b] = step;
  if (op === "event" || op === "call") {
    out(`${op === "event" ? "FIRE" : "CALL"} ${a}`);
    const args = (b || []).map(toArg);
    printResult(op === "event" ? bridge.fireEvent(slot, a, args) : bridge.callFunction(slot, a, args));
  } else if (op === "globals") {
    for (const k of a) {
      const v = bridge.getGlobalValue(slot, k);
      if (!v) out(`GLOBAL ${k}=<none>`);
      else if (v.tag === "int-val") out(`GLOBAL ${k}=i:${v.val}`);
      else if (v.tag === "str-val") out(`GLOBAL ${k}=s:"${v.val}"`);
      else out(`GLOBAL ${k}=b:${v.val}`);
    }
  } else if (op === "advance") {
    clock += a;
  } else if (op === "config") {
    bridge.setPluginConfig(slot, JSON.stringify(a));
    out("CONFIG set");
  }
}
