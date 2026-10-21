"""Time shen-c binaries on shenbench workloads with /usr/bin/time -l.

usage: python3 -I run.py LOG LABEL=BIN[,LABEL=BIN...] WORKLOAD[,...] ROUNDS [--warm] [--timeout S] [--env K=V]...

Each driver runs as `/usr/bin/time -l BIN script driver.shen` with cwd set to
the driver directory. Native code is cached per binary (SHEN_RC_NATIVE_CACHE
keyed by the binary's sha256); --warm first runs every driver once untimed so
timed runs do not include C compiler time. Every timed run checks
RC_NATIVE compiles=0 and compares payload() with the go-1f587de reference.
Rows are appended to LOG as JSON lines.
"""
import hashlib, json, os, re, subprocess, sys, threading, time
from pathlib import Path

SP = Path("/private/tmp/claude-502/-Users-reuben-projects-capcov-fork/f9388696-09e2-41d3-92e3-080e770493d1/scratchpad")
W = SP / "shenbench/workloads"
REF = SP / "shenbench/results-c2/out/time-c2/0/go-1f587de"
CACHE = SP / "speed/perceus-cache"
MARKS = ("CAPCOV-OPERATION-RESULT ", "CAPCOV-BENCH-RESULT ", "MC ")
BEGIN, END = "<<<CAPCOV-SHEN-JSON-BEGIN>>>", "<<<CAPCOV-SHEN-JSON-END>>>"


def payload(stdout):  # copied from bench_c2.py
    out, inside = [], False
    for line in stdout.splitlines():
        s = line.rstrip("\r")
        if s == BEGIN:
            inside = True; out.append(s); continue
        if s == END:
            inside = False; out.append(s); continue
        if inside or s.startswith(MARKS):
            out.append(s)
    return "\n".join(out) + "\n"


def drivers(w):
    d = W / w
    if (d / "driver.shen").exists():
        return [d]
    return sorted(p for p in d.iterdir() if (p / "driver.shen").exists())


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def one(binary, d, env, timeout):
    p = subprocess.Popen(["/usr/bin/time", "-l", binary, "script", "driver.shen"], cwd=d,
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         env=env, start_new_session=True)
    killed = []
    def kill():
        killed.append(1)
        try: os.killpg(p.pid, 9)
        except ProcessLookupError: pass
    t = threading.Timer(timeout, kill); t.start()
    o, e = p.communicate(); t.cancel()
    o = o.decode("utf-8", "replace"); e = e.decode("utf-8", "replace")
    def num(pat):
        m = re.findall(pat, e, re.M)
        return float(m[-1]) if m else 0.0
    tl = re.findall(r"^\s*([\d.]+) real\s+([\d.]+) user\s+([\d.]+) sys$", e, re.M)
    real, user, sys_ = map(float, tl[-1]) if tl else (0.0, 0.0, 0.0)
    native = re.search(r"RC_NATIVE .*?compiles=(\d+)", e)
    nat = dict(re.findall(r"(\w+)=([\d.]+)", re.findall(r"^RC_NATIVE .*$", e, re.M)[-1])) if native else {}
    return {"code": "timeout" if killed else p.returncode,
            "real": real, "user": user, "sys": sys_,
            "rss": num(r"^\s*(\d+)\s+maximum resident set size$"),
            "instr": num(r"^\s*(\d+)\s+instructions retired$"),
            "compiles": int(native.group(1)) if native else None,
            "emit_s": float(nat.get("emit_s", 0)), "load_s": float(nat.get("load_s", 0)),
            "disk_hits": int(nat.get("disk_hits", 0)), "bundle_hits": int(nat.get("bundle_hits", 0)),
            "out": o, "err": e}


def run(binary, w, env, timeout):
    agg = {"code": [], "real": 0.0, "cpu": 0.0, "rss": 0.0, "instr": 0.0, "compiles": 0,
           "emit_s": 0.0, "load_s": 0.0, "disk_hits": 0}
    pays = []
    for d in drivers(w):
        r = one(binary, d, env, timeout)
        agg["code"].append(r["code"]); agg["real"] += r["real"]; agg["cpu"] += r["user"] + r["sys"]
        agg["rss"] = max(agg["rss"], r["rss"]); agg["instr"] += r["instr"]
        agg["compiles"] += r["compiles"] or 0
        for k in ("emit_s", "load_s", "disk_hits"):
            agg[k] += r[k]
        pays.append(payload(r["out"]))
        if r["code"] != 0:
            agg.setdefault("stderr_tail", r["err"][-2000:])
    pay = "".join(pays)
    ref = (REF / f"{w}.payload").read_text()
    agg["payload_ok"] = pay == ref
    agg["payload_sha"] = hashlib.sha256(pay.encode()).hexdigest()[:16]
    return agg


def main():
    a = sys.argv[1:]
    log, bins, wls, rounds = Path(a[0]), [x.split("=", 1) for x in a[1].split(",")], a[2].split(","), int(a[3])
    timeout = float(a[a.index("--timeout") + 1]) if "--timeout" in a else 900
    envs = {}
    for label, b in bins:
        h = sha(b)
        env = {**os.environ, "SHEN_RC_NATIVE_CACHE": str(CACHE / h[:16]), "SHEN_RC_NATIVE_STATS": "1"}
        for k in ("SHEN_RC_PRINT", "SHEN_RC_STATS"):
            env.pop(k, None)
        for i, arg in enumerate(a):
            if arg == "--env":
                k, v = a[i + 1].split("=", 1); env[k] = v
        envs[label] = (b, h, env)
    if "--warm" in a:
        for label, (b, h, env) in envs.items():
            for w in wls:
                r = run(b, w, env, timeout)
                print(f"warm {label} {w} cpu={r['cpu']:.1f} compiles={r['compiles']} ok={r['payload_ok']}", flush=True)
    with open(log, "a") as fh:
        for rnd in range(rounds):
            for w in wls:
                for label, (b, h, env) in envs.items():
                    la0 = os.getloadavg()[0]
                    r = run(b, w, env, timeout)
                    la1 = os.getloadavg()[0]
                    row = {"ts": int(time.time()), "round": rnd, "workload": w, "label": label, "bin": b,
                           "mode": env.get("SHEN_RC_EXECUTION", "native"),
                           "bin_sha256": h, "load_before": round(la0, 1), "load_after": round(la1, 1), **r}
                    fh.write(json.dumps(row) + "\n"); fh.flush()
                    print(f"r{rnd} {w:10} {label:10} cpu={r['cpu']:8.2f}s real={r['real']:8.2f}s "
                          f"instr={r['instr']/1e9:8.1f}G rss={r['rss']/1e6:7.1f}MB compiles={r['compiles']} "
                          f"emit={r['emit_s']:.1f}s codes={r['code']} ok={r['payload_ok']} load={la0:.0f}/{la1:.0f}", flush=True)


main()
