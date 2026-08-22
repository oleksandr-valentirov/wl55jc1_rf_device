#!/usr/bin/env python3
"""Grade a before/after pair of linkjoin runs with one formula over both windows.

The two windows must be graded by the same code or the comparison is two
analyses, not one measurement. Per board throughout: the boards differ in aim by
48 us and in slope, and pooling them reports the average of two populations.
radio_devices_docs/wl55_device/testing/bench-harness.md
"""
import sys, re, math, statistics
PAT = re.compile(r"^(\d+)\s+(\d)\s+(\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(-?\d+)\s+(\d+)\s+rssi")
LOST = re.compile(r"^(\d+)\s+(\d)\s+(\d+)\s+(-?\d+)\s+-\s")

def load(paths):
    got, sent = [], []
    for p in paths:
        for l in open(p):
            m = PAT.match(l)
            if m:
                got.append((int(m[2]), int(m[6]), int(m[7]), int(m[4])))
                sent.append(int(m[2]))
                continue
            m = LOST.match(l)
            if m:
                sent.append(int(m[2]))
    return got, sent

def per_node_fit(paths, label):
    """The slope is a per-board quantity too. Pooling two boards whose slopes
    differ reports the average of two populations and calls it a measurement -
    which is what +282 ppm was."""
    print(f"  {label}")
    for p in paths:
        xs, ys = [], []
        for l in open(p):
            m = PAT.match(l)
            if m:
                xs.append(int(m[6])); ys.append(int(m[7]))
        name = p.rsplit("/", 1)[-1]
        n = len(xs)
        if n < 3:
            print(f"    {name:<14} n={n}: too few accepted arrivals to fit")
            continue
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sxx = sum((x-mx)**2 for x in xs)
        b = sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / sxx
        a = my - b*mx
        r = [y-(a+b*x) for x, y in zip(xs, ys)]
        s = math.sqrt(sum(q*q for q in r)/(n-2)); se = s/math.sqrt(sxx)
        print(f"    {name:<14} n={n:3d}  slope {b*1e6:+5.0f} ppm  se {se*1e6:3.0f}  "
              f"t {b/se:5.2f}  95% [{(b-2.1*se)*1e6:+5.0f},{(b+2.1*se)*1e6:+5.0f}]  "
              f"intercept {a:.0f}")


def per_node_off(paths):
    """off is a per-board quantity: the two differ by ~48 us, so pooling them
    reports a spread neither board has."""
    for p in paths:
        rows = {}
        for l in open(p):
            m = PAT.match(l) or LOST.match(l)
            if m:
                rows.setdefault(int(m[2]), []).append(int(m[4]))
        if not rows:
            continue
        name = p.rsplit("/", 1)[-1]
        cells = []
        for k in sorted(rows):
            v = rows[k]
            sd = statistics.stdev(v) if len(v) > 1 else float("nan")
            cells.append(f"k{k} {statistics.mean(v):.0f} sd {sd:.0f} n {len(v)}")
        print(f"  off {name:<14} " + "   ".join(cells))


def stats(got, sent, label):
    print(f"\n===== {label} =====")
    print(f"{'k':>2} {'sent':>5} {'rx':>4} {'delivery':>9}  {'resid mean':>10} {'resid sd':>9}")
    out = {}
    for k in (0, 1, 2):
        s = sent.count(k)
        v = [g[2] for g in got if g[0] == k]
        o = [g[3] for g in got if g[0] == k]
        sd = statistics.stdev(v) if len(v) > 1 else float("nan")
        mu = statistics.mean(v) if v else float("nan")
        om = statistics.mean(o) if o else float("nan")
        osd = statistics.stdev(o) if len(o) > 1 else float("nan")
        out[k] = (len(v), sd)
        print(f"{k:>2} {s:>5} {len(v):>4} {100.0*len(v)/s if s else 0:>8.1f}%  "
              f"{mu:>10.0f} {sd:>9.0f}")
    xs = [g[1] for g in got]; ys = [g[2] for g in got]; n = len(xs)
    if n > 2:
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sxx = sum((x-mx)**2 for x in xs)
        b = sum((x-mx)*(y-my) for x, y in zip(xs, ys)) / sxx
        a = my - b*mx
        r = [y-(a+b*x) for x, y in zip(xs, ys)]
        s = math.sqrt(sum(q*q for q in r)/(n-2)); se = s/math.sqrt(sxx)
        print(f"slope {b*1e6:+.0f} ppm  se {se*1e6:.0f}  t {b/se:.2f}  "
              f"95% [{(b-2.0*se)*1e6:+.0f}, {(b+2.0*se)*1e6:+.0f}]  n={n}")
    else:
        b = se = float("nan")
        print("slope: not enough accepted arrivals to fit")
    print(f"total: {len(got)} of {len(sent)} accepted, "
          f"{100.0*len(got)/len(sent) if sent else 0:.1f}%")
    return out, b, se

