"""
gen_plugin_api.py - generate the plugin API descriptor (plugin-api.json) for a host firmware.

Runs as a PlatformIO post-build script (registered via `extra_scripts = post:...`) and can also
be run standalone:

    python3 gen_plugin_api.py --project-dir <firmware> [--out plugin-api.json] [--version X]

Configuration is read from the project's mtota.json:

    "plugin-api": {
        "product": "edge-o-matic",
        "actions_dir": "src/actions",
        "events_header": "include/actions/index.h"
    }

Host functions are discovered from `mta_register_system_function("name", fn, PERMISSION)` calls
in actions_dir/*.c, events from `mta_register_event("name", PERMISSION)` calls in the events
header. Permissions may be string literals, NULL, or #define'd string macros. Each entry is
documented by a preceding doc comment:

    /**
     * Description text.
     *
     * @plugin name          (or @event name)
     * @module module
     * @arg name:type[?] description
     * @returns type description
     * @payload name:type description
     */

Outputs <out> (JSON, 2-space indent) and <out without .json>.sha256 (hex digest of the file).
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys

SCHEMA_URL = "https://schemas.maus-tec.com/plugin-api/v1.3.0/plugin-api.json"
KNOWN_TAGS = ("plugin", "event", "module", "arg", "returns", "payload")

FUNC_RE = re.compile(
    r'mta_register_system_function\s*\(\s*"([^"]+)"\s*,\s*\w+\s*,\s*(NULL|"[^"]*"|\w+)\s*\)'
)
EVENT_RE = re.compile(r'mta_register_event\s*\(\s*"([^"]+)"\s*,\s*(NULL|"[^"]*"|\w+)\s*\)')
DOC_RE = re.compile(r"/\*\*(.*?)\*/", re.S)


def log(msg):
    print("[gen_plugin_api] %s" % msg)


def warn(msg):
    print("[gen_plugin_api] WARNING: %s" % msg, file=sys.stderr)


# ---------------------------------------------------------------------------
# Version
# ---------------------------------------------------------------------------


def git_version(project_dir):
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--long", "--abbrev=8"],
            cwd=project_dir,
            stderr=subprocess.STDOUT,
        ).decode("utf-8").strip()
    except (subprocess.CalledProcessError, OSError) as e:
        warn("git describe failed: %s" % e)
        return "0.0.0"

    m = re.match(r"^(.*)-(\d+)-g([0-9a-f]+)$", out)
    if not m:
        return out.lstrip("v")

    tag, n, sha = m.group(1).lstrip("v"), int(m.group(2)), m.group(3)
    return tag if n == 0 else "%s-%d-g%s" % (tag, n, sha)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------


def extract_defines(source):
    return dict(re.findall(r'#define\s+(\w+)\s+"([^"]+)"', source))


def resolve_permission(raw, defines):
    raw = raw.strip()
    if raw == "NULL":
        return None
    if raw.startswith('"') and raw.endswith('"'):
        return raw[1:-1]
    if raw in defines:
        return defines[raw]
    warn("unresolved permission token '%s'" % raw)
    return raw


def parse_doc(body):
    """Parse a doc comment body into (description, [(tag, value), ...])."""
    lines = []
    for line in body.split("\n"):
        line = line.strip()
        if line.startswith("*"):
            line = line[1:].strip()
        lines.append(line)

    desc_parts = []
    tags = []

    for line in lines:
        m = re.match(r"^@(\w+)\s*(.*)$", line)
        if m and m.group(1) in KNOWN_TAGS:
            tags.append((m.group(1), m.group(2).strip()))
        elif line:
            desc_parts.append(line)

    return " ".join(desc_parts), tags


def parse_typed(value):
    """'name:type[?] description' -> (name, type, optional, description)."""
    m = re.match(r"^(\w+)\s*:\s*(\w+)(\?)?\s*(.*)$", value)
    if not m:
        return None
    return m.group(1), m.group(2), bool(m.group(3)), m.group(4).strip()


def collect_docs(source, name_tag):
    """Map documented names (by @plugin / @event) to their parsed doc comments."""
    docs = {}
    for m in DOC_RE.finditer(source):
        desc, tags = parse_doc(m.group(1))
        names = [v for (t, v) in tags if t == name_tag]
        if names:
            docs[names[0].split()[0]] = (desc, tags)
    return docs


def build_function(name, permission, doc):
    entry = {"name": name}
    desc, tags = doc if doc else ("", [])

    module = next((v for (t, v) in tags if t == "module"), None)
    if module:
        entry["module"] = module
    entry["permission"] = permission
    if desc:
        entry["description"] = desc

    args = []
    for t, v in tags:
        if t != "arg":
            continue
        parsed = parse_typed(v)
        if not parsed:
            warn("%s: malformed @arg '%s'" % (name, v))
            continue
        arg_name, arg_type, optional, arg_desc = parsed
        arg = {"name": arg_name, "type": arg_type}
        if optional:
            arg["optional"] = True
        if arg_desc:
            arg["description"] = arg_desc
        args.append(arg)
    if args:
        entry["args"] = args

    for t, v in tags:
        if t != "returns":
            continue
        parts = v.split(None, 1)
        if not parts:
            continue
        ret = {"type": parts[0]}
        if len(parts) > 1 and parts[1].strip():
            ret["description"] = parts[1].strip()
        entry["returns"] = ret
        break

    return entry


def build_event(name, permission, doc):
    entry = {"name": name}
    desc, tags = doc if doc else ("", [])

    module = next((v for (t, v) in tags if t == "module"), None)
    if module:
        entry["module"] = module
    entry["permission"] = permission
    if desc:
        entry["description"] = desc

    payload = []
    for t, v in tags:
        if t != "payload":
            continue
        parsed = parse_typed(v)
        if not parsed:
            warn("%s: malformed @payload '%s'" % (name, v))
            continue
        field_name, field_type, _, field_desc = parsed
        field = {"name": field_name, "type": field_type}
        if field_desc:
            field["description"] = field_desc
        payload.append(field)
    if payload:
        entry["payload"] = payload

    return entry


def scan_functions(actions_dir):
    entries = []
    for fname in sorted(os.listdir(actions_dir)):
        if not fname.endswith(".c"):
            continue
        with open(os.path.join(actions_dir, fname), "r") as f:
            source = f.read()
        defines = extract_defines(source)
        docs = collect_docs(source, "plugin")
        for name, perm_raw in FUNC_RE.findall(source):
            if name not in docs:
                warn("function '%s' (%s) has no @plugin doc comment" % (name, fname))
            entries.append(build_function(name, resolve_permission(perm_raw, defines), docs.get(name)))
    return entries


def scan_events(header_path, actions_dir=None):
    sources = [(os.path.basename(header_path), header_path)]
    if actions_dir and os.path.isdir(actions_dir):
        for fname in sorted(os.listdir(actions_dir)):
            if fname.endswith(".c"):
                sources.append((fname, os.path.join(actions_dir, fname)))
    entries = []
    seen = set()
    for label, path in sources:
        with open(path, "r") as f:
            source = f.read()
        defines = extract_defines(source)
        docs = collect_docs(source, "event")
        for name, perm_raw in EVENT_RE.findall(source):
            if name in seen:
                continue
            seen.add(name)
            if name not in docs:
                warn("event '%s' (%s) has no @event doc comment" % (name, label))
            entries.append(build_event(name, resolve_permission(perm_raw, defines), docs.get(name)))
    return entries


# ---------------------------------------------------------------------------
# Generation
# ---------------------------------------------------------------------------


def load_config(project_dir):
    path = os.path.join(project_dir, "mtota.json")
    cfg = {}
    products = []
    if os.path.exists(path):
        with open(path, "r") as f:
            mtota = json.load(f)
        cfg = mtota.get("plugin-api", {})
        products = mtota.get("products", [])
    return {
        "product": cfg.get("product", "unknown"),
        "actions_dir": cfg.get("actions_dir", "src/actions"),
        "events_header": cfg.get("events_header", "include/actions/index.h"),
        "sku": products[0] if len(products) == 1 else None,
    }


def generate(project_dir, output_path, version=None):
    cfg = load_config(project_dir)
    actions_dir = os.path.join(project_dir, cfg["actions_dir"])
    functions = scan_functions(actions_dir)
    events = scan_events(os.path.join(project_dir, cfg["events_header"]), actions_dir)

    payload = {
        "$schema": SCHEMA_URL,
        "product": cfg["product"],
        "version": version or git_version(project_dir),
        "functions": functions,
        "events": events,
    }
    if cfg["sku"]:
        payload["sku"] = cfg["sku"]

    text = json.dumps(payload, indent=2) + "\n"

    os.makedirs(os.path.dirname(os.path.abspath(output_path)), exist_ok=True)
    with open(output_path, "w") as f:
        f.write(text)

    sha_path = re.sub(r"\.json$", "", output_path) + ".sha256"
    with open(sha_path, "w") as f:
        f.write(hashlib.sha256(text.encode("utf-8")).hexdigest() + "\n")

    log("Wrote %s (%d functions, %d events, version %s)" % (
        output_path, len(functions), len(events), payload["version"]))
    return payload


def _pio_post_action(source, target, env):
    project_dir = env.subst("$PROJECT_DIR")
    build_dir = env.subst("$BUILD_DIR")
    generate(project_dir, os.path.join(build_dir, "plugin-api.json"))


try:
    Import("env")  # noqa: F821 - injected by PlatformIO/SCons
    env.AddPostAction("$BUILD_DIR/${PROGNAME}.elf", _pio_post_action)  # noqa: F821
except NameError:
    if __name__ == "__main__":
        parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
        parser.add_argument("--project-dir", default=".")
        parser.add_argument("--out", default="plugin-api.json")
        parser.add_argument("--version", default=None)
        a = parser.parse_args()
        generate(a.project_dir, a.out, a.version)
