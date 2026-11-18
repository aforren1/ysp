"""Hand mutants of the color glyphs in pack/colr.c, pack/layout.c and
pack/pack_tool.c (the mutation runner cannot build the layout library or
the tool): each is applied in place, its test built in BUILD and run, and
the file restored, also on an error. Run nothing else in the tree meanwhile.

    uv run --no-project python tests/probe/colr/hand_mutants.py REPO [ID...]

REPO's build/linux holds test_layout and test_pack_tool (YSP_BUILD_LAYOUT,
YSP_BUILD_PACK, tests on). docs/layout.md, Checks, gives the results."""
import subprocess
import sys

R = sys.argv[1]
B = R + "/build/linux"
M = [
    ("colr-01", "pack/colr.c", "return sa + sb - su > 1e-7 * (sa + sb);", "return 0 && sa + sb - su > 1e-7 * (sa + sb);", "layout"),
    ("colr-02", "pack/colr.c", "x.a = m.a; x.b = -m.b; x.c = -m.c;", "x.a = m.a; x.b = m.b; x.c = -m.c;", "layout"),
    ("colr-03", "pack/colr.c", "{ lg->cut = 1; memcpy(lg->box, b, sizeof b); }", "{ memcpy(lg->box, b, sizeof b); }", "layout"),
    ("colr-04", "pack/colr.c", "if (depth > YLAY__HB_DEPTH - 1) { w->too_deep = 1; return; }", "if (depth > 200) { w->too_deep = 1; return; }", "layout"),
    ("colr-05", "pack/colr.c", "double a = hb_color_get_alpha(c) / 255.0 * mult;", "double a = mult;", "layout"),
    ("colr-06", "pack/colr.c", "else if (w.var || var_table) ylay__refuse(p, YLAY__REF_VARIABLE, 0);", "else if (0 && (w.var || var_table)) ylay__refuse(p, YLAY__REF_VARIABLE, 0);", "layout"),
    ("colr-07", "pack/colr.c", "rec[4] = (float)(A.a / r1); rec[5] = (float)(A.c / r1);", "rec[4] = (float)(A.a / r1); rec[5] = (float)(A.b / r1);", "layout"),
    ("colr-08", "pack/colr.c", "if (r0 == 0 && r1 == 0) return YLAY__NOTHING;", "if (0) return YLAY__NOTHING;", "layout"),
    ("colr-09", "pack/colr.c", "        if (!(fabs(den) > 1e-6 * sqrt(nx * nx + ny * ny) * hypot(f->g[2] - f->g[0], f->g[3] - f->g[1]))) return YLAY__NOTHING;",
     "        if (!(fabs(den) > 1e-6 * sqrt(nx * nx + ny * ny) * hypot(f->g[2] - f->g[0], f->g[3] - f->g[1]))) { ylay__refuse(p, YLAY__REF_DEGENERATE, 0); return 0; }", "layout"),
    ("colr-10", "pack/colr.c", "                p->n_fills = o->start;   /* the backdrop is the alpha, not a layer */", "", "layout"),
    ("colr-11", "pack/colr.c", "        } else {\n            ylay__refuse(p, YLAY__REF_COMPOSITE, (int32_t)mode);\n        }", "        }", "layout"),
    ("lay-01", "pack/layout.c", "colr ? SKB_FONT_FAMILY_EMOJI : SKB_FONT_FAMILY_DEFAULT", "SKB_FONT_FAMILY_DEFAULT", "layout"),
    ("lay-02", "pack/layout.c", "            if (paint && F->colr.ref[g]) return", "            if (0 && paint && F->colr.ref[g]) return", "layout"),
    ("lay-03", "pack/layout.c", "    if (F->colr.n_paint && yol_cset_add_texels(set, F->colr.paints, F->colr.n_paint) != 0)", "    if (0)", "tool"),
    ("tool-01", "pack/pack_tool.c", "w32(d + 8, any_paint ? 2u : 1u);", "w32(d + 8, 1u);", "tool"),
    ("tool-02", "pack/pack_tool.c", "for (i = 0; i < b.n; i++) if (b.e[i]->fmt > fmt) fmt = b.e[i]->fmt;", "(void)0;", "tool"),
    ("tool-03", "pack/pack_tool.c", "        if (any_colr && other) {", "        if (0) {", "tool"),
    ("tool-04", "pack/pack_tool.c", "            if (!e->colr[f] && other)", "            if (0)", "tool"),
    ("tool-05", "pack/pack_tool.c", "(ru->flags & YLAY_RUN_PAINT ? (uint32_t)YPAK_RUN_PAINT << 16 : 0u)", "0u", "tool"),
    ("tool-06", "pack/pack_tool.c", "                        for (g = (uint32_t)yf.n_glyphs; g < fi.set_glyphs && g < font->used_n[face]; g++)",
     "                        for (g = (uint32_t)yf.n_glyphs; g < (uint32_t)yf.n_glyphs; g++)", "tool"),
]
TEST = {"layout": ("test_layout", [B + "/pack/test_layout"], B), "tool": ("test_pack_tool", [B + "/pack/test_pack_tool"], B + "/pack")}
res = []
ONLY = set(sys.argv[2:])
for mid, f, find, repl, t in M:
    if ONLY and mid not in ONLY:
        continue
    path = R + "/" + f
    src = open(path, "rb").read().decode()
    if src.count(find) != 1:
        res.append((mid, "no-match %d" % src.count(find)))
        print(res[-1], flush=True)
        continue
    try:
        open(path, "wb").write(src.replace(find, repl).encode())
        target, cmd, cwd = TEST[t]
        b = subprocess.run(["ninja", "-C", B, target], capture_output=True, text=True)
        if b.returncode:
            res.append((mid, "build-failed"))
            print(res[-1], flush=True)
            continue
        subprocess.run(["rm", "-f", B + "/pack/pack_tool_work/refused.ysppak"])
        r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=600)
        out = r.stdout + r.stderr
        fails = [l for l in out.splitlines() if "FAIL" in l]
        res.append((mid, ("killed: " + (fails[0][-120:] if fails else "exit %d" % r.returncode)) if r.returncode else "SURVIVED"))
    finally:
        open(path, "wb").write(src.encode())
    print(res[-1], flush=True)
subprocess.run(["ninja", "-C", B, "test_layout", "test_pack_tool"], capture_output=True)
print("killed %d of %d" % (sum(1 for _, s in res if s.startswith("killed")), len(res)))
