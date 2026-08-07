#!/usr/bin/env python3
"""Prepare a Processing sketch for the C++ compiler.

Processing's preprocessor does three things this reproduces:

  1. Emits forward declarations, because Java does not care what order
     functions, classes and fields are defined in, and C++ does.
  2. Concatenates every .pde in the sketch folder into one translation unit --
     the "tabs" of the Processing editor are not separate compilation units.
  3. Wraps a sketch that is only a list of statements ("static mode", no
     setup()/draw() at all) into a setup() body.

Ordering matters once files are concatenated: a class used as a base or as a
value member must already be complete, so files are topologically sorted by
"file G defines a class that file F mentions". Forward declarations cover the
pointer/reference cycles a linear order cannot, and `extern` declarations
cover globals that one tab defines and another uses.

Usage: pde_prototypes.py <main.pde> <out_prototypes.hh> <out_includes.hh>
Both outputs are included by sketch_pde.cc.
"""
import os
import re
import sys

# A top-level function definition: return type and name at column 0, ending at
# the closing paren (optionally with the brace). A global with constructor
# arguments -- `PVector center(w/2, h/2);` -- ends in `;`, so the anchor keeps
# it from being read as a declaration.
SIG = re.compile(
    r"^([A-Za-z_]\w*)\s*[*&]?\s+([A-Za-z_]\w*)\s*\(([^)]*)\)\s*\{?\s*$")
CLASS = re.compile(r"^(class|struct)\s+([A-Za-z_]\w*)")
# "Type declarator" left over after an initializer or constructor args are cut.
DECL = re.compile(r"^(.+?[\s*&])([A-Za-z_]\w*)\s*(\[[^\]]*\])?$")

NOT_A_TYPE = {
    "return", "else", "if", "for", "while", "do", "switch", "case", "new",
    "delete", "using", "namespace", "typedef", "template", "public", "private",
    "protected", "class", "struct", "enum", "union", "const", "static",
    "extern", "inline", "virtual", "friend", "operator", "sizeof", "throw",
}


def global_decl(line):
    """`float gravity = 0.05;` -> ('float ', 'gravity', ''), else None."""
    s = line.strip()
    if not s.endswith(";") or s.startswith(("#", "//", "*")):
        return None
    s = s[:-1]
    for cut in ("=", "("):  # drop initializer / constructor arguments
        i = s.find(cut)
        if i > 0:
            s = s[:i]
    m = DECL.match(s.strip())
    if not m:
        return None
    kind = m.group(1)
    # `const`/`constexpr` globals are compile-time constants here (array
    # bounds!) -- an extern declaration would demote them and break the code
    # that uses them as such. `void` means we matched a function declaration.
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
        if line[:1].strip():  # column 0 only
            g = global_decl(line)
            if g:
                globals_.append(g)
    return funcs, classes, globals_, text


def order_files(files, defined_in, texts):
    """Topological sort: a file comes after every file whose classes it uses."""
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
            ready = [pending[0]]  # cycle: break it, forward decls cover us
        ready.sort()
        for f in ready:
            ordered.append(f)
            pending.remove(f)
    return ordered


def main():
    main_pde, out_proto, out_inc = sys.argv[1], sys.argv[2], sys.argv[3]
    folder = os.path.dirname(main_pde) or "."
    files = sorted(
        os.path.join(folder, f) for f in os.listdir(folder) if f.endswith(".pde"))
    if main_pde not in files:
        files.append(main_pde)

    all_funcs, all_classes, texts, defined_in = [], [], {}, {}
    globals_in = {}
    for f in files:
        funcs, classes, globals_, text = scan(f)
        all_funcs += funcs
        all_classes += classes
        texts[f] = text
        for _, name in classes:
            defined_in[name] = f
        for g in globals_:
            globals_in[g[1]] = (f, g)

    # Static mode: no functions at all, so the whole sketch is one setup() body.
    static_mode = not all_funcs
    ordered = order_files(files, defined_in, texts)

    with open(out_proto, "w") as fh:
        fh.write(f"// generated by tools/pde_prototypes.py from {main_pde}"
                 " -- do not edit\n#pragma once\n")
        if not static_mode:
            for key, name in all_classes:
                fh.write(f"{key} {name};\n")
            # Only globals one tab defines and another uses: single-file
            # sketches need none of this, so they carry none of the risk.
            for name, (owner, (kind, nm, arr)) in sorted(globals_in.items()):
                if any(o != owner and re.search(rf"\b{re.escape(name)}\b", texts[o])
                       for o in files):
                    fh.write(f"extern {kind}{nm}{arr};\n")
            for sig in all_funcs:
                fh.write(sig + "\n")

    with open(out_inc, "w") as fh:
        fh.write(f"// generated by tools/pde_prototypes.py from {main_pde}"
                 " -- do not edit\n#pragma once\n")
        if static_mode:
            # Processing "static mode": the file is a statement list, so it
            # becomes the body of setup(). Drawing once is enough -- the render
            # target persists across frames.
            fh.write("void setup()\n{\n")
            for f in ordered:
                fh.write(f'#include "{os.path.abspath(f)}"\n')
            fh.write("}\nvoid draw() {}\n")
        else:
            for f in ordered:
                fh.write(f'#include "{os.path.abspath(f)}"\n')


main()
