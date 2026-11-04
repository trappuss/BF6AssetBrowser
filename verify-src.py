#!/usr/bin/env python3
"""Pre-build source checks for the AssetBrowser monorepo — ONE script, per-project tables (plan §6.6).

The checks are the union of what has actually broken a build in this family, each one carried
over from the project that learned it (the docstrings say which):
  D4   0 empty/truncated file · 1 unbalanced {}()[] · 2 header-only helper used without a REAL
       #include directive · 3 printf-style format-vs-arg mismatch · 4 Qt macro-name collisions ·
       5 duplicate lambda locals · 6 duplicate QHash/QMap literal keys · 7 CsvCopy::install order ·
       8 QSettings keys written and never read · 9 combos persisted by currentText()
  FOX  10 qPrintable (mangles non-ASCII through the Windows codepage — invisible on Linux)
  POE2 11 name-substring text matches, COUNTED against a baseline (template §3.8: classify by
       authored data, never a name substring; a count above the baseline is the review trigger)

Every table is per project: HEADER_ONLY (which namespaces live in which header), the dead-key
allow-list, the currentText baseline and the substring baseline. Inventory + baseline everywhere
a rule has legitimate exceptions — existing entries are listed, only growth fails.

    python verify-src.py                 # every project present in the tree
    python verify-src.py core poe2       # some projects
    python verify-src.py --quiet         # only problems
Exit 0 = clean, 1 = a hard failure.
"""
from __future__ import annotations
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent


def relpath(f: Path) -> str:
    try:
        return f.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return f.as_posix()


