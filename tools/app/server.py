#!/usr/bin/env python3
"""Backport App (test phase) - the Backport Helper with a proper window.

Runs a small web server on this PC only (127.0.0.1) and opens the app in your browser. Everything it does
is the Backport Helper's own code (tools/backport_helper.py): the same steps, the same backups, the same
questions - asked in the window instead of the console. Close this console window to stop it.

Python 3.11+, standard library only (the kit itself needs numpy and Pillow, as before).
"""
import http.server
import json
import mimetypes
import os
import secrets
import socket
import sys
import threading
import time
import traceback
import urllib.parse
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

import backport_helper as h  # noqa: E402

STATIC = os.path.join(HERE, "static")
TOKEN = secrets.token_urlsafe(16)
SETTING_KEYS = ("kit", "client_data", "work", "server_dbc", "mysql", "db_host", "db_port", "db_user",
                "db_world", "armor_pieces")


# --------------------------------------------------------------------------------------------------
# jobs: one helper action at a time, run in a thread, its console talk routed to the window

class Job:
    def __init__(self, name):
        self.id = secrets.token_hex(4)
        self.name = name
        self.events = []          # {"t": line|title|prompt|answer|folder|end, ...}
        self.state = "running"    # running | waiting | done | failed
        self.prompt = None
        self._answer = None
        self._got = threading.Event()
        self.lock = threading.Lock()

    def emit(self, **e):
        with self.lock:
            self.events.append(e)

    # stand-ins for the helper's console functions
    def say(self, text=""):
        self.emit(t="line", text=str(text))

    def print(self, *args, **_kw):
        self.emit(t="line", text=" ".join(str(a) for a in args))

    def title(self, text):
        self.emit(t="title", text=str(text))

    def _wait(self, prompt):
        self._got.clear()
        self.prompt = prompt
        self.state = "waiting"
        self.emit(t="prompt", **prompt)
        self._got.wait()
        self.prompt = None
        self.state = "running"
        return self._answer

    def ask(self, question, default=None):
        a = self._wait({"kind": "ask", "question": question,
                        "default": "" if default is None else str(default)})
        a = (a or "").strip().strip('"')
        self.emit(t="answer", text=a or "(Enter)")
        return a if a else ("" if default is None else str(default))

    def yes(self, question, default=True):
        a = self._wait({"kind": "yes", "question": question, "default": bool(default)})
        self.emit(t="answer", text="Yes" if a else "No")
        return bool(a)

    def getpass(self, question=""):
        a = self._wait({"kind": "password", "question": question.rstrip(": ")})
        self.emit(t="answer", text="(password given)")
        return a or ""

    def open_folder(self, path):
        self.emit(t="folder", path=path, sheets=os.path.basename(os.path.normpath(path)).lower() == "sheets")

    def answer(self, value):
        if self.state == "waiting":
            self._answer = value
            self._got.set()
            return True
        return False


class _GetpassShim:
    def __init__(self, job):
        self.getpass = job.getpass


JOB = None
JOB_LOCK = threading.Lock()


def start_job(name, fn, *args):
    """Run fn(*args) with the helper's say/ask/yes/print routed to a Job. One at a time."""
    global JOB
    with JOB_LOCK:
        if JOB and JOB.state in ("running", "waiting"):
            return None
        job = JOB = Job(name)

    def run():
        mods = [h, h.converter, h.merger, h.rescuer]
        saved = {m: {k: m.__dict__.get(k, _MISSING) for k in ("say", "title", "ask", "yes", "print",
                                                               "open_folder", "getpass", "pause")}
                 for m in mods}
        try:
            for m in mods:
                m.print = job.print
            h.say, h.title, h.ask, h.yes = job.say, job.title, job.ask, job.yes
            h.open_folder, h.getpass, h.pause = job.open_folder, _GetpassShim(job), (lambda: None)
            result = fn(*args)
            job.state = "failed" if result is False else "done"
        except SystemExit:
            job.state = "done"
        except Exception as ex:  # keep the app alive and show what went wrong
            job.say(f"!! Something unexpected went wrong: {ex!r}")
            job.say(traceback.format_exc())
            job.state = "failed"
        finally:
            for m, kv in saved.items():
                for k, v in kv.items():
                    if v is _MISSING:
                        m.__dict__.pop(k, None)
                    else:
                        setattr(m, k, v)
            job.emit(t="end", state=job.state)

    threading.Thread(target=run, daemon=True).start()
    return job