def grade(pre, post):
    print("\n===== pre-registered grading =====")
    (p0n, p0s), (p2n, p2s) = pre[0][0], pre[0][2]
    (q0n, q0s), (q2n, q2s) = post[0][0], post[0][2]
    print(f"P2  sd(k=0) <= 100 us       before {p0s:.0f} -> after {q0s:.0f}   "
          f"{'PASS' if q0s <= 100 else 'FAIL (F2: the comparison is confounded)'}")
    if q2n < 8:
        print(f"P1  NOT POWERED: k=2 gave n={q2n}, the pre-registration required n>=8")
    else:
        ratio = q2s/q0s
        print(f"P1  sd(k=2) <= 2 x sd(k=0)  ratio before {p2s/p0s:.1f} -> after {ratio:.1f}   "
              f"{'PASS' if ratio <= 2 else ('FAIL (F1)' if ratio > 4 else 'inconclusive')}")
    print("P3  graded per board above, not from the pooled slope: the two boards"
          " differ,")
    print("    and the pooled figure is the average of two populations. Board B had"
          " no")
    print("    slope to flatten before the change, so P3 is gradeable on A only -"
          " which")
    print("    the pre-registration did not foresee and should have.")
    print("\nCensoring runs one way: only accepted frames are here, so both the sd and")
    print("the slope are floors. A PASS is weaker evidence than a FAIL.")

def within_cycle(paths, label):
    """The slope inside one superframe. Its three opportunities share one beacon,
    one anchor, one period estimate and one calib scale, so neither this device's
    inter-cycle noise nor the hub's scale drift reaches it. The sign flips from
    cycle to cycle, which a constant rate divergence cannot do - so the sd of
    these is the quantity, and their mean is not a rate."""
    print(f"  {label}")
    for p in paths:
        per = {}
        for l in open(p):
            m = PAT.match(l)
            if m:
                per.setdefault(int(m[1]), []).append((int(m[6]), int(m[7])))
        sl = []
        for sf in sorted(per):
            v = per[sf]
            if len(v) < 2:
                continue
            xs = [q[0] for q in v]; ys = [q[1] for q in v]
            mx, my = statistics.mean(xs), statistics.mean(ys)
            sl.append(sum((x-mx)*(y-my) for x, y in zip(xs, ys))
                      / sum((x-mx)**2 for x in xs) * 1e6)
        name = p.rsplit("/", 1)[-1]
        if not sl:
            print(f"    {name:<14} no cycle carried two or more arrivals")
            continue
        sd = f"{statistics.stdev(sl):.0f}" if len(sl) > 1 else "-"
        print(f"    {name:<14} {len(sl):2d} cycles  sd {sd:>5} ppm  "
              f"{sum(1 for x in sl if x > 0)}+/{sum(1 for x in sl if x < 0)}-  "
              + " ".join(f"{x:+.0f}" for x in sl))


def _logc(n, k):
    return math.lgamma(n+1) - math.lgamma(k+1) - math.lgamma(n-k+1)


def fisher(a, b, c, d):
    """Two-sided, by summing every table no more likely than the observed one."""
    n = a + b + c + d
    p0 = _logc(a+b, a) + _logc(c+d, c) - _logc(n, a+c)
    tot = 0.0
    for i in range(0, min(a+b, a+c) + 1):
        k = a + c - i
        if k < 0 or k > c + d:
            continue
        p = _logc(a+b, i) + _logc(c+d, k) - _logc(n, a+c)
        if p <= p0 + 1e-9:
            tot += math.exp(p)
    return min(1.0, tot)


def delivery(d):
    """Per board, because one flash moved the two boards in opposite directions
    and a pooled rate would have hidden that entirely."""
    print("\n===== delivery, per board =====")
    for tag in ("A", "B"):
        n = {}
        for w in ("before", "after"):
            got = sent = 0
            for l in open(f"{d}/{w}{tag}.txt"):
                if PAT.match(l):
                    got += 1; sent += 1
                elif LOST.match(l):
                    sent += 1
            n[w] = (got, sent)
        (g0, s0), (g1, s1) = n["before"], n["after"]
        if not s0 or not s1:
            continue
        p = fisher(g0, s0-g0, g1, s1-g1)
        r0, r1 = 100.0*g0/s0, 100.0*g1/s1
        print(f"  {tag}: {g0}/{s0} = {r0:.1f}%  ->  {g1}/{s1} = {r1:.1f}%   "
              f"{'ROSE' if r1 > r0 else 'FELL'}   Fisher two-sided p = {p:.2g}")