# ── Per-project tables ──────────────────────────────────────────────────────────────────────────
# roots: folders scanned. header_only: namespace -> header as the #include directive spells it.
# dead_allowed: QSettings keys written but read elsewhere by a means this script cannot see, WITH
# the reason. text_baseline: combos persisted by label, per file, with the reason. substr_baseline:
# the reviewed count of .contains/startsWith/endsWith(literal) per file (0 = none expected).
PROJECTS = {
    "core": {
        "roots": ["core", "conformance"],
        "header_only": {
            "core::rm":        "core/math/RigMath.h",
            "core::paths":     "core/app/AppPaths.h",
            "core::settings":  "core/app/CoreSettings.h",
            "core::hotkeys":   "core/app/Hotkeys.h",
            "core::cachemaint":"core/app/CacheMaint.h",
            "core::density":   "core/app/Density.h",
            "core::fontscale": "core/app/FontScale.h",
            "core::glyph":     "core/ui/Glyphs.h",
            "core::search":    "core/ui/QueryTerm.h",
            "core::menuctx":   "core/ui/MenuContext.h",
            "core::MenuText":  "core/ui/MenuText.h",
            "core::searchbox": "core/ui/SearchBox.h",
            "core::TextReport":"core/ui/TextReportDialog.h",
            "core::PanelPersist": "core/ui/PanelPersist.h",
            "core::hover":     "core/ui/HoverPreview.h",
        },
        "dead_allowed": {
            # Written by core::settings / the SettingsDialog, read by every plugin's tabs through
            # core::settings::value(QStringLiteral(key)) — the literal appears in CoreSettings.h.
        },
        "text_baseline": {},
        "substr_baseline": {
            # Reviewed 2026-09-20 — every hit is a text test on a syntax token, a log level, a file
            # extension or a JSON key; none decides what an ASSET is.
            "core/app/AppShell.cpp": 2,            # "hotkeys/view", "hotkeys/anim" key-name prefixes
            "core/app/LogConsole.cpp": 4,          # log level column (WARN/CRIT/FATAL/DBG)
            "core/app/StartupProfile.h": 1,        # "index built=" profile mark
            "core/export/ExportLayout.cpp": 1,     # self-test: a sanitised folder name
            "core/export/GlbExporter.cpp": 11,     # ".gltf" + self-test JSON key presence
            "core/tabs/ModelsTab.cpp": 5,          # .glb/.png/.gif on a user-typed save path + exportTo: ".gltf" — the export KIND is the path's extension; audit 2026-09-25: "_tex" on the core's OWN thumbnail folder name (may an emptied one be removed) — text, not a classification
            "core/tabs/TexturesTab.cpp": 1,        # .png
            "core/ui/SearchQuery.h": 3,            # self-test cases
            "core/customize/Build.cpp": 3,         # the build text's own key prefixes (slot./toggle.) and a self-test on its output
            "core/app/Harness.cpp": 1,             # "random:" — the harness's --build syntax for a seeded random build
            "conformance/tests/test_export_util.cpp": 2,   # self-tests: a folder name, .dds
            "conformance/tests/test_glb.cpp": 6,           # JSON key presence
            "conformance/tools/shell_check.cpp": 36,        # run 23 (2026-10-05): the vehicle-seat check reads its two notes ("modelled in place", "CNP_HMIC"); run 22 (2026-09-26): the unskinned-piece check reads its own note's opening words; 2e-5a (2026-09-26): the looks block's assertions on note text ("1 model(s) not placed"), look keys and group names (exact QMap/QSet/QStringList membership the regex also counts), asset-path tails of its own synthetic ids (c03.ftex, c10.ftex, 2020_c01.fv2), and its stand-in swatch hook picking a colour by its own test path ("scol") — none classifies game data; 2e-4 (2026-09-26): the harness subject=@<n> check reads its "the other list has 2" text; run 18 (2026-09-25): the harness @<n> check reads its "no such slot" text; 2e-3 (2026-09-25): the weapon fit rule's allowed ids (exact QSet membership the regex also counts) and the "not in this install: <stem>" note — assertions, not classifications; 2e-2 (2026-09-25): FOX's Exclude note says the default conflicts too — an assertion on note text; audit 2026-09-25: the thumbnail folder path names core_thumbs_v<N> — a text assertion; 2e core (2026-09-25): the seat note's opening words and the "left whole" note — assertions on note text; 2c (2026-09-25): a look carries the group name it hides — an exact QSet membership test the regex also counts; 2b (2026-09-24): the unseated note names its socket and says "unbaked" — assertions on the note text; 2a (2026-09-24): three keys in the build text and two section captions in a slot list — assertions on text the test itself reads back; the SYNTHETIC store's .dds bucket, a failed-asset name, the product name + "animations" key in a written .glb
            "conformance/tools/viewport_check.cpp": 6,     # shader source markers
        },
    },
    "poe2": {
        "roots": ["games/poe2"],
        "header_only": {
            "core::rm":       "core/math/RigMath.h",
            "core::settings": "core/app/CoreSettings.h",
            "core::glyph":    "core/ui/Glyphs.h",
            "QueryTerm":      "util/QueryTerm.h",
            "ExportLayout":   "util/ExportLayout.h",
            "Facets":         "index/Facets.h",
        },
        "dead_allowed": {},
        "text_baseline": {},
        "substr_baseline": {
            # POE2's own rule: a folder-segment test on authored paths is a path-structure test; the
            # alias-slot tokens (body/gloves/boots/head/face) are the .ao's OWN alias names, and
            # hair/beard/_lod//lod//wings/ are the reviewed POE2 filters carried over verbatim.
            "games/poe2/Poe2CustomizeSource.cpp": 11,   # counted 11 on 2026-09-25 (lowered from 14: the tool asked; the .ao keyword tests no longer all match the literal form); the same 10, moved from tabs/CustomizeTab.cpp (2026-09-23); +4 in logAoChain: .ao line keywords (extends / socket / parent / attached_object) — text parsing of a text format, not a classification
            "games/poe2/Poe2Store.cpp": 2,      # .smd/.fmt — the file KIND is its extension
        },
    },
    "fox": {
        "roots": ["games/fox"],
        "header_only": {
            "core::rm":       "core/math/RigMath.h",
            "QueryTerm":      "util/QueryTerm.h",
            "searchq":        "util/SearchQuery.h",
            "ExportLayout":   "util/ExportLayout.h",
            "cachemaint":     "app/CacheMaint.h",
            "Hotkeys":        "app/Hotkeys.h",
            "handbook":       "app/Handbook.h",
        },
        "dead_allowed": {}, "text_baseline": {},
        "substr_baseline": {
            "games/fox/FoxPlugin.cpp": 5,   # "0x" prefix on a typed id term (twice: isIdTerm, idTermMatches); "other:" — FOX's --character spec syntax; "slotcolor:" — a field of FOX's own build-text grammar, counted by --build-roundtrip; "slotcolor:" in a harness spec (does it ask for a swatch)
            "games/fox/FoxStore.cpp": 1,    # ".fmdl" — the sibling .fcnp is found by swapping the extension
            "games/fox/tabs/FoxFilesPreview.cpp": 1,   # ".fmdl" chopped off a base name for the export stem
            # 2e-2 (2026-09-25), FOX's own rules carried over verbatim, as POE2's filters were: "main" /
            # "body" / "ply" and "_def" are displayNameFor (CTW:127-135: a uniform's suit name goes on its
            # body model only; the stem tidied for display); "_m" / "_f" are lookSex (CTW:1944), which
            # picks the preset table by the catalogue's OWN subject id (PC:549, PC:973), not an asset
            # name; ".fmdl" — the sibling .fcnp by extension, as FoxStore
            "games/fox/FoxCensus.cpp": 1,   # run 22 (2026-09-26): ".fmdl" — the file KIND by its extension, to find the .fcnp beside it (as FoxStore::hardpoints)
            "games/fox/FoxCustomizeSource.cpp": 11,   # 2e-5a (2026-09-26): three CLASSIFICATIONS, ported verbatim from FOX's camoSectionFor / camoLabelFor (CTW:2867, CTW:3421) for parity with FOX's own tab — "camo_c" / "scol_c" table names and a "_skin" .fv2 path are the game's own naming, which FOX measured (--fovacensus) and keys its Camo / Variation captions on; no authored field says it; +1 2e-3: ".fmdl" in the stem index (CTW:4355) — the file KIND by its extension, as FOX's own
        },
    },
    "d4":  {"roots": ["games/d4"],  "header_only": {"core::rm": "core/math/RigMath.h"}, "dead_allowed": {}, "text_baseline": {},
            "substr_baseline": {
                "games/d4/D4Store.cpp": 1,   # "~unnamed_" — the name SnoIndex itself GENERATES for an SNO with no known name
            }},
    # DrStore::classifyTexture splits Models vs Textures by a record's file EXTENSION — the
    # template's warned-against name match, kept DELIBERATELY as a placeholder until DR's cooked-object
    # type field is decoded (README "Not done yet"). It is a QRegularExpression, which this script does
    # not count as a substring match, so the baseline stays empty.
    "dr":  {"roots": ["games/dr"],  "header_only": {}, "dead_allowed": {}, "text_baseline": {},
            "substr_baseline": {}},
}