_MISSING = object()


# --------------------------------------------------------------------------------------------------
# what the window shows

def settings():
    return h.load_settings()


def setup_problems(s):
    """What is missing before the app can work, in plain words (empty when ready)."""
    out = []
    if not s.get("kit") or not os.path.isfile(os.path.join(s["kit"], "wow-retail-backport", "scripts", "bp.py")):
        out.append("kit")
    data = s.get("client_data") or ""
    if not (os.path.isdir(data) and any(f.lower() == "common.mpq" for f in os.listdir(data))):
        out.append("client_data")
    if not s.get("work"):
        out.append("work")
    elif not os.path.isfile(os.path.join(s["work"], "bp.json")):
        out.append("project")
    return out


def state():
    s = settings()
    probs = setup_problems(s)
    led = h.ledger(s) if not probs else {}
    steps = []
    for step in ["scan", "select"] + h.STEPS:
        r = led.get(step)
        steps.append({"step": step, "words": h.STEP_WORDS.get(step, step),
                      "state": "done" if r and r.get("ok") else ("failed" if r else "todo"),
                      "at": (r or {}).get("at", "")})
    inst = {}
    if not probs:
        data = s["client_data"]
        name = h.patch_name(s)
        inst["kit_patch"] = name
        inst["kit_patch_installed"] = bool(name) and os.path.exists(os.path.join(data, name))
        inst["equip_plus_patches"] = [n for n in sorted(os.listdir(data))
                                      if os.path.isfile(os.path.join(data, n, h.converter.MARKER))]
        inst["equip_plus_dll"] = os.path.isfile(os.path.join(h.client_root(s), "Extensions", "wxl-equip-plus",
                                                             "wxl-equip-plus.dll"))
        inst["extended_dbc"] = os.path.isfile(os.path.join(h.client_root(s), "Extensions", "wxl-extended-dbc",
                                                           "wxl-extended-dbc.dll"))
        inst["can_undo"] = bool(s.get("last_backup")) and os.path.isfile(
            os.path.join(s.get("last_backup") or "", "install.json"))
        inst["left_out"] = len(h.excluded(s))
        inst["brought_back"] = len(h._json(os.path.join(s["work"], "rescued.json"), {}))
    return {"settings": {k: s.get(k, "") for k in SETTING_KEYS}, "problems": probs, "steps": steps,
            "installed": inst, "job": job_summary()}


def job_summary():
    j = JOB
    if not j:
        return None
    return {"id": j.id, "name": j.name, "state": j.state, "prompt": j.prompt, "events": len(j.events)}


def sheets_for(work):
    d = os.path.join(work, "sheets")
    out = {}
    if os.path.isdir(d):
        for f in sorted(os.listdir(d)):
            if f.lower().endswith(".png"):
                out.setdefault(f.split("_", 1)[0], []).append(f)
    return out


def project():
    s = settings()
    if setup_problems(s):
        return {"entries": []}
    sel = h._json(os.path.join(s["work"], "selection.json"), {}) or {}
    displays = sel.get("displays") or {}
    ids = (h._json(os.path.join(s["work"], "ids.json"), {}) or {}).get("item") or {}
    ex, rescued = h.excluded(s), h._json(os.path.join(s["work"], "rescued.json"), {})
    sheets = sheets_for(s["work"])
    entries = []
    for i, (label, sid, ds) in enumerate(h.project_entries(s)):
        pieces = []
        for d in ds:
            e = displays.get(d, {})
            items = e.get("items") or []
            pieces.append({"display": d, "slot": e.get("slot", "?"),
                           "name": next((x.get("name") for x in items if x.get("name")), f"look {d}"),
                           "retail_item": next((x.get("item") for x in items if x.get("item")), None),
                           "quality": next((x.get("quality") for x in items if x.get("quality") is not None), 1),
                           "item_id": ids.get(d), "left_out": d in ex, "brought_back": d in rescued,
                           "sheets": sheets.get(d, [])})
        if sid:
            st = (sel.get("sets") or {}).get(sid, {})
            name = st.get("name") or f"Set {sid}"
            if st.get("label"):
                name += f" ({st['label']})"
        else:
            name = pieces[0]["name"] if pieces else label
        entries.append({"index": i, "kind": "set" if sid else "item", "set": sid, "name": name, "pieces": pieces})
    return {"entries": entries}


