#!/usr/bin/env python3
"""Prepare a Processing sketch for the C++ compiler.

Java does not care what order anything is declared in; C++ does. Processing's
own preprocessor papers over that for Java, and this does the equivalent for
C++:

  1. Forward declarations for classes, cross-tab globals, and functions.
  2. Every .pde in the sketch folder becomes ONE translation unit -- the
     editor's "tabs" are not separate compilation units -- ordered so a class
     is complete before another class uses it as a base or a value member.
  3. Class definitions are HOISTED above the functions. A sketch happily
     writes `new EggRing(...)` inside setup() and defines EggRing at the
     bottom of the file; C++ needs the definition first.
  4. A sketch that is only a list of statements ("static mode": no setup() or
     draw() anywhere) is wrapped into a setup() body.

Usage: pde_prototypes.py <main.pde> <out_prototypes.hh> <out_body.hh>
Both outputs are included by sketch_pde.cc. The body carries #line directives
so compiler errors still point into the original .pde.
"""
import os
import re
import sys

SIG = re.compile(
    r"^([A-Za-z_]\w*)\s*[*&]?\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*\{?\s*$")
CLASS = re.compile(r"^(class|struct)\s+([A-Za-z_]\w*)")
DECL = re.compile(r"^(.+?[\s*&])([A-Za-z_]\w*)\s*(\[[^\]]*\])?$")

NOT_A_TYPE = {
    "return", "else", "if", "for", "while", "do", "switch", "case", "new",
    "delete", "using", "namespace", "typedef", "template", "public", "private",
    "protected", "class", "struct", "enum", "union", "const", "static",
    "extern", "inline", "virtual", "friend", "operator", "sizeof", "throw",
}


def strip_code(text):
    """Blank out comments and literals so brace counting can trust the text."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        two = text[i:i + 2]
        if two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append("".join(ch if ch == "\n" else " " for ch in text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def split_classes(text):
    """-> (class_chunks, other_chunks), each (start_line, source_text)."""
    lines = text.splitlines(keepends=True)
    blank = strip_code(text).splitlines(keepends=True)
    starts = [i for i, ln in enumerate(lines) if CLASS.match(ln)]

    classes, taken = [], set()
    for s in starts:
        if s in taken:
            continue
        depth, seen_brace, end = 0, False, None
        for i in range(s, len(lines)):
            for ch in blank[i]:
                if ch == "{":
                    depth += 1
                    seen_brace = True
                elif ch == "}":
                    depth -= 1
            if seen_brace and depth <= 0:
                end = i
                break
        if end is None:  # unbalanced; leave it where it is
            continue
        # Consume the `;` after the closing brace if it is on a later line.
        while end + 1 < len(lines) and not blank[end].rstrip().endswith(";") \
                and blank[end + 1].strip().startswith(";"):
            end += 1
        classes.append((s, "".join(lines[s:end + 1])))
        taken.update(range(s, end + 1))

    others, run_start, run = [], None, []
    for i, ln in enumerate(lines):
        if i in taken:
            if run:
                others.append((run_start, "".join(run)))
                run, run_start = [], None
            continue
        if run_start is None:
            run_start = i
        run.append(ln)
    if run:
        others.append((run_start, "".join(run)))
    return classes, others


def global_decl(line):
    s = line.strip()
    if not s.endswith(";") or s.startswith(("#", "//", "*")):
        return None
    s = s[:-1]
    for cut in ("=", "("):
        i = s.find(cut)
        if i > 0:
            s = s[:i]
    m = DECL.match(s.strip())
    if not m:
        return None
    kind = m.group(1)
    # const/constexpr globals are compile-time constants here (array bounds!);
    # an extern declaration would demote them. `void` means a function.
    if re.search(r"\b(const|constexpr|static|extern|typedef|using|void|return)\b", kind):
        return None
    return kind, m.group(2), m.group(3) or ""


def scan(path):
    funcs, classes, globals_ = [], [], []
    text = open(path, encoding="utf-8", errors="replace").read()
    for line in text.splitlines():
        m = CLASS.match(line)
        if m:
            classes.append((m.group(1), m.group(2)))
            continue
        m = SIG.match(line)
        if m and m.group(1) not in NOT_A_TYPE:
            funcs.append(f"{m.group(1)} {m.group(2)}({m.group(3)});")
            continue
        if line[:1].strip():
            g = global_decl(line)
            if g:
                globals_.append(g)
    return funcs, classes, globals_, text


def order_files(files, defined_in, texts):
    deps = {f: set() for f in files}
    for f in files:
        words = set(re.findall(r"[A-Za-z_]\w*", texts[f]))
        for name, owner in defined_in.items():
            if owner != f and name in words:
                deps[f].add(owner)

    ordered, pending = [], list(files)
    while pending:
        ready = [f for f in pending if not (deps[f] - set(ordered))]
        if not ready:
            ready = [pending[0]]  # cycle: forward declarations cover it
        ready.sort()
        for f in ready:
            ordered.append(f)
            pending.remove(f)
    return ordered


def emit(fh, path, start_line, text):
    fh.write(f'#line {start_line + 1} "{os.path.abspath(path)}"\n')
    fh.write(text)
    if not text.endswith("\n"):
        fh.write("\n")


def main():
    main_pde, out_proto, out_body = sys.argv[1], sys.argv[2], sys.argv[3]
    folder = os.path.dirname(main_pde) or "."
    files = sorted(
        os.path.join(folder, f) for f in os.listdir(folder) if f.endswith(".pde"))
    if main_pde not in files:
        files.append(main_pde)

    all_funcs, all_classes, texts, defined_in, globals_in = [], [], {}, {}, {}
    for f in files:
        funcs, classes, globals_, text = scan(f)
        all_funcs += funcs
        all_classes += classes
        texts[f] = text
        for _, name in classes:
            defined_in[name] = f
        for g in globals_:
            globals_in[g[1]] = (f, g)

    static_mode = not all_funcs
    ordered = order_files(files, defined_in, texts)

    with open(out_proto, "w") as fh:
        fh.write(f"// generated by tools/pde_prototypes.py from {main_pde}"
                 " -- do not edit\n#pragma once\n")
        if not static_mode:
            for key, name in all_classes:
                fh.write(f"{key} {name};\n")
            for name, (owner, (kind, nm, arr)) in sorted(globals_in.items()):
                if any(o != owner and re.search(rf"\b{re.escape(name)}\b", texts[o])
                       for o in files):
                    fh.write(f"extern {kind}{nm}{arr};\n")
            for sig in all_funcs:
                fh.write(sig + "\n")

    split = {f: split_classes(texts[f]) for f in ordered}
    with open(out_body, "w") as fh:
        fh.write(f"// generated by tools/pde_prototypes.py from {main_pde}"
                 " -- do not edit\n#pragma once\n")
        for f in ordered:  # every class definition, before any function body
            for start, text in split[f][0]:
                emit(fh, f, start, text)
        if static_mode:
            # The file is a statement list, so it becomes the body of setup().
            # Drawing once is enough: the render target persists across frames.
            fh.write("void setup()\n{\n")
        for f in ordered:
            for start, text in split[f][1]:
                emit(fh, f, start, text)
        if static_mode:
            fh.write("}\nvoid draw() {}\n")


main()