QT_MACROS = {"emit", "signals", "slots", "foreach"}


def strip_code(text: str) -> str:
    """Blank out strings, char literals and comments with a single-pass scanner.

    A regex pipeline is not good enough here: an earlier version tested for `/*` BEFORE
    stripping `//`, so a `/*` inside a line comment flipped it into block-comment mode and
    swallowed the rest of the file — reporting phantom imbalances on files that compile.
    Handles raw strings R"delim(...)delim" too, which appear in shader sources.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        # line comment
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        # block comment
        if c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")          # keep line numbers usable
                i += 1
            i += 2
            continue
        # raw string R"delim( ... )delim"
        if c == "R" and nxt == '"':
            j = text.find("(", i + 2)
            if j > 0:
                delim = text[i + 2:j]
                close = ')' + delim + '"'
                k = text.find(close, j)
                if k > 0:
                    out.append('""')
                    out.extend("\n" * text.count("\n", i, k))
                    i = k + len(close)
                    continue
        # ordinary string
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append('""')
            continue
        # char literal
        if c == "'":
            i += 1
            while i < n and text[i] != "'":
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append("''")
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_balance(path: Path, code: str) -> list[str]:
    b = code.count("{") - code.count("}")
    p = code.count("(") - code.count(")")
    s = code.count("[") - code.count("]")
    if (b, p, s) == (0, 0, 0):
        return []
    return [f"unbalanced delimiters: braces {b:+d}, parens {p:+d}, brackets {s:+d}"]


def check_header_only_includes(path: Path, raw: str, code: str, HEADER_ONLY: dict) -> list[str]:
    """A namespace used but never included. Matches the DIRECTIVE, not a substring."""
    problems = []
    for ns, header in HEADER_ONLY.items():
        if not re.search(rf"\b{re.escape(ns)}::", code):
            continue                                   # not used here
        # The DEFINING header cannot include itself. Without this, adding a second namespace from
        # an existing header to the table immediately fails that header.
        if path.as_posix().endswith(header):
            continue
        pat = re.compile(rf'^\s*#\s*include\s+["<]{re.escape(header)}[">]\s*$', re.M)
        if not pat.search(raw):
            problems.append(
                f"uses {ns}:: but has no `#include \"{header}\"` directive "
                f"(a comment mentioning the path does NOT count)")
    return problems


FMT_CALL = re.compile(r"\b(qInfo|qWarning|qCritical|qDebug|printf|fprintf)\s*\(", re.M)


CTX_INSTALL_RE = re.compile(r"\b(?:CsvCopy::install|installCopyMenu)\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)")
CTX_POLICY_RE = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*->\s*setContextMenuPolicy\s*\(\s*Qt::(\w+)")


def check_ctx_menu_order(text: str) -> list[str]:
    """CsvCopy::install must come AFTER the view sets its own context-menu policy.

    CsvCopy::install only declines to add its Copy/Copy all menu when the view ALREADY has a
    policy set. Called first, it sees DefaultContextMenu, installs a handler, and the caller's
    later connect() adds a SECOND handler to the same signal — Qt runs both, CsvCopy's is
    connected first, so its menu opens and the real one is unreachable until dismissed.

    This shipped in three views (ModelsTab m_list, ModelsTab m_partsView, TexturesTab m_view) and
    is invisible in review: every line is individually correct and the menu simply never changes.
    Cheap to check mechanically, so it is checked on every build.
    """
    problems: list[str] = []
    installs: dict[str, list[int]] = {}
    policies: dict[str, list[tuple[int, str]]] = {}
    for i, line in enumerate(text.split("\n"), 1):
        m = CTX_INSTALL_RE.search(line)
        if m:
            installs.setdefault(m.group(1), []).append(i)
        m2 = CTX_POLICY_RE.search(line)
        if m2:
            policies.setdefault(m2.group(1), []).append((i, m2.group(2)))
    for var, lines_ in installs.items():
        for il in lines_:
            later = [(pl, pk) for pl, pk in policies.get(var, [])
                     if pl > il and pk != "DefaultContextMenu"]
            if later:
                pl, pk = later[0]
                problems.append(
                    f"line {il}: CsvCopy::install({var}) runs BEFORE {var} sets its own "
                    f"context-menu policy at line {pl} (Qt::{pk}) — CsvCopy will install a "
                    f"competing Copy/Copy all menu that hides the real one. Move the install "
                    f"AFTER the setContextMenuPolicy call.")
    return problems


def _split_args(s: str) -> list[str]:
    """Top-level comma split, respecting nesting AND string/char literals.

    The literal handling is the fix for a long-standing false positive: the docstring used to
    claim literals were "already-stripped", but the argument list handed here still contains
    them, so a comma INSIDE a string — qWarning("...", "a, b") or any message containing a
    comma — was counted as an argument separator. Every such call was reported as an arg/spec
    mismatch, and the workaround was to reword messages with em dashes, i.e. the checker was
    quietly dictating prose. Now a literal is skipped whole, escapes included.
    """
    args, depth, cur = [], 0, ""
    i, n = 0, len(s)
    while i < n:
        ch = s[i]
        if ch in "\"'":
            quote = ch
            j = i + 1
            while j < n:
                if s[j] == "\\":       # escape: consume the next char whatever it is
                    j += 2
                    continue
                if s[j] == quote:
                    j += 1
                    break
                j += 1
            cur += s[i:j]
            i = j
            continue
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append(cur)
            cur = ""
        else:
            cur += ch
        i += 1
    if cur.strip():
        args.append(cur)
    return args


def _skip_ws(t: str, i: int) -> int:
    while i < len(t):
        if t[i] in " \t\r\n":
            i += 1
        elif t.startswith("//", i):
            i = t.find("\n", i)
            if i < 0:
                return len(t)
        elif t.startswith("/*", i):
            j = t.find("*/", i)
            i = len(t) if j < 0 else j + 2
        else:
            break
    return i


def _read_string_run(t: str, i: int):
    """Consume consecutive "..." literals (C concatenation). Returns (contents, next_index)."""
    parts, saw = [], False
    while True:
        i = _skip_ws(t, i)
        if i >= len(t) or t[i] != '"':
            break
        saw = True
        i += 1
        buf = ""
        while i < len(t) and t[i] != '"':
            if t[i] == "\\" and i + 1 < len(t):
                buf += t[i:i + 2]
                i += 2
                continue
            buf += t[i]
            i += 1
        i += 1
        parts.append(buf)
    return ("".join(parts) if saw else None), i


def check_format_args(path: Path, raw: str) -> list[str]:
    """Compare % specifiers against argument count.

    Parses the call rather than guessing with rindex('"'): arguments frequently CONTAIN string
    literals (ternaries, qPrintable(...)), which made a naive split report every such call as
    having zero arguments. Only the leading concatenated literal run is the format string; if
    the format is not a literal (a variable), the call is skipped rather than guessed at.
    """
    problems = []
    for m in FMT_CALL.finditer(raw):
        fn = m.group(1)
        i = raw.index("(", m.start())
        depth, j = 0, i
        while j < len(raw):
            if raw[j] == "(":
                depth += 1
            elif raw[j] == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if j >= len(raw):
            continue
        body = raw[i + 1:j]
        k = 0
        if fn == "fprintf":                     # first arg is the stream
            args0 = _split_args(body)
            if len(args0) < 2:
                continue
            k = len(args0[0]) + 1
        fmt, k = _read_string_run(body, k)
        if fmt is None:
            continue                            # format is not a literal — cannot check
        specs = [s for s in re.findall(r"%[-+ #0-9.*hlLqjzt]*[diouxXeEfgGaAcspn%]", fmt)
                 if s != "%%"]
        if not specs:
            continue
        rest = body[k:].lstrip()
        if rest.startswith(","):
            rest = rest[1:]
        args = [a for a in _split_args(rest) if a.strip()]
        if len(args) != len(specs):
            line = raw[:m.start()].count("\n") + 1
            problems.append(
                f"line {line}: {fn}() has {len(specs)} format specifier(s) "
                f"but {len(args)} argument(s)")
    return problems


DECL = re.compile(r"\b(?:auto|int|float|double|bool|QString|QMenu|QAction)\s+(\w+)\s*=")

# The DECL list above is a closed set of types and only matches the `TYPE NAME =` form, so it
# missed `QHash<QString, LatestSlot> slots;` — templated type, no initialiser — and misses
# parameters entirely. That cost a full build cycle: `slots` expands to nothing, so the line became
# `QHash<QString, LatestSlot> ;` and every later `slots.insert(...)` compiled as `.insert(...)`.
# MSVC then reports "syntax error: '.'" pointing at correct-looking code, several functions away
# from the actual mistake, which is close to the worst possible diagnostic.
#
# Two broader patterns, both keyed on the macro name being USED as an identifier:
#   USE  — member access. `slots.` / `signals->` / `slots[` cannot be anything but a mistake.
#   DECL2 — `<type> NAME` followed by ; = , ) — covers locals, members and parameters.
# Neither fires on the legitimate spellings: `public slots:` and `signals:` are followed by ':',
# and `emit obj.sig()` captures `obj`, not `emit`.
QT_MACRO_USE = re.compile(r"\b(emit|signals|slots|foreach)\s*(?:\.|->|\[)")
QT_MACRO_DECL2 = re.compile(r"[>\w\]]\s*[&*]?\s+(emit|signals|slots|foreach)\s*[;=,)]")


def check_qt_macro_names(path: Path, code: str) -> list[str]:
    problems = []
    seen = set()

    def add(pos: int, name: str, why: str) -> None:
        line = code[:pos].count("\n") + 1
        if (line, name) in seen:
            return
        seen.add((line, name))
        problems.append(
            f"line {line}: `{name}` is a Qt macro that expands to NOTHING — {why}. "
            f"Rename the identifier.")

    for m in DECL.finditer(code):
        if m.group(1) in QT_MACROS:
            add(m.start(), m.group(1), "the declaration silently disappears")
    for m in QT_MACRO_USE.finditer(code):
        add(m.start(), m.group(1), "used here as an object, so this line loses its subject")
    for m in QT_MACRO_DECL2.finditer(code):
        add(m.start(), m.group(1), "declared here as a variable or parameter")
    return problems


BODY = re.compile(r"^(?:\w[\w:<>,~\s\*&]*?)\b(\w+::\w+)\s*\([^;{]*\)\s*(?:const\s*)?\{", re.M)
LOCAL = re.compile(r"^\s{4,}auto\s+(\w+)\s*=\s*\[", re.M)


def check_duplicate_locals(path: Path, code: str) -> list[str]:
    """Two `auto NAME = [...]` at the same brace depth inside one function body.

    Splicing a lambda body into a new member function easily duplicates the helper lambdas it
    already declared — MSVC reports 'redefinition; multiple initialization' for each, three
    errors per name, and it costs a whole build cycle to find out. Cheap to catch here.
    """
    problems = []
    for m in BODY.finditer(code):
        start = m.end() - 1
        depth, i, n = 0, start, len(code)
        while i < n:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        body = code[start:i]
        seen = {}
        for d in LOCAL.finditer(body):
            name = d.group(1)
            if name in seen:
                line = code[:start + d.start()].count("\n") + 1
                problems.append(
                    f"line {line}: `{name}` declared twice in {m.group(1)}() — "
                    f"duplicate lambda (MSVC: 'redefinition; multiple initialization')")
            else:
                seen[name] = True
    return problems


MAP_CONTAINERS = ("QHash", "QMap", "std::map", "std::unordered_map")


def strip_comments(text: str) -> str:
    """Comments out, string CONTENTS kept — strip_code blanks strings, which this check needs."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                i += 1
            continue
        if c == "/" and nxt == "*":
            i += 2
            while i + 1 < n and not (text[i] == "*" and text[i + 1] == "/"):
                if text[i] == "\n":
                    out.append("\n")
                i += 1
            i += 2
            continue
        if c in "\"'":
            q, out_start = c, i
            i += 1
            while i < n and text[i] != q:
                if text[i] == "\\":
                    i += 1
                i += 1
            i += 1
            out.append(text[out_start:i])
            continue
        out.append(c)
        i += 1
    return "".join(out)


