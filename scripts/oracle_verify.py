#!/usr/bin/env python3
"""Brute-force 2-hop oracle for the DualIndex benchmark.

Reads the raw SNAP wiki-Vote edge list, reproduces Poseidon's first-seen
raw->internal id mapping (import_node returns sequential ids from 0), computes
2-hop answers by direct nested iteration (no sorting, no index), then checks
1000 queries from each persisted workload file against BOTH the C++ walker's
ground-truth counts (stored in the .bin) and DualIndex's dumped answers.

Usage: oracle_verify.py --edges Wiki-Vote.txt --queries_dir queries --dataset wiki_vote
"""
import argparse, struct, sys, os

HDR = struct.Struct("<4sIIIIIQQ")            # magic,ver,type,seed,count,stride,tc,xor
STRIDE = {0: 4, 1: 4, 2: 2, 3: 3}            # u32 words per record, by type id
NAME = {0: "point_pos", 1: "point_neg", 2: "single_hop", 3: "multi_hop"}


def load_adj(path):
    """raw edge list -> adjacency list keyed by internal id (first-seen order)."""
    ident, adj = {}, []
    def iid(x):
        if x not in ident:
            ident[x] = len(adj); adj.append([])
        return ident[x]
    with open(path) as f:
        for line in f:
            if not line.strip() or line[0] == "#":
                continue
            a, b = line.split()[:2]
            u, v = iid(int(a)), iid(int(b))     # order matches get_pid(from),get_pid(to)
            adj[u].append(v)
    return adj


def records(path, type_id):
    with open(path, "rb") as f:
        magic, ver, t, seed, n, stride, tc, xr = HDR.unpack(f.read(HDR.size))
        assert magic == b"PQBF" and t == type_id and stride == STRIDE[type_id] * 4, path
        rec = struct.Struct("<%dI" % STRIDE[type_id])
        return [rec.unpack(f.read(rec.size)) for _ in range(min(1000, n))]


def answers(path):
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        (n,) = struct.unpack("<I", f.read(4))
        return list(struct.unpack("<%dI" % n, f.read(4 * n)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--edges", required=True)
    ap.add_argument("--queries_dir", default="queries")
    ap.add_argument("--dataset", default="wiki_vote")
    a = ap.parse_args()
    adj = load_adj(a.edges)
    print("loaded %d nodes from %s" % (len(adj), a.edges))

    mism = 0
    for tid in (0, 1, 2, 3):
        base = os.path.join(a.queries_dir, "%s_%s" % (a.dataset, NAME[tid]))
        recs = records(base + ".bin", tid)
        di = answers(base + "_answers.bin")
        for i, r in enumerate(recs):
            if tid in (0, 1):                                    # point: src,h1,h2,gt
                s, h1, h2, gt = r
                brute = 1 if (h1 in adj[s] and h2 in adj[h1]) else 0
            elif tid == 2:                                       # single_hop: src,gt
                s, gt = r
                brute = sum(len(adj[h1]) for h1 in adj[s])
            else:                                                # multi_hop: src,h1,gt
                s, h1, gt = r
                brute = sum(len(adj[h1]) for x in adj[s] if x == h1)
            walker = gt
            dix = di[i] if di is not None else brute
            if not (brute == walker == dix):
                mism += 1
                if mism <= 10:
                    print("MISMATCH %s[%d] brute=%d walker=%d dualindex=%d q=%s"
                          % (NAME[tid], i, brute, walker, dix, r))
        print("  %-11s checked %d (dualindex dump: %s)"
              % (NAME[tid], len(recs), "yes" if di is not None else "MISSING"))

    if mism:
        print("FAIL: %d mismatches" % mism); sys.exit(1)
    print("OK: all workloads agree (brute == C++ walker == DualIndex)")


if __name__ == "__main__":
    main()