def save_settings_from(body):
    s = settings()
    for k in SETTING_KEYS:
        if k in body:
            v = body[k]
            s[k] = bool(v) if k == "armor_pieces" else str(v).strip().strip('"')
    for k in ("kit", "client_data", "work", "server_dbc"):
        if s.get(k):
            s[k] = os.path.abspath(s[k])
    h.save_settings(s)
    return s


def check_settings(s):
    """Field -> problem, for the settings form."""
    bad = {}
    if not os.path.isfile(os.path.join(s.get("kit", ""), "wow-retail-backport", "scripts", "bp.py")):
        bad["kit"] = "No kit here (no wow-retail-backport\\scripts\\bp.py)."
    data = s.get("client_data", "")
    if not (os.path.isdir(data) and any(f.lower() == "common.mpq" for f in os.listdir(data))):
        bad["client_data"] = "No common.MPQ here, so it is not a 3.3.5a Data folder."
    if not s.get("work"):
        bad["work"] = "Choose a project folder."
    if s.get("server_dbc") and not os.path.isfile(os.path.join(s["server_dbc"], "Item.dbc")):
        bad["server_dbc"] = "No Item.dbc here."
    return bad


# actions the window can start (each is the helper's own function)

def _init_project(s):
    h.say("Creating the project (this checks wago.tools for the newest retail build) ...")
    code, _ = h.run_kit(s, "init", "--work", s["work"], "--client", s["client_data"])
    if code != 0:
        h.say("!! Creating the project failed - see above.")
        return False
    h.say("Project created.")
    return True


def _download_kit(s):
    import shutil
    import subprocess
    kit = s.get("kit") or (r"C:\Tools\wow-retail-backport" if os.name == "nt"
                           else os.path.expanduser("~/wow-retail-backport"))
    if os.path.exists(kit) and os.listdir(kit):
        h.say(f"!! {kit} already exists and is not empty. Choose an empty folder.")
        return False
    if not shutil.which("git"):
        h.say("!! git is not installed. Install Git for Windows, or download the kit yourself.")
        return False
    h.say(f"Downloading the kit into {kit} ...")
    p = subprocess.run(["git", "clone", h.KIT_REPO, kit], capture_output=True, text=True)
    h.say((p.stdout + p.stderr).strip())
    if p.returncode:
        return False
    s["kit"] = os.path.abspath(kit)
    h.save_settings(s)
    h.say("Done.")
    return True


def _add(s, chosen, family):
    if not h.passed(s, "scan") and not h.ensure_scan(s):
        return False
    ok = h.add_to_project(s, chosen, family)
    if ok:
        h.say("Added. Build when you have added everything you want.")
    return ok


def _remove(s, indexes):
    entries = h.project_entries(s)
    chosen = [entries[i] for i in indexes if 0 <= i < len(entries)]
    if not chosen:
        h.say("Nothing chosen - nothing removed.")
        return False
    return h.remove_entries(s, chosen)


def _update_equip_plus(s):
    h.check_equip_plus(s, update=True)


ACTIONS = {
    "build": ("Build", h.build),
    "install": ("Install", h.install),
    "undo": ("Undo last install", h.undo),
    "bring_back": ("Bring back left-out looks", h.bring_back),
    "clean_db": ("Clean up the database", h.clean_database),
    "update_equip_plus": ("Update Equip Plus", _update_equip_plus),
    "scan": ("Read your client", h.ensure_scan),
    "init": ("Create the project", _init_project),
    "download_kit": ("Download the kit", _download_kit),
}


# --------------------------------------------------------------------------------------------------
# HTTP