def check_duplicate_map_keys(path: Path, raw: str) -> list[str]:
    """A key repeated inside one QHash/QMap brace initializer.

    THIS EXISTS BECAUSE IT SILENTLY UNDID A CORRECTION. SnoIndex::groupNameMap() set
    {140,"Face"} and {152,"AppearanceSet"} from measured CoreTOC data, and twelve lines later
    the same initializer repeated 140 and 152 as "BattlePassTier" and "TrackedReward". A brace
    initializer keeps the LAST literal for a repeated key, so two guesses overwrote two facts,
    the fix appeared in the diff, shipped, and did nothing. Nothing warned: it compiles, it is
    not a duplicate symbol, and the file reads correctly unless you happen to compare every key
    against every other one.

    Scoped to the associative containers ONLY. An array of structs may legitimately repeat its
    first member, so flagging every `{a, b}` list would be noise; in a QHash/QMap initializer a
    repeated key is a bug every time.
    """
    problems = []
    text = strip_comments(raw)
    for m in re.finditer(r"=\s*\{", text):
        head = text[max(0, m.start() - 220):m.start()]
        head = head[head.rfind(";") + 1:]                 # this declaration only
        if not any(c in head for c in MAP_CONTAINERS):
            continue
        # Walk to the matching close brace, collecting the elements one level in.
        i, depth, n = m.end() - 1, 0, len(text)
        elems = []                                        # (key, offset)
        while i < n:
            ch = text[i]
            if ch in "\"'":                                # skip a literal wholesale
                q = ch
                i += 1
                while i < n and text[i] != q:
                    i += 2 if text[i] == "\\" else 1
                i += 1
                continue
            if ch == "{":
                depth += 1
                if depth == 2:                            # start of one element
                    j, d2 = i + 1, 0
                    while j < n:
                        c2 = text[j]
                        if c2 in "\"'":
                            q = c2
                            j += 1
                            while j < n and text[j] != q:
                                j += 2 if text[j] == "\\" else 1
                        elif c2 in "{([":
                            d2 += 1
                        elif c2 in "})]":
                            if d2 == 0:
                                break
                            d2 -= 1
                        elif c2 == "," and d2 == 0:
                            break
                        j += 1
                    elems.append((" ".join(text[i + 1:j].split()), i))
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        if len(elems) < 3:                                # not a lookup table
            continue
        seen = {}
        for key, off in elems:
            if not key:
                continue
            if key in seen:
                line = text[:off].count("\n") + 1
                problems.append(
                    f"line {line}: key {key} repeated in this QHash/QMap initializer "
                    f"(first at line {seen[key]}) — the LAST literal wins, so the earlier "
                    f"entry is silently discarded")
            else:
                seen[key] = text[:off].count("\n") + 1
    return problems


