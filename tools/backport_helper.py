#!/usr/bin/env python3
"""Backport Helper - bring retail armour sets into your 3.3.5a client and server, one question at a time.

Double-click "Backport Helper.bat" (or run `python backport_helper.py`). The helper drives the
wow-retail-backport kit and Equip Plus's tools for you:

  1. Add something  - search retail by name, pick what you want, it is added to your project.
  2. Build          - runs every kit step in order, stopping with a plain explanation if one fails.
  3. Install        - backs up what it replaces, then updates patch-X (the sets), patch-W (their 3D
                      pieces for Equip Plus), clears the client cache, merges the server tables and
                      runs the item SQL.
  4. Undo           - puts back everything the last install replaced.

It keeps ONE kit project for everything you backport (the kit's own rule), never re-scans while its own
patch is installed (that would make the kit trip over its own rows), and never writes to your server's
folder or database without asking first.

Python 3.9+, standard library only. Settings live in %USERPROFILE%\\.wxl-equip-plus\\helper.json.
"""
import datetime
import getpass
import json
import os
import re
import shutil
import subprocess
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import backport_to_equipplus as converter  # noqa: E402
import merge_server_dbc as merger  # noqa: E402
import rescue_left_out as rescuer  # noqa: E402
from mpqread import MpqReader  # noqa: E402

SETTINGS_DIR = os.path.join(os.path.expanduser("~"), ".wxl-equip-plus")
SETTINGS_FILE = os.path.join(SETTINGS_DIR, "helper.json")
KIT_REPO = "https://github.com/Kirazul/wow-retail-backport.git"
EQUIP_PLUS_LATEST = "https://api.github.com/repos/TheDaveCalaz/wxl-equip-plus/releases/latest"
STEPS = ["fetch", "newtech", "measure", "build", "tables", "audit", "prove-gates", "pack", "sheet"]
STEP_WORDS = {
    "scan": "Reading your client (races, bodies, tables)",
    "select": "Working out every file the selection needs",
    "fetch": "Downloading the retail files",
    "newtech": "Checking for retail technology the kit does not know yet",
    "measure": "Measuring how helms sit on your client's bodies",
    "build": "Converting models and textures",
    "tables": "Writing the item tables and the server SQL",
    "audit": "Checking everything for crashes, holes and floating parts",
    "prove-gates": "Proving the checks themselves work",
    "pack": "Packing patch-X",
    "sheet": "Drawing the preview pictures",
}
STEP_HELP = {
    "fetch": "Some retail files did not download. This is usually wago.tools being busy - choose Build "
             "again in a few minutes; the kit carries on where it stopped.",
    "newtech": "Retail uses something the kit has not learned to convert yet. Building now would produce "
               "broken items, so the helper stopped. Remove that selection, or ask for help with the "
               "kit's report in reports\\newtech.json.",
    "measure": "The kit could not measure your client's bodies. If you changed body models recently, "
               "the log below says which one failed.",
    "build": "Some models or textures failed to convert. The log below groups the failures by cause.",
    "audit": "The final check found a real problem, so nothing was packed. Nothing in your game was "
             "touched. The log below names each problem.",
    "prove-gates": "The kit's own self-test failed, so its checks cannot be trusted. Nothing was packed.",
    "pack": "Packing failed. Check there is free disk space, then choose Build again.",
}


# --------------------------------------------------------------------------------------------------
# small UI helpers

def say(text=""):
    print(text)


def title(text):
    say()
    say("=" * 72)
    say("  " + text)
    say("=" * 72)


def ask(question, default=None):
    hint = f" [{default}]" if default not in (None, "") else ""
    while True:
        try:
            answer = input(f"{question}{hint}: ").strip().strip('"')
        except EOFError:
            raise SystemExit(0)
        if answer:
            return answer
        if default is not None:
            return str(default)


def yes(question, default=True):
    d = "Y/n" if default else "y/N"
    while True:
        a = ask(f"{question} ({d})", "").lower()
        if not a:
            return default
        if a in ("y", "yes"):
            return True
        if a in ("n", "no"):
            return False


def pause():
    try:
        input("\nPress Enter to continue...")
    except EOFError:
        pass


def open_folder(path):
    try:
        if os.name == "nt":
            os.startfile(path)  # noqa: S606 - opening the user's own folder in Explorer
        elif sys.platform == "darwin":
            subprocess.Popen(["open", path])
        else:
            subprocess.Popen(["xdg-open", path])
    except OSError:
        say(f"(Open this folder yourself: {path})")


# --------------------------------------------------------------------------------------------------
# settings