class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "BackportApp"

    def log_message(self, *_a):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        data = body if isinstance(body, bytes) else json.dumps(body).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def _authorised(self):
        return secrets.compare_digest(self.headers.get("X-Token", ""), TOKEN)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            return json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return {}

    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        if u.path.startswith("/api/"):
            if not self._authorised():
                return self._send(403, {"error": "not allowed"})
            return self._api_get(u.path[5:], q)
        if u.path.startswith("/sheet/"):
            if q.get("t", [""])[0] != TOKEN:
                return self._send(403, b"", "text/plain")
            name = os.path.basename(urllib.parse.unquote(u.path[7:]))
            p = os.path.join(settings().get("work", ""), "sheets", name)
            if os.path.isfile(p) and name.lower().endswith(".png"):
                with open(p, "rb") as f:
                    return self._send(200, f.read(), "image/png")
            return self._send(404, b"", "text/plain")
        path = "index.html" if u.path in ("/", "") else u.path.lstrip("/")
        full = os.path.normpath(os.path.join(STATIC, path))
        if not full.startswith(STATIC) or not os.path.isfile(full):
            return self._send(404, b"not found", "text/plain")
        with open(full, "rb") as f:
            ctype = mimetypes.guess_type(full)[0] or "application/octet-stream"
            if ctype.startswith("text/") or ctype.endswith("javascript"):
                ctype += "; charset=utf-8"
            return self._send(200, f.read(), ctype)

    def _api_get(self, what, q):
        if what == "state":
            return self._send(200, state())
        if what == "project":
            return self._send(200, project())
        if what == "job":
            j = JOB
            if not j:
                return self._send(200, {"job": None})
            since = int(q.get("since", ["0"])[0] or 0)
            with j.lock:
                ev = j.events[since:]
            return self._send(200, {"job": job_summary(), "events": ev, "next": since + len(ev)})
        if what == "sheets":
            s = settings()
            return self._send(200, {"sheets": sheets_for(s.get("work", "")) if s.get("work") else {}})
        if what == "search":
            s = settings()
            if setup_problems(s):
                return self._send(400, {"error": "Finish the settings first."})
            if not h.passed(s, "scan"):
                return self._send(200, {"need_scan": True, "results": []})
            text = (q.get("q", [""])[0] or "").strip()
            if len(text) < 2:
                return self._send(200, {"results": []})
            try:
                return self._send(200, {"results": h.search_retail(s, text)})
            except RuntimeError as ex:
                return self._send(200, {"error": str(ex)[-2000:], "results": []})
        return self._send(404, {"error": "unknown"})

    def do_POST(self):
        u = urllib.parse.urlparse(self.path)
        if not u.path.startswith("/api/") or not self._authorised():
            return self._send(403, {"error": "not allowed"})
        what, body = u.path[5:], self._body()
        s = settings()
        if what == "answer":
            j = JOB
            return self._send(200, {"ok": bool(j and j.answer(body.get("value")))})
        if what == "settings":
            s = save_settings_from(body)
            return self._send(200, {"problems": check_settings(s), "state": state()})
        if what == "open":
            target = {"project": s.get("work"), "sheets": os.path.join(s.get("work", ""), "sheets"),
                      "backups": os.path.join(s.get("work", ""), "backups"), "data": s.get("client_data"),
                      }.get(body.get("what"))
            if target and os.path.isdir(target):
                h.open_folder(target)
                return self._send(200, {"ok": True})
            return self._send(200, {"ok": False})
        if what.startswith("run/"):
            name = what[4:]
            if name == "add":
                chosen = [{"kind": c["kind"], "id": str(int(c["id"]))} for c in body.get("chosen", [])
                          if c.get("kind") in ("set", "item")]
                job = start_job("Add to project", _add, s, chosen, bool(body.get("family")))
            elif name == "remove":
                job = start_job("Remove from project", _remove, s, [int(i) for i in body.get("indexes", [])])
            elif name in ACTIONS:
                label, fn = ACTIONS[name]
                job = start_job(label, fn, s)
            else:
                return self._send(404, {"error": "unknown action"})
            if not job:
                return self._send(409, {"error": "Something is already running - let it finish first."})
            return self._send(200, {"job": job_summary()})
        return self._send(404, {"error": "unknown"})


def free_port(preferred=8765):
    for port in [preferred] + list(range(preferred + 1, preferred + 40)):
        with socket.socket() as sk:
            try:
                sk.bind(("127.0.0.1", port))
                return port
            except OSError:
                continue
    return 0


def main():
    print("Backport App (test phase)")
    if not h.find_python_ok():
        input("\nPress Enter to close...")
        return 1
    port = free_port()
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
    url = f"http://127.0.0.1:{srv.server_address[1]}/#t={TOKEN}"
    print(f"Open in your browser: {url}")
    print("Keep this window open while you use the app. Close it to stop.")
    if "--no-browser" not in sys.argv:
        threading.Timer(0.6, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