def check_truncation(path: Path, raw: str) -> list[str]:
    """Empty or near-empty source file — almost always a botched write, not intent.

    THIS EXISTS BECAUSE THE OTHER CHECKS CANNOT SEE IT. An editing script that opened a file
    for writing before reading it truncated main.cpp and CacheVersioning.h to ZERO BYTES, and
    this script reported "131 file(s) clean" — an empty file has balanced delimiters, no bad
    format strings and no duplicate lambdas. It passed every test with flying colours because
    there was nothing left to test.

    A .cpp/.h in this tree is never legitimately empty: even the thinnest header carries
    `#pragma once` and a comment. The floor is deliberately low (a handful of bytes) so this
    only ever fires on real damage, never on a small-but-real file.
    """
    stripped = raw.strip()
    if not stripped:
        return ["FILE IS EMPTY (0 bytes of content) — almost certainly a truncated write. "
                "Restore it from .Backups/ before doing anything else."]
    # A file with no directive, no comment and no brace is not plausibly source.
    if len(stripped) < 24 and not any(t in stripped for t in ("#", "//", "{", ";")):
        return [f"file is only {len(stripped)} byte(s) and contains no code — "
                f"looks truncated; check .Backups/ before building"]
    return []


QPRINTABLE = re.compile(r"\bqPrintable\s*\(")