def arm(path, label):
    """One board's half of a between-board window."""
    per, got, sent, ks = {}, 0, 0, [0, 0, 0]
    for l in open(path):
        m = PAT.match(l)
        if m:
            got += 1
            sent += 1
            ks[int(m[2])] += 1
            per.setdefault(int(m[1]), []).append((int(m[6]), int(m[7])))
        elif LOST.match(l):
            sent += 1
    sl = []
    for sf in sorted(per):
        v = per[sf]
        if len(v) < 2:
            continue
        xs = [q[0] for q in v]
        ys = [q[1] for q in v]
        mx, my = statistics.mean(xs), statistics.mean(ys)
        sl.append(sum((x-mx)*(y-my) for x, y in zip(xs, ys))
                  / sum((x-mx)**2 for x in xs) * 1e6)
    sd = statistics.stdev(sl) if len(sl) > 1 else float("nan")
    print("  %-24s %3d/%3d = %5.1f%%   k %d/%d/%d   %2d cycles   sd %s ppm"
          % (label, got, sent, 100.0*got/sent if sent else 0, ks[0], ks[1], ks[2],
             len(sl), ("%.0f" % sd) if len(sl) > 1 else "-"))
    return {"got": got, "sent": sent, "k": ks, "cycles": len(sl), "sd": sd}


def between(control_path, treated_path):
    """Treatment separated by board inside one window, graded as pre-registered.

    The alternative - two windows on one board - was measured moving on its own:
    an untouched board's k composition went 19/9/4 to 27/4/0 at p = 0.026."""
    print("\n===== one window, treatment separated by board =====")
    c = arm(control_path, "control  BASELINE 1")
    x = arm(treated_path, "treated  BASELINE 64")
    print("\n===== pre-registered grading =====")
    low = min(c["cycles"], x["cycles"])
    if low < 8:
        print("F2  NOT POWERED: the smaller arm has %d cycles, the registration"
              " required 8." % low)
        print("    Report as not measured. Do not grade the sd on this window.")
        return 1
    rc = c["got"] / c["sent"] if c["sent"] else 0
    rx = x["got"] / x["sent"] if x["sent"] else 0
    hi, lo = max(rc, rx), min(rc, rx)
    if lo > 0 and hi / lo > 3.0:
        print("F3  the arms' accepted rates differ %.1fx (%.1f%% vs %.1f%%): the two"
              % (hi/lo, 100*rc, 100*rx))
        print("    populations are censored to different depths, so their scatters"
              " are floors")
        print("    of different heights. Report both, grade nothing.")
        return 1
    print("    sd(control) %.0f ppm   sd(treated) %.0f ppm" % (c["sd"], x["sd"]))
    if c["sd"] > x["sd"]:
        print("    PREDICTION HELD: the control scatters more, which is the"
              " direction item 41 predicts.")
    else:
        print("F1  sd(control) <= sd(treated): the prediction is wrong in"
              " direction.")
    print("\n    Censoring runs one way, so both figures are floors and a held"
          " prediction is")
    print("    weaker evidence than a failed one.")
    return 0


if __name__ == "__main__":
    d = sys.argv[1]
    if d == "--between":
        sys.exit(between(sys.argv[2], sys.argv[3]))
    pre  = stats(*load([f"{d}/beforeA.txt", f"{d}/beforeB.txt"]), "BEFORE  (two-beacon period)")
    per_node_off([f"{d}/beforeA.txt", f"{d}/beforeB.txt"])
    per_node_fit([f"{d}/beforeA.txt", f"{d}/beforeB.txt"], "slope per board:")
    within_cycle([f"{d}/beforeA.txt", f"{d}/beforeB.txt"], "within-cycle slope:")
    post = stats(*load([f"{d}/afterA.txt",  f"{d}/afterB.txt"]),  "AFTER   (64-superframe span)")
    per_node_off([f"{d}/afterA.txt",  f"{d}/afterB.txt"])
    per_node_fit([f"{d}/afterA.txt",  f"{d}/afterB.txt"], "slope per board:")
    within_cycle([f"{d}/afterA.txt",  f"{d}/afterB.txt"], "within-cycle slope:")
    delivery(d)
    grade(pre, post)