def load_settings():
    try:
        with open(SETTINGS_FILE, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_settings(s):
    os.makedirs(SETTINGS_DIR, exist_ok=True)
    tmp = SETTINGS_FILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(s, f, indent=2)
    os.replace(tmp, SETTINGS_FILE)


def bp_py(s):
    return os.path.join(s["kit"], "wow-retail-backport", "scripts", "bp.py")


def client_root(s):
    return os.path.dirname(os.path.normpath(s["client_data"]))


def project_cfg(s):
    try:
        with open(os.path.join(s["work"], "bp.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def ledger(s):
    try:
        with open(os.path.join(s["work"], "reports", "steps.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def passed(s, step):
    return bool(ledger(s).get(step, {}).get("ok"))


def patch_name(s):
    return project_cfg(s).get("patch_name") or ""


# --------------------------------------------------------------------------------------------------
# running the kit

def run_kit(s, *args, show=True):
    """Run one bp.py command; returns (exit code, output text)."""
    cmd = [sys.executable, bp_py(s), *args]
    proc = subprocess.Popen(cmd, cwd=os.path.dirname(bp_py(s)), stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace")
    lines = []
    for line in proc.stdout:
        lines.append(line)
        if show:
            print("    " + line.rstrip())
    return proc.wait(), "".join(lines)


def run_step(s, step, *extra):
    say()
    say(f"-> {STEP_WORDS.get(step, step)} ...")
    code, out = run_kit(s, step, "--work", s["work"], *extra)
    if passed(s, step) and code == 0:
        say(f"   done: {step}")
        return True
    say()
    say(f"!! The step '{step}' did not pass.")
    if step in STEP_HELP:
        say("   " + STEP_HELP[step])
    say(f"   Full log: {os.path.join(s['work'], 'logs', step + '.log')}")
    return False


# --------------------------------------------------------------------------------------------------
# first-run setup

def find_python_ok():
    if sys.version_info < (3, 11):
        say(f"The backport kit needs Python 3.11 or newer; this is {sys.version.split()[0]}.")
        say("Install a newer Python from python.org, then run the helper again.")
        return False
    for mod in ("numpy", "PIL"):
        try:
            __import__(mod)
        except ImportError:
            say(f"The kit needs the Python package '{mod}'.")
            if yes("Install numpy and Pillow now?"):
                subprocess.call([sys.executable, "-m", "pip", "install", "numpy", "pillow"])
            else:
                return False
    return True


def setup(s):
    title("Setup - the helper needs to know where a few things are")
    say("Press Enter to accept the suggestion in [brackets].")

    # the kit
    say()
    default_kit = s.get("kit") or (r"C:\Tools\wow-retail-backport" if os.name == "nt"
                                   else os.path.expanduser("~/wow-retail-backport"))
    while True:
        kit = ask("Where is the backport kit (the folder you cloned it into)?", default_kit)
        if os.path.isfile(os.path.join(kit, "wow-retail-backport", "scripts", "bp.py")):
            break
        if not os.path.exists(kit) and yes(f"There is no kit at {kit}. Download it there now?"):
            if shutil.which("git") and subprocess.call(["git", "clone", KIT_REPO, kit]) == 0:
                break
            say("Could not download it (is git installed?). Clone it yourself, then try again.")
            continue
        say("That folder does not hold the kit (no wow-retail-backport\\scripts\\bp.py). Try again.")
    s["kit"] = os.path.abspath(kit)

    # the client
    say()
    while True:
        data = ask("Where is your WoW client's Data folder (it holds common.MPQ)?",
                   s.get("client_data") or r"C:\WoW335\Data")
        if os.path.isdir(data) and any(f.lower() == "common.mpq" for f in os.listdir(data)):
            break
        say("That folder has no common.MPQ, so it is not a 3.3.5a Data folder. Try again.")
    s["client_data"] = os.path.abspath(data)

    # the project
    say()
    say("The project folder is where the kit keeps everything you backport. Use ONE for everything.")
    work = ask("Project folder (an existing one, or a new name)?", s.get("work") or r"C:\Backport\MyProject")
    s["work"] = os.path.abspath(work)
    save_settings(s)
    if not os.path.isfile(os.path.join(s["work"], "bp.json")):
        say("That is a new project. Creating it (this checks wago.tools for the newest retail build) ...")
        code, _ = run_kit(s, "init", "--work", s["work"], "--client", s["client_data"])
        if code != 0:
            say("!! Creating the project failed - see above.")
            return False

    # the server (optional)
    say()
    if yes("Do you run the server on this PC (so the helper can update its dbc folder and database)?"):
        while True:
            dbc = ask("Where is the server's dbc folder (it holds Item.dbc)?", s.get("server_dbc") or "")
            if os.path.isfile(os.path.join(dbc, "Item.dbc")):
                break
            say("No Item.dbc there. Try again.")
        s["server_dbc"] = os.path.abspath(dbc)
        say()
        say("For the item SQL the helper uses the mysql command line. Your password is never stored.")
        s["mysql"] = ask("Path to mysql.exe (or just 'mysql' if it is on PATH)",
                         s.get("mysql") or (shutil.which("mysql") or "mysql"))
        s["db_host"] = ask("Database host", s.get("db_host") or "127.0.0.1")
        s["db_port"] = ask("Database port", s.get("db_port") or "3306")
        s["db_user"] = ask("Database user", s.get("db_user") or "acore")
        s["db_world"] = ask("World database name", s.get("db_world") or "acore_world")
    else:
        s["server_dbc"] = ""
    save_settings(s)
    say()
    say("Setup saved. You can change it any time from the menu.")
    return True


# --------------------------------------------------------------------------------------------------
# 1. add something

FIND_SET = re.compile(r"^\s*set\s+(\d+)\s+exp\s+(\S+)\s+classmask\s+(\S+)\s+parent\s+(\S+)\s+(.*)$")
FIND_ITEM = re.compile(r"^\s*item\s+(\d+)\s+exp\s+(\S+)\s+q(\S+)\s+inv\s+(\S+)\s+(.*)$")


def search_retail(s, text):
    """The kit's search, as a list of {kind, id, name, label, ...}. Raises RuntimeError on failure."""
    code, out = run_kit(s, "find", "--work", s["work"], text, "--limit", "60", show=False)
    if code != 0:
        raise RuntimeError(out.strip() or "the search failed")
    found = []
    for line in out.splitlines():
        m = FIND_SET.match(line)
        if m:
            found.append({"kind": "set", "id": m.group(1), "name": m.group(5).strip(), "exp": m.group(2),
                          "classmask": m.group(3), "parent": m.group(4),
                          "label": f"SET   {m.group(5)}  (set {m.group(1)}, expansion {m.group(2)}"
                                   f"{', part of family ' + m.group(4) if m.group(4) != '0' else ''})"})
            continue
        m = FIND_ITEM.match(line)
        if m:
            found.append({"kind": "item", "id": m.group(1), "name": m.group(5).strip(), "exp": m.group(2),
                          "quality": m.group(3), "inv": m.group(4),
                          "label": f"ITEM  {m.group(5)}  (item {m.group(1)}, quality {m.group(3)})"})
    return found


def add_to_project(s, chosen, family=False):
    """Add [{kind, id}] to the kit's selection. True when the kit took them."""
    args = ["select", "--work", s["work"], "--add"]
    for c in chosen:
        args += [f"--{c['kind']}", str(c["id"])]
    if family and any(c["kind"] == "set" for c in chosen):
        args.append("--family")
    say()
    say("-> Adding to your project ...")
    code, out = run_kit(s, *args)
    if code == 0 and passed(s, "select"):
        n = apply_exclusions(s)
        if n:
            say(f"   ({n} look(s) you left out earlier stay left out)")
        return True
    say("!! Adding failed - see above.")
    return False


def add_something(s):
    title("Add something to your project")
    if not passed(s, "scan") and not ensure_scan(s):
        return
    text = ask("Search retail for (part of a set or item name, e.g. Lion's Heritage)")
    say("Searching ... (the first search downloads retail's tables, which can take a minute)")
    try:
        found = search_retail(s, text)
    except RuntimeError as ex:
        say(str(ex))
        say("!! The search failed - see above.")
        return
    if not found:
        say("Nothing matched. Try fewer words, or the set's name as Wowhead spells it.")
        return
    say()
    for i, f in enumerate(found, 1):
        say(f"  {i:3}. {f['label']}")
    say()
    say("Sets usually come as one entry per colour. Pick every one you want.")
    picks = ask("Numbers to add, separated by commas (e.g. 1,2,3), or Enter to cancel", "")
    if not picks:
        return
    chosen = []
    for part in re.split(r"[,\s]+", picks):
        if part.isdigit() and 1 <= int(part) <= len(found):
            chosen.append(found[int(part) - 1])
    if not chosen:
        say("No valid numbers - nothing added.")
        return
    family = any(c["kind"] == "set" for c in chosen) and \
        yes("Also add every other colour of these sets that retail links to them?", False)
    if add_to_project(s, chosen, family):
        say()
        say("Added. Choose 'Build' from the menu when you have added everything you want.")


REBUILD_AFTER_REMOVE = ["tables", "audit", "prove-gates", "pack", "sheet"]


def rescued_left(work):
    return bool(_json(os.path.join(work, "rescued.json"), {}))


def remove_something(s):
    title("Remove something")
    say("  1. Take sets or items out of your project")
    say("  2. Clean up the database   (items your server has but your game does not - they show up")
    say("                              invisible, with no icon)")
    c = ask("Choose a number, or Enter to go back", "")
    if c == "1":
        remove_from_project(s)
    elif c == "2":
        clean_database(s)


def project_entries(s):
    """Everything in the project: [(label, set id or None, [display ids])], sets first."""
    sel = _json(os.path.join(s["work"], "selection.json"), None)
    if not sel or not sel.get("displays"):
        return []
    sets = sel.get("sets") or {}
    displays = sel["displays"]
    entries = []
    for sid, st in sorted(sets.items(), key=lambda kv: (kv[1].get("name", ""), int(kv[0]))):
        ds = sorted({str(d) for v in (st.get("slots") or {}).values() for d in v})
        entries.append((f"SET   {st.get('name', '?')}  (set {sid}, {len(ds)} piece(s))", sid, ds))
    in_sets = {str(d) for st in sets.values() for v in (st.get("slots") or {}).values() for d in v}
    for d, e in sorted(displays.items(), key=lambda kv: int(kv[0])):
        if d in in_sets:
            continue
        name = next((i.get("name") for i in e.get("items", []) if i.get("name")), "") or f"look {d}"
        entries.append((f"ITEM  {name}  ({e.get('slot', '?')}, look {d})", None, [d]))
    return entries


def remove_from_project(s):
    """Drop sets or single items from the project: out of the kit's selection and plan, out of the looks
    brought back, and out of the database at the next Install. Nothing downloaded is deleted, ids stay
    reserved, and anything removed can be added again later."""
    title("Remove something from your project")
    entries = project_entries(s)
    if not entries:
        say("Nothing has been added yet.")
        return
    say("Everything in this project:")
    say()
    for i, (label, _, _) in enumerate(entries, 1):
        say(f"  {i:3}. {label}")
    say()
    picks = ask("Numbers to remove, separated by commas, or Enter to cancel", "")
    if not picks:
        return
    chosen = [entries[int(x) - 1] for x in re.split(r"[,\s]+", picks)
              if x.isdigit() and 1 <= int(x) <= len(entries)]
    if not chosen:
        say("No valid numbers - nothing removed.")
        return
    remove_entries(s, chosen)


def remove_entries(s, chosen, confirm=True):
    """Take project_entries() rows out of the project (see remove_from_project)."""
    work = s["work"]
    sel_path = os.path.join(work, "selection.json")
    sel = _json(sel_path, None)
    if not sel or not sel.get("displays"):
        return False
    sets = sel.get("sets") or {}
    displays = sel["displays"]
    drop_sets = {sid for _, sid, _ in chosen if sid}
    # a look goes only when nothing that stays still uses it
    keep_sets = {sid: st for sid, st in sets.items() if sid not in drop_sets}
    still_used = {str(d) for st in keep_sets.values() for v in (st.get("slots") or {}).values() for d in v}
    drop = {d for _, _, ds in chosen for d in ds if d not in still_used}
    say()
    for label, _, _ in chosen:
        say("   remove " + label)
    say(f"   ({len(drop)} look(s) in all)")
    say("Characters already holding these items will see them vanish from the game once you Install.")
    if confirm and not yes("Remove them?"):
        return False

    # the kit's selection and plan
    for sid in drop_sets:
        sets.pop(sid, None)
    for d in drop:
        displays.pop(d, None)
    for st in sets.values():
        st["slots"] = {k: [x for x in v if str(x) not in drop] for k, v in (st.get("slots") or {}).items()}
    _save_json(sel_path, sel)
    plan_path = os.path.join(work, "build", "plan.json")
    plan = _json(plan_path, None)
    if plan:
        for d in drop:
            plan.pop(d, None)
        _save_json(plan_path, plan)

    # looks left out or brought back
    ex = excluded(s)
    if any(d in ex for d in drop):
        _save_json(os.path.join(work, "helper-excluded.json"), {k: v for k, v in ex.items() if k not in drop})
    rescued_path = os.path.join(work, "rescued.json")
    rescued = _json(rescued_path, {})
    gone_rescued = {d: rescued[d] for d in drop if d in rescued}
    if gone_rescued:
        left = {k: v for k, v in rescued.items() if k not in gone_rescued}
        _save_json(rescued_path, left)
        if not left:   # nothing brought back any more: drop what option 8 staged for the patch
            shutil.rmtree(os.path.join(work, "out", "equip-plus-rescue"), ignore_errors=True)

    # the database: delete the removed items at the next Install (ids stay reserved in ids.json)
    item_ids = (_json(os.path.join(work, "ids.json"), {}).get("item") or {})
    gone_items = {int(item_ids[d]) for d in drop if d in item_ids} | \
        {int(i) for v in gone_rescued.values() for i in v.get("items", [])}
    if gone_items:
        rm_path = os.path.join(work, "out", "sql", "item_template_removed.sql")
        os.makedirs(os.path.dirname(rm_path), exist_ok=True)
        old = set()
        if os.path.isfile(rm_path):
            with open(rm_path, encoding="utf-8") as f:
                body = f.read()
            if "IN (" in body:
                old = {int(x) for x in re.findall(r"\d+", body.split("IN (", 1)[1])}
        ids = sorted(old | gone_items)
        with open(rm_path, "w", encoding="utf-8") as f:
            f.write("-- items removed with the Backport Helper; run before the kit's item_template.sql\n")
            f.write("DELETE FROM `item_template` WHERE `entry` IN (%s);\n" % ",".join(map(str, ids)))

    # what has to run again
    steps_path = os.path.join(work, "reports", "steps.json")
    led = _json(steps_path, {})
    for step in REBUILD_AFTER_REMOVE:
        if step in led:
            led[step]["ok"] = False
    if led:
        _save_json(steps_path, led)
    say()
    say(f"Removed {len(chosen)} entr{'y' if len(chosen) == 1 else 'ies'} ({len(drop)} look(s)).")
    if gone_rescued and rescued_left(work):
        say("Some of them were looks brought back with option 8: choose 8 again so the rest are re-made.")
    say("Next: choose 'Build', then 'Install'. Nothing downloaded was deleted, so adding them back is quick.")
    return True


# --------------------------------------------------------------------------------------------------
# clean up the database
#
# A backport item the server has but the game's Item.dbc does not shows in game with its name and nothing
# else: no model, no icon. That happens when a set's rows reached the database but its patch never reached
# the game (a build that failed, SQL run by hand, a set taken out before option 9 existed, a different
# project). This finds them by comparing the database with every Item table the game actually loads, and
# deletes only those, after a backup.

ITEM_TABLES = {"DBFilesClient\\Item.dbc": "items", "DBFilesClient\\Item.dbc1-equipplus": "items",
               "DBFilesClient\\ItemDisplayInfo.dbc": "looks",
               "DBFilesClient\\ItemDisplayInfo.dbc1-equipplus": "looks"}


def _dbc_ids(data):
    """First column of every row of a WDBC table."""
    import struct
    if len(data) < 20 or data[:4] != b"WDBC":
        return set()
    n, _, size, _ = struct.unpack_from("<4I", data, 4)
    return {struct.unpack_from("<I", data, 20 + i * size)[0] for i in range(n) if 20 + i * size + 4 <= len(data)}


def game_item_ids(data_dir):
    """Every item id and look (display) id in any Item / ItemDisplayInfo table the client can load
    (patches, locale patches, folder patches). Returns ({"items", "looks"}, Item tables read, archives
    that could not be read)."""
    ids, tables, unread = {"items": set(), "looks": set()}, 0, []
    roots = [data_dir] + [os.path.join(data_dir, d) for d in os.listdir(data_dir)
                          if os.path.isdir(os.path.join(data_dir, d)) and not d.lower().endswith(".mpq")]
    for root in roots:
        for name in sorted(os.listdir(root)):
            if not name.lower().endswith(".mpq"):
                continue
            path = os.path.join(root, name)
            try:
                if os.path.isdir(path):
                    for rel, kind in ITEM_TABLES.items():
                        f = os.path.join(path, *rel.split("\\"))
                        if os.path.isfile(f):
                            with open(f, "rb") as fh:
                                ids[kind] |= _dbc_ids(fh.read())
                            tables += kind == "items"
                else:
                    m = MpqReader(path)
                    try:
                        for rel, kind in ITEM_TABLES.items():
                            if m.has(rel):
                                ids[kind] |= _dbc_ids(m.read(rel))
                                tables += kind == "items"
                    finally:
                        m.close()
            except Exception:   # an archive this reader cannot open; stock archives hold no backport items
                unread.append(name)
    return ids, tables, unread


def _mysql_base(s, exe):
    return [exe, "-h", s.get("db_host", "127.0.0.1"), "-P", str(s.get("db_port", "3306")),
            "-u", s.get("db_user", "acore")]


def clean_database(s):
    title("Clean up the database")
    work = s["work"]
    if not s.get("db_user"):
        say("No database is set up. Add your database details under Settings first.")
        return
    mysql = s.get("mysql") or "mysql"
    mysql = mysql if os.path.isfile(mysql) else shutil.which(mysql)
    if not mysql:
        say(f"!! Cannot find mysql at '{s.get('mysql') or 'mysql'}'. Change it under Settings.")
        return
    dump = os.path.join(os.path.dirname(mysql), "mysqldump" + (".exe" if mysql.lower().endswith(".exe") else ""))
    dump = dump if os.path.isfile(dump) else shutil.which("mysqldump")
    if not dump:
        say("!! Cannot find mysqldump next to mysql, so the rows could not be backed up first.")
        say("   Nothing was changed.")
        return

    say("Reading the item tables your game loads ...")
    game, tables, unread = game_item_ids(s["client_data"])
    if not tables:
        say("!! Could not find any Item table in your game's Data folder, so there is nothing safe to compare")
        say("   against. Nothing was changed.")
        return
    say(f"   {tables} item table(s), {len(game['items'])} item(s), {len(game['looks'])} look(s)")
    if unread:
        say("   (skipped, could not open: " + ", ".join(unread) + ")")

    # backport items: the kit's range and every id this project ever handed out, plus Equip Plus's own
    start = int(_json(os.path.join(work, "bp.json"), {}).get("item_id_start") or 0) or 900000
    known = set()
    for v in (_json(os.path.join(work, "ids.json"), {}).get("item") or {}).values():
        known |= {int(x) for x in (v if isinstance(v, list) else [v])}
    for v in _json(os.path.join(work, "rescued.json"), {}).values():
        known |= {int(x) for x in v.get("items", [])}
    known.add(start)
    where = f"entry >= {min(start, rescuer.ITEM_START)} OR entry IN ({','.join(map(str, sorted(known)))})"

    say()
    pw = getpass.getpass(f"Password for {s.get('db_user')} (not shown, not stored): ")
    env = dict(os.environ, MYSQL_PWD=pw)
    db = s.get("db_world", "acore_world")
    r = subprocess.run(_mysql_base(s, mysql) + ["-N", "-B", db, "-e",
                       f"SELECT entry, displayid, name FROM item_template WHERE {where} ORDER BY entry"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        say("!! Could not read the database:")
        say("   " + (r.stderr or r.stdout).strip().replace("\n", "\n   "))
        return
    rows = []
    for line in r.stdout.splitlines():
        entry, display, name = (line.split("\t", 2) + ["", ""])[:3]
        if not (entry.isdigit() and display.isdigit()):
            continue
        if int(entry) not in game["items"]:
            rows.append((int(entry), name, "not in the game's item table"))
        elif int(display) not in game["looks"]:
            rows.append((int(entry), name, f"its look {display} is not in the game"))
    if not rows:
        say("Nothing to clean up: every backport item in your database is in your game.")
        return

    say()
    say(f"{len(rows)} item(s) in your database that your game cannot show:")
    for entry, name, why in rows[:60]:
        say(f"   {entry}  {name}  ({why})")
    if len(rows) > 60:
        say(f"   ... and {len(rows) - 60} more")
    say()
    say("Characters holding these items will lose them. If any of them are sets you still want, choose")
    say("'Build' and 'Install' instead - that puts the game's half back.")
    if not yes(f"Back up and delete these {len(rows)} item(s) from {db}.item_template?"):
        return

    ids = ",".join(str(e) for e, _, _ in rows)
    backup = os.path.join(work, "backups", now())
    os.makedirs(backup, exist_ok=True)
    bfile = os.path.join(backup, "item_template_cleaned.sql")
    with open(bfile, "w", encoding="utf-8") as f:
        r = subprocess.run([dump] + _mysql_base(s, mysql)[1:] + ["--no-create-info", "--skip-triggers",
                           f"--where=entry IN ({ids})", db, "item_template"],
                           env=env, stdout=f, stderr=subprocess.PIPE, text=True)
    if r.returncode != 0 or os.path.getsize(bfile) == 0:
        say("!! The backup failed, so nothing was deleted:")
        say("   " + (r.stderr or "").strip().replace("\n", "\n   "))
        return
    r = subprocess.run(_mysql_base(s, mysql) + [db, "-e", f"DELETE FROM item_template WHERE entry IN ({ids})"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0:
        say("!! The delete did not run (your backup is still at " + bfile + "):")
        say("   " + (r.stderr or r.stdout).strip().replace("\n", "\n   "))
        return
    say()
    say(f"Deleted {len(rows)} item(s). Backup: {bfile}")
    say(f"To put them back: in the mysql prompt, USE {db}; then SOURCE {bfile.replace(chr(92), '/')};")
    say("Restart the worldserver if it is running.")


# --------------------------------------------------------------------------------------------------
# looks left out
#
# The kit has no way to drop one look from a selection (`select --add` only adds), and its build keeps
# every failed look in build/plan.json - so one look a stock client cannot draw (a purely 3D back piece,
# a quiver) would block every later build. The helper keeps its own list of looks the user chose to leave
# out, in <project>/helper-excluded.json, and removes them from the kit's selection and plan after every
# change. Removing a look only takes it out of what is built; nothing the kit downloaded is deleted.

def _json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def _save_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=1)
    os.replace(tmp, path)


def excluded(s):
    return _json(os.path.join(s["work"], "helper-excluded.json"), {})


def apply_exclusions(s):
    """Remove every left-out look from the kit's selection.json and build/plan.json. Returns how many."""
    ex = excluded(s)
    if not ex:
        return 0
    removed = 0
    sel_path = os.path.join(s["work"], "selection.json")
    sel = _json(sel_path, None)
    if sel:
        for d in list(sel.get("displays", {})):
            if d in ex:
                del sel["displays"][d]
                removed += 1
        for st in sel.get("sets", {}).values():
            st["slots"] = {k: [x for x in v if str(x) not in ex] for k, v in st.get("slots", {}).items()}
            st["slots"] = {k: v for k, v in st["slots"].items() if v}
        _save_json(sel_path, sel)
    plan_path = os.path.join(s["work"], "build", "plan.json")
    plan = _json(plan_path, None)
    if plan:
        for d in list(plan):
            if d in ex:
                del plan[d]
        _save_json(plan_path, plan)
    return removed


def failed_looks(s):
    plan = _json(os.path.join(s["work"], "build", "plan.json"), {})
    return {d: v for d, v in plan.items() if not v.get("ok") and not str(v.get("error", "")).startswith("Encrypted")}


def offer_to_leave_out(s):
    """After a failed build: offer to leave the failing looks out. True if the user agreed."""
    bad = failed_looks(s)
    if not bad:
        return False
    say()
    say(f"{len(bad)} look(s) could not be built:")
    for d, v in sorted(bad.items(), key=lambda kv: int(kv[0])):
        items = ",".join(str(i.get("item", i)) if isinstance(i, dict) else str(i) for i in v.get("items", []))
        say(f"   look {d}  ({v.get('slot', '?')}{', items ' + items if items else ''}): {v.get('error', '')}")
    say()
    say("'Nothing a 3.3.5 client can draw' means the piece is purely 3D (a back item or quiver, say) with")
    say("no flat texture a stock client could show instead. Everything else in your sets is fine.")
    if not yes("Leave these looks out and carry on with the rest?"):
        return False
    ex = excluded(s)
    for d, v in bad.items():
        ex[d] = {"slot": v.get("slot"), "reason": v.get("error"), "when": now()}
    _save_json(os.path.join(s["work"], "helper-excluded.json"), ex)
    apply_exclusions(s)
    say(f"Left out {len(bad)} look(s). They stay left out when you add more sets later.")
    return True


# --------------------------------------------------------------------------------------------------
# scanning safely

def ensure_scan(s):
    """Run the kit's scan, with its own patch temporarily out of Data so it reads the stock tables."""
    say()
    say("The kit needs to read your client first.")
    name = patch_name(s)
    data = s["client_data"]
    moved = []
    installed = [n for n in os.listdir(data) if name and n.lower() == name.lower()]
    if installed:
        say(f"{name} (your earlier backports) is installed. The helper will move it aside while the kit")
        say("reads your client, then put it straight back.")
        if wow_running():
            say("!! Close World of Warcraft first.")
            return False
        hold = os.path.join(s["work"], "backups", "scan-hold")
        os.makedirs(hold, exist_ok=True)
        for n in installed:
            shutil.move(os.path.join(data, n), os.path.join(hold, n))
            moved.append(n)
    try:
        return run_step(s, "scan")
    finally:
        hold = os.path.join(s["work"], "backups", "scan-hold")
        for n in moved:
            shutil.move(os.path.join(hold, n), os.path.join(data, n))
        if moved:
            try:
                os.rmdir(hold)
            except OSError:
                pass


# --------------------------------------------------------------------------------------------------
# 2. build

def build(s):
    title("Build everything you have added")
    led = ledger(s)
    if not led.get("select", {}).get("ok"):
        say("Nothing has been added yet. Choose 'Add something' first.")
        return False
    if not passed(s, "scan") and not ensure_scan(s):
        return False
    say("This runs every step in order. The first build of a big set can take a while; later builds")
    say("only redo what changed. If one step fails, the helper stops and explains.")
    apply_exclusions(s)
    for step in STEPS:
        if passed(s, step):
            say(f"   already done: {step}")
            continue
        extra = ("--extension",) if step == "build" else ()
        if run_step(s, step, *extra):
            continue
        if step == "build" and offer_to_leave_out(s) and run_step(s, step, *extra):
            continue
        return False
    sheets = os.path.join(s["work"], "sheets")
    say()
    say("Build finished. Last check, and it matters: look at the preview pictures.")
    say("Look for helms floating above or sunk into heads, and white, black or bright green patches.")
    say("Non-player races (Naga, Vrykul and so on) do not matter.")
    open_folder(sheets)
    if not yes("Do the pictures look right?"):
        say("Then do not install yet. Take a screenshot of what looks wrong and ask for help.")
        return False
    s["built_ok_at"] = now()
    save_settings(s)
    say("Good. Choose 'Install' next.")
    return True


# --------------------------------------------------------------------------------------------------
# 3. install

def now():
    return datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S")


def wow_running():
    if os.name != "nt":
        return False
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Wow.exe"], capture_output=True, text=True).stdout
        return "wow.exe" in out.lower()
    except OSError:
        return False


def copy_any(src, dst):
    if os.path.isdir(src):
        shutil.copytree(src, dst)
    else:
        shutil.copy2(src, dst)


def remove_any(path):
    if os.path.isdir(path):
        shutil.rmtree(path)
    elif os.path.exists(path):
        os.remove(path)


def kit_archives(s):
    out = os.path.join(s["work"], "out")
    return sorted(f for f in os.listdir(out) if f.lower().endswith(".mpq") and os.path.isfile(os.path.join(out, f)))


def install(s):
    title("Install into your game (and server)")
    if not passed(s, "pack"):
        say("There is nothing built to install yet. Choose 'Build' first.")
        return False
    if wow_running():
        say("!! World of Warcraft is running. Close it first, then choose Install again.")
        return False
    data = s["client_data"]
    archives = kit_archives(s)
    if not archives:
        say("!! The kit's out folder holds no patch. Choose 'Build' again.")
        return False

    say("Making Equip Plus's 3D-piece patch from what the kit exported ...")
    armour = s.get("armor_pieces", False)
    try:
        conv = converter.convert(s["work"], letter=None, armor_pieces=armour, log=lambda m: say("    " + m))
        w_patch = conv["patch"]
    except converter.ConvertError as ex:
        if "nothing to convert" in str(ex):
            say("    (no 3D pieces to add for these sets - that is fine)")
            w_patch = None
        else:
            say(f"!! {ex}")
            return False

    stamp = now()
    backup = os.path.join(s["work"], "backups", stamp)
    record = {"time": stamp, "client": [], "server": [], "added": []}
    os.makedirs(os.path.join(backup, "client"), exist_ok=True)

    def place(src, name):
        dest = os.path.join(data, name)
        if os.path.exists(dest):
            copy_any(dest, os.path.join(backup, "client", name))
            record["client"].append(name)
            remove_any(dest)
        else:
            record["added"].append(os.path.join(data, name))
        copy_any(src, dest)
        say(f"   installed {name}")

    say()
    say(f"Backing up and installing (backup: {backup}) ...")
    for a in archives:
        place(os.path.join(s["work"], "out", a), a)
    if w_patch:
        place(w_patch, os.path.basename(w_patch))
    cache = os.path.join(client_root(s), "Cache")
    if os.path.isdir(cache):
        shutil.rmtree(cache, ignore_errors=True)
        say("   cleared the client's Cache folder")

    if s.get("server_dbc"):
        say()
        say("Server tables: merging your sets into the server's own Item.dbc and ItemDisplayInfo.dbc ...")
        merged = os.path.join(s["work"], "out", "server-dbc")
        code = merger.main(["--work", s["work"], "--server", s["server_dbc"], "--out", merged])
        if code != 0:
            say("!! The merge failed - your server was not touched.")
        elif yes("Copy the merged tables into the server's dbc folder now (the old ones are backed up)?"):
            os.makedirs(os.path.join(backup, "server"), exist_ok=True)
            for name in merger.SERVER_TABLES:
                dest = os.path.join(s["server_dbc"], name + ".dbc")
                shutil.copy2(dest, os.path.join(backup, "server", name + ".dbc"))
                shutil.copy2(os.path.join(merged, name + ".dbc"), dest)
                record["server"].append(name + ".dbc")
                say(f"   updated {name}.dbc")
        run_sql(s)
    with open(os.path.join(backup, "install.json"), "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2)
    s["last_backup"] = backup
    save_settings(s)

    check_equip_plus(s)
    say()
    say("Installed. Restart the worldserver if it is running, then log in.")
    say("Your new items are in the kit's item list: open the project's out\\sql\\item_template.sql, or use")
    say(".lookup item <name> in game, then .additem <id>.")
    return True


def run_sql(s):
    files = [p for p in (os.path.join(s["work"], "out", "sql", "item_template_removed.sql"),
                         os.path.join(s["work"], "out", "sql", "item_template.sql"),
                         os.path.join(s["work"], "out", "equip-plus-rescue", "sql", "item_template.sql"))
             if os.path.isfile(p)]
    if not files:
        say("!! No item_template.sql was built - skipped.")
        return
    say()
    if not yes("Run the item SQL on your world database now?"):
        say(f"Skipped. To run it yourself: in the mysql prompt, USE {s.get('db_world', 'acore_world')}; then")
        for sql in files:
            say("SOURCE " + sql.replace("\\", "/") + ";")
        return
    mysql = s.get("mysql") or "mysql"
    if not (os.path.isfile(mysql) or shutil.which(mysql)):
        say(f"!! Cannot find mysql at '{mysql}'. Change it under Settings, or run the SQL yourself:")
        for sql in files:
            say("   SOURCE " + sql.replace("\\", "/") + ";")
        return
    pw = getpass.getpass(f"Password for {s.get('db_user')} (not shown, not stored): ")
    env = dict(os.environ, MYSQL_PWD=pw)
    for sql in files:
        with open(sql, "rb") as f:
            r = subprocess.run([mysql, "-h", s.get("db_host", "127.0.0.1"), "-P", str(s.get("db_port", "3306")),
                                "-u", s.get("db_user", "acore"), s.get("db_world", "acore_world")],
                               stdin=f, env=env, capture_output=True, text=True)
        what = ("brought-back items' SQL" if "equip-plus-rescue" in sql else
                "removed items' SQL" if sql.endswith("item_template_removed.sql") else "item SQL")
        if r.returncode == 0:
            say(f"   {what} applied")
        else:
            say(f"!! The {what} did not apply:")
            say("   " + (r.stderr or r.stdout).strip().replace("\n", "\n   "))
            return


def check_equip_plus(s, update=False):
    dll = os.path.join(client_root(s), "Extensions", "wxl-equip-plus", "wxl-equip-plus.dll")
    if os.path.isfile(dll) and not update:
        return
    say()
    if update:
        if wow_running():
            say("!! Close World of Warcraft first - the game keeps the extension file open.")
            return
        if not yes("Download the latest Equip Plus and replace the installed one?"):
            return
    else:
        say("Equip Plus is not installed, so the 3D belts and other pieces will not show.")
        if not yes("Download the latest Equip Plus and install it now?"):
            say("You can install it from the WXL Hub instead.")
            return
    try:
        with urllib.request.urlopen(EQUIP_PLUS_LATEST, timeout=30) as r:
            rel = json.load(r)
        asset = next(a for a in rel.get("assets", []) if a["name"] == "wxl-equip-plus.dll")
        os.makedirs(os.path.dirname(dll), exist_ok=True)
        with urllib.request.urlopen(asset["browser_download_url"], timeout=120) as r, open(dll, "wb") as f:
            shutil.copyfileobj(r, f)
        say(f"   installed Equip Plus {rel.get('tag_name', '')}")
    except (OSError, ValueError, StopIteration, KeyError) as ex:
        say(f"!! Could not download it ({ex}). Install it from the WXL Hub instead.")


# --------------------------------------------------------------------------------------------------
# 4. undo

def undo(s):
    title("Undo the last install")
    backup = s.get("last_backup")
    rec_path = os.path.join(backup or "", "install.json")
    if not backup or not os.path.isfile(rec_path):
        say("There is no install to undo.")
        return
    with open(rec_path, encoding="utf-8") as f:
        rec = json.load(f)
    say(f"This puts back what the install on {rec['time']} replaced:")
    for n in rec["client"]:
        say(f"   client: {n}")
    for p in rec["added"]:
        say(f"   remove: {p} (it did not exist before)")
    for n in rec["server"]:
        say(f"   server: {n}")
    if not yes("Go ahead?", False):
        return
    if wow_running():
        say("!! Close World of Warcraft first.")
        return
    data = s["client_data"]
    for n in rec["client"]:
        remove_any(os.path.join(data, n))
        copy_any(os.path.join(backup, "client", n), os.path.join(data, n))
    for p in rec["added"]:
        remove_any(p)
    for n in rec["server"]:
        shutil.copy2(os.path.join(backup, "server", n), os.path.join(s["server_dbc"], n))
    os.rename(rec_path, rec_path + ".undone")
    s["last_backup"] = ""
    save_settings(s)
    say("Done. Delete the client's Cache folder and restart the worldserver. The database rows from the")
    say("SQL are left in place; they do nothing without the tables and patches.")


# --------------------------------------------------------------------------------------------------
# bring back left-out looks

def bring_back(s):
    title("Bring back left-out looks")
    ex = excluded(s)
    if not ex:
        say("No looks have been left out, so there is nothing to bring back.")
        return
    say("These looks were left out because a stock client cannot draw them:")
    for d, v in sorted(ex.items()):
        say(f"   look {d} ({v.get('slot')})")
    say()
    say("Equip Plus CAN draw their 3D pieces. The helper has the kit convert them on a small side project,")
    say("gives each its own item, and adds them to patch-W the next time you Install. Rigged pieces bind")
    say("to the skeleton by bone name, so on stock bodies the fit may not be perfect - Undo puts it back.")
    ext_dbc = os.path.join(client_root(s), "Extensions", "wxl-extended-dbc", "wxl-extended-dbc.dll")
    if not os.path.isfile(ext_dbc):
        say()
        say("!! wxl-extended-dbc is not installed. The new items' table rows travel as its add-on files,")
        say("   so install it from the WXL Hub first, or these looks will not appear.")
        if not yes("Carry on anyway?", False):
            return
    if not yes("Bring them back?"):
        return
    try:
        r = rescuer.rescue(s["work"], s["kit"], log=say)
    except rescuer.RescueError as ex2:
        say(f"!! {ex2}")
        return
    for p in r["problems"]:
        say(f"   PROBLEM {p}")
    say()
    say(f"Done: {len(r['rows'])} model row(s) staged. Choose 'Install' to put them in your game and server.")


# --------------------------------------------------------------------------------------------------
# status and settings

def status(s):
    title("Where things stand")
    say(f"Project:  {s['work']}")
    say(f"Client:   {s['client_data']}")
    say(f"Server:   {s.get('server_dbc') or '(not managed by the helper)'}")
    led = ledger(s)
    say()
    for step in ["scan", "select"] + STEPS:
        r = led.get(step)
        mark = "done" if r and r.get("ok") else ("FAILED" if r else "to do")
        say(f"   {mark:7} {STEP_WORDS.get(step, step)}")
    data = s["client_data"]
    say()
    name = patch_name(s)
    if name:
        say(f"   {name} installed in Data: {'yes' if os.path.exists(os.path.join(data, name)) else 'no'}")
    for n in sorted(os.listdir(data)):
        p = os.path.join(data, n, converter.MARKER)
        if os.path.isfile(p):
            say(f"   {n} (Equip Plus 3D pieces) installed: yes")
    dll = os.path.join(client_root(s), "Extensions", "wxl-equip-plus", "wxl-equip-plus.dll")
    say(f"   Equip Plus extension installed: {'yes' if os.path.isfile(dll) else 'no'}")
    rec = _json(os.path.join(s["work"], "rescued.json"), {})
    if rec:
        say("   Looks brought back with Equip Plus: " + ", ".join(
            f"{d} (display {v.get('display')}, item {', '.join(map(str, v.get('items', [])))})"
            for d, v in sorted(rec.items())))
    ex = excluded(s)
    if ex:
        say(f"   Looks left out (a stock client cannot draw them): "
            + ", ".join(f"{d} ({v.get('slot')})" for d, v in sorted(ex.items())))
    say(f"   3D armour pieces (HD bodies only): {'included' if s.get('armor_pieces') else 'belts only'}")


def settings_menu(s):
    title("Settings")
    say("  1. Run setup again (kit, client, project, server)")
    say(f"  2. 3D armour pieces: currently {'ON' if s.get('armor_pieces') else 'OFF'} "
        "(boots, gloves, legs, robe skirts - these fit HD bodies only)")
    say("  3. Update Equip Plus to the latest release")
    say("  4. Back")
    c = ask("Choose", "4")
    if c == "1":
        setup(s)
    elif c == "3":
        check_equip_plus(s, update=True)
    elif c == "2":
        s["armor_pieces"] = not s.get("armor_pieces")
        save_settings(s)
        say(f"3D armour pieces are now {'ON' if s['armor_pieces'] else 'OFF'}. Install again to apply.")


# --------------------------------------------------------------------------------------------------

def main():
    title("Backport Helper - retail armour for your 3.3.5a client")
    if not find_python_ok():
        pause()
        return 1
    s = load_settings()
    if not (s.get("kit") and s.get("client_data") and s.get("work")) or not os.path.isfile(bp_py(s)):
        if not setup(s):
            pause()
            return 1
    while True:
        title("What would you like to do?")
        say("  1. Add something      (search retail and pick sets or items)")
        say("  2. Build              (convert everything you have added)")
        say("  3. Install            (put it in your game and server, with a backup)")
        say("  4. Undo last install")
        say("  5. Where things stand")
        say("  6. Settings")
        say("  7. Quit")
        say("  8. Bring back left-out looks  (3D pieces only Equip Plus can draw)")
        say("  9. Remove something           (take sets or items out, or clean up the database)")
        c = ask("Choose a number", "")
        try:
            if c == "1":
                add_something(s)
            elif c == "2":
                build(s)
            elif c == "3":
                install(s)
            elif c == "4":
                undo(s)
            elif c == "5":
                status(s)
            elif c == "6":
                settings_menu(s)
            elif c == "7":
                return 0
            elif c == "8":
                bring_back(s)
            elif c == "9":
                remove_something(s)
            else:
                continue
        except KeyboardInterrupt:
            say("\n(stopped)")
        except Exception as ex:  # keep the window open and explain, rather than vanish
            say(f"\n!! Something unexpected went wrong: {ex!r}")
            say("   Nothing further was changed. Take a screenshot of this window if you ask for help.")
        pause()


if __name__ == "__main__":
    sys.exit(main())