def check_qprintable(path: Path, code: str) -> list[str]:
    """`qPrintable` converts through the LOCAL 8-BIT codepage.

    On Windows that is the ANSI codepage, and it destroys the string twice
    over: a character with no CP1252 encoding degrades to `?` (a logged
    U+2192 arrow came out as a literal question mark), and one that does
    encode comes back as a single high byte that is then invalid UTF-8 and
    lands in the log file as U+FFFD. Both were found in a real run's log,
    and NEITHER can be reproduced on Linux, where the local 8-bit codec is
    already UTF-8 and the round trip is lossless — so the container will
    never catch this and this check has to.

    `qUtf8Printable` is the same macro without the codepage step.
    """
    problems = []
    for m in QPRINTABLE.finditer(code):
        line = code[:m.start()].count("\n") + 1
        problems.append(
            f"line {line}: `qPrintable` mangles non-ASCII on Windows "
            f"(local 8-bit codepage). Use `qUtf8Printable`.")
    return problems


def _settings_bodies(files: list) -> dict:
    """path -> source with line comments stripped. Shared by the three checks below."""
    out = {}
    for f in files:
        try:
            raw = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        out[relpath(f)] = "\n".join(l.split("//")[0] for l in raw.splitlines())
    return out


# ── 1. Keys written and never read ─────────────────────────────────────────────────────────────
# Accepted entries, with the reason each is not a defect. Empty is the goal; add to it only with a
# reason a reader can check, never to silence the tool.

_SET_LITERAL = re.compile(r'setValue\s*\(\s*QStringLiteral\(\s*"([A-Za-z0-9_]+/[^"%]*)"')
_REMOVE_LITERAL = r'remove\s*\(\s*QStringLiteral\(\s*"%s"'
_SETVALUE_LITERAL = r'setValue\s*\(\s*QStringLiteral\(\s*"%s"'
# QStringLiteral("wardrobe2/viewport/") + key      → the whole family is reachable by a helper
_CONCAT_PREFIX = re.compile(r'QStringLiteral\(\s*"([A-Za-z0-9_/]*/)"\s*\)\s*\+')
# prefix + QStringLiteral("/_loading")             → the leaf is the literal, the group is computed
_CONCAT_TAIL = re.compile(r'\+\s*QStringLiteral\(\s*"(/[A-Za-z0-9_/]+)"')


def check_dead_settings_keys(files: list, SETTINGS_DEAD_ALLOWED: dict) -> tuple:
    """A key written with a literal whose literal is read nowhere — directly or via a helper."""
    bodies = _settings_bodies(files)
    whole = "\n".join(bodies.values())
    written = {}
    for rel, body in bodies.items():
        for k in _SET_LITERAL.findall(body):
            written.setdefault(k, set()).add(rel)
    # Concatenation is how most of this codebase reads grouped keys, and ignoring it made the first
    # draft of this check report fifteen keys of which fourteen were fine. Both spellings count.
    prefixes = set(_CONCAT_PREFIX.findall(whole))
    tails = set(_CONCAT_TAIL.findall(whole))

    def is_read(k: str) -> bool:
        occurrences = whole.count('"%s"' % k)
        occurrences -= len(re.findall(_SETVALUE_LITERAL % re.escape(k), whole))
        occurrences -= len(re.findall(_REMOVE_LITERAL % re.escape(k), whole))
        if occurrences > 0:
            return True                       # named literally somewhere that is not a write
        for p in prefixes:                    # value(QStringLiteral("group/") + leaf)
            if k.startswith(p) and "/" not in k[len(p):]:
                return True
        for t in tails:                       # value(group + QStringLiteral("/leaf"))
            if k.endswith(t):
                return True
        return False

    fails, warns = [], []
    for k in sorted(written):
        if is_read(k) or k in SETTINGS_DEAD_ALLOWED:
            continue
        where = ", ".join(sorted(written[k]))
        fails.append(f'"{k}" is written by {where} and read nowhere — either wire up the read, '
                     f"delete the write, or record it in SETTINGS_DEAD_ALLOWED with a reason")
    return fails, warns


# ── 2. Combos persisted by their display text ──────────────────────────────────────────────────
# Reviewed 2026-09-12. Every site below stores the LABEL on purpose, because the label is the
# identity at that layer — the armour slots, the look presets and the theme resolver all speak
# appearance NAMES end to end, and findText is how they are restored. skinTone/skinDetail were the
# ones that did not fit that pattern (their items carry a colour and a style token their consumers
# actually use) and have been converted to currentData(). A count ABOVE the baseline means a new
# combo is being persisted by label — check whether its items carry userData first.
_TEXT_PERSIST = re.compile(r'setValue\s*\([^;]{0,240}?currentText\s*\(\s*\)', re.S)


def check_text_persisted_combos(files: list, SETTINGS_TEXT_BASELINE: dict) -> tuple:
    bodies = _settings_bodies(files)
    fails, warns = [], []
    for rel, body in sorted(bodies.items()):
        n = len(_TEXT_PERSIST.findall(body))
        if not n:
            continue
        base = SETTINGS_TEXT_BASELINE.get(rel, 0)
        if n > base:
            fails.append(f"{rel}: {n} combo(s) persisted by currentText(), baseline {base} — "
                         f"store currentData() unless the LABEL really is the identity at that "
                         f"layer, then raise the baseline with the reason")
        elif n < base:
            warns.append(f"{rel}: {n} left (baseline {base}) — lower the baseline in verify-src.py")
        else:
            warns.append(f"{rel}: {n}")
    return fails, warns


# ── 11. Name-substring text matches, counted (POE2) ─────────────────────────────────────────────
# .contains/startsWith/endsWith on a string literal: each must be a genuine text test (an
# extension, a syntax prefix, a folder-structure test on an authored path), never a classification
# that should key on authored data. Inventory + baseline per file; growth fails.
_SUBSTR = re.compile(r'\.(contains|startsWith|endsWith)\s*\(\s*(?:QStringLiteral|QLatin1String)\s*\(\s*"([^"]+)"')
_SUBSTR_RAW = re.compile(r'\.(contains|startsWith|endsWith)\s*\(\s*"([^"]+)"')


def check_substring_counts(files: list, baseline: dict) -> tuple:
    fails, warns = [], []
    for f in sorted(files):
        try:
            raw = f.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        body = "\n".join(l.split("//")[0] for l in raw.splitlines())
        n = len(_SUBSTR.findall(body)) + len(_SUBSTR_RAW.findall(body))
        rel = relpath(f)
        base = baseline.get(rel, 0)
        if n > base:
            fails.append(f"{rel}: {n} name-substring test(s), baseline {base} — confirm each is a text "
                         f"test, not a classification (template §3.8), then raise the baseline with the reason")
        elif n < base:
            warns.append(f"{rel}: {n} left (baseline {base}) — lower the baseline")
        elif n:
            warns.append(f"{rel}: {n}")
    return fails, warns


def run_project(name: str, cfg: dict, quiet: bool) -> tuple:
    files = []
    for r in cfg["roots"]:
        d = ROOT / r
        if d.exists():
            files += sorted(list(d.rglob("*.cpp")) + list(d.rglob("*.h")))
    if not files:
        return 0, 0
    total = 0
    for f in files:
        try:
            raw = f.read_text(encoding="utf-8", errors="replace")
        except OSError as e:
            print(f"[FAIL] {relpath(f)}: {e}")
            total += 1
            continue
        code = strip_code(raw)
        trunc = check_truncation(f, raw)
        if trunc:
            total += len(trunc)
            print(f"\n[FAIL] {relpath(f)}")
            for p in trunc:
                print(f"       - {p}")
            continue
        problems = (check_balance(f, code)
                    + check_header_only_includes(f, raw, code, cfg["header_only"])
                    + check_format_args(f, raw)
                    + check_qt_macro_names(f, code)
                    + check_qprintable(f, code)
                    + check_duplicate_locals(f, code)
                    + check_duplicate_map_keys(f, raw)
                    + check_ctx_menu_order(raw))
        if problems:
            total += len(problems)
            print(f"\n[FAIL] {relpath(f)}")
            for p in problems:
                print(f"       - {p}")
    for title, (cfails, cwarns) in (
            ("settings keys written but never read", check_dead_settings_keys(files, cfg["dead_allowed"])),
            ("combos persisted by display text",     check_text_persisted_combos(files, cfg["text_baseline"])),
            ("name-substring text matches",          check_substring_counts(files, cfg["substr_baseline"]))):
        if cfails:
            total += len(cfails)
            print(f"\n[FAIL] {name}: {title}")
            for p in cfails:
                print(f"       - {p}")
        if cwarns and not quiet:
            print(f"\nverify-src [{name}]: {title} (reviewed, not a failure) —")
            for p in cwarns:
                print(f"       · {p}")
    return total, len(files)


def main() -> int:
    quiet = "--quiet" in sys.argv
    wanted = [a for a in sys.argv[1:] if not a.startswith("--")] or list(PROJECTS)
    grand, nfiles = 0, 0
    for name in wanted:
        if name not in PROJECTS:
            print(f"verify-src: unknown project '{name}' (know: {', '.join(PROJECTS)})")
            return 1
        t, n = run_project(name, PROJECTS[name], quiet)
        grand += t; nfiles += n
        if not quiet:
            print(f"verify-src [{name}]: {n} file(s), {t} problem(s)")
    if grand == 0:
        if not quiet:
            print(f"verify-src: OK — {nfiles} file(s) clean (non-empty, balance, header-only includes, "
                  f"format args, Qt macro names, qPrintable, duplicate lambdas, duplicate map keys, "
                  f"CsvCopy order, dead settings keys, combo text persistence, substring baselines)")
        return 0
    print(f"\nverify-src: {grand} problem(s) in {nfiles} file(s) — fix before building.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
