/*
 * End-to-end benchmark: a SNAP edge list loaded *natively* into Poseidon,
 * 2-hop path triples extracted by graph_db::build_learned_index(), then the
 * DualIndex (ZM-Index point queries + FloodSourceSort range queries) is
 * exercised with sampled query workloads.
 *
 * Usage:  ./bench_dualindex <dataset_path> <dataset_name> [num_queries]
 *   dataset_path  full path to a SNAP edge list (tab-separated From\tTo, # comments)
 *   dataset_name  short name for CSV output, e.g. wiki_vote / roadnet_ca / web_google
 *   num_queries   optional, default 100000 (negatives use num_queries/10)
 *
 * Phase 1: parse edge list -> import nodes/edges -> build_learned_index().
 * Phase 2: point (+/-), single-hop and multi-hop range workloads with latency,
 *          throughput and correctness measured against a ground-truth triple set
 *          built by replicating the exact 2-hop walk build_learned_index() does.
 *
 * The raw edge list is the genuine SNAP file (not pre-extracted triples), so
 * this is a real end-to-end test through Poseidon's own extraction path.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <random>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <cstdint>

#include "graph_db.hpp"
#include "graph_pool.hpp"
#include "defs.hpp"

using clk = std::chrono::high_resolution_clock;

static const std::string RESULTS_DIR = "results";
static const std::string UNIFIED_CSV = "results/poseidon_e2e_all_snap.csv";
static const unsigned SEED = 42;
static const size_t N_WARMUP = 1000;

// pack three node ids (< 2^21) into one 64-bit key
static inline uint64_t pack(uint32_t s, uint32_t h1, uint32_t h2) {
    return (uint64_t(s) << 42) | (uint64_t(h1) << 21) | uint64_t(h2);
}
// pack a (src, hop1) pair into one 64-bit key (for multi-hop ground truth)
static inline uint64_t pack2(uint32_t s, uint32_t h1) {
    return (uint64_t(s) << 21) | uint64_t(h1);
}

struct Metrics {
    std::string name;
    size_t n = 0;
    double mean_us = 0, p50 = 0, p95 = 0, p99 = 0;
    double throughput_qps = 0;
    double correct_pct = 0;
    double avg_results = 0;
};

static void finalize_latency(std::vector<double>& lat, Metrics& m) {
    m.n = lat.size();
    if (lat.empty()) return;
    double sum = 0;
    for (double v : lat) sum += v;
    m.mean_us = sum / lat.size();
    std::sort(lat.begin(), lat.end());
    auto pct = [&](double p) {
        size_t idx = (size_t)(p * (lat.size() - 1));
        return lat[idx];
    };
    m.p50 = pct(0.50);
    m.p95 = pct(0.95);
    m.p99 = pct(0.99);
    m.throughput_qps = (sum > 0) ? (lat.size() / (sum / 1e6)) : 0.0;
}

// known expected triple counts (for the +/-10% Phase 1 validation)
static double expected_triples(const std::string& name) {
    if (name == "wiki_vote")  return 4542805.0;
    if (name == "roadnet_ca") return 17523394.0;
    if (name == "web_google") return 60687836.0;
    return -1.0; // unknown -> skip validation
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0]
                  << " <dataset_path> <dataset_name> [num_queries]" << std::endl;
        return 2;
    }
    const std::string dataset_path = argv[1];
    const std::string dataset_name = argv[2];
    const size_t NQ = (argc >= 4) ? (size_t)std::stoull(argv[3]) : 100000;
    const size_t N_POINT_POS = NQ;
    const size_t N_POINT_NEG = std::max<size_t>(1, NQ / 10);
    const size_t N_RANGE = NQ;

    const std::string csv_path = "results/poseidon_e2e_" + dataset_name + ".csv";
    const std::string test_path = PMDK_PATH(std::string("dualindex_") + dataset_name);

    std::cout << "=== Poseidon x DualIndex End-to-End: " << dataset_name << " ==="
              << std::endl;
    std::cout << "    dataset_path = " << dataset_path << std::endl;
    std::cout << "    num_queries  = " << NQ << " (neg " << N_POINT_NEG << ")" << std::endl;

    // -------------------- Phase 1: parse + load --------------------
    std::cout << "\n[Phase 1] Parsing " << dataset_path << " ..." << std::endl;
    std::ifstream in(dataset_path);
    if (!in) {
        std::cerr << "ERROR: cannot open " << dataset_path << std::endl;
        return 1;
    }

    std::vector<std::pair<long, long>> edges;
    edges.reserve(6000000);
    {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            long a, b;
            if (ss >> a >> b) edges.emplace_back(a, b);
        }
    }
    std::cout << "Parsed " << edges.size() << " edges from file" << std::endl;

    auto pool = graph_pool::create(test_path);
    auto db = pool->create_graph(dataset_name);

    // map raw SNAP id -> contiguous Poseidon id, importing each node once
    std::unordered_map<long, offset_t> snap2pos;
    snap2pos.reserve(2000000);
    auto get_pid = [&](long raw) -> offset_t {
        auto it = snap2pos.find(raw);
        if (it != snap2pos.end()) return it->second;
        offset_t pid = db->import_node("Node", {});
        snap2pos.emplace(raw, pid);
        return pid;
    };

    std::cout << "Importing nodes + relationships into Poseidon ..." << std::endl;
    size_t n_edges = 0;
    for (auto& e : edges) {
        offset_t s = get_pid(e.first);
        offset_t t = get_pid(e.second);
        db->import_relationship(s, t, "EDGE", {});
        n_edges++;
    }
    size_t n_nodes = snap2pos.size();
    std::cout << "Imported nodes: " << n_nodes << std::endl;
    std::cout << "Imported edges: " << n_edges << std::endl;

    if (n_nodes > (size_t(1) << 21)) {
        std::cerr << "WARNING: " << n_nodes << " nodes exceed the 2^21 key-packing limit; "
                  << "negative-query dedup / ground-truth keys may collide." << std::endl;
    }

    // -------------------- ground-truth 2-hop walk --------------------
    // Replicates graph_db::build_learned_index()'s extraction exactly so we
    // can sample query workloads and verify results. Must run in a tx.
    std::cout << "\nExtracting ground-truth 2-hop triples (independent walk) ..." << std::endl;
    std::vector<std::array<uint32_t, 3>> triples;
    triples.reserve(20000000);
    std::unordered_map<uint32_t, std::vector<uint32_t>> range_gt; // src -> [hop2...]
    std::unordered_map<uint64_t, std::vector<uint32_t>> mh_gt;    // (src,hop1) -> [hop2...]

    db->begin_transaction();
    {
        auto& cv_nodes = db->get_nodes()->as_vec();
        offset_t max_nodes = cv_nodes.last_used() + 1;
        for (offset_t i = 0; i < max_nodes; i++) {
            if (!cv_nodes.is_used(i)) continue;
            auto& src = db->node_by_id(i);
            offset_t rid = src.from_rship_list;
            while (rid != UNKNOWN) {
                auto& r1 = db->rship_by_id(rid);
                offset_t h1 = r1.dest_node;
                auto& h1n = db->node_by_id(h1);
                offset_t rid2 = h1n.from_rship_list;
                while (rid2 != UNKNOWN) {
                    auto& r2 = db->rship_by_id(rid2);
                    offset_t h2 = r2.dest_node;
                    triples.push_back({(uint32_t)i, (uint32_t)h1, (uint32_t)h2});
                    range_gt[(uint32_t)i].push_back((uint32_t)h2);
                    mh_gt[pack2((uint32_t)i, (uint32_t)h1)].push_back((uint32_t)h2);
                    rid2 = r2.next_src_rship;
                }
                rid = r1.next_src_rship;
            }
        }
    }
    db->commit_transaction();
    const size_t num_triples = triples.size();
    std::cout << "Ground-truth triples extracted: " << num_triples << std::endl;

    // membership set for negative-query generation
    std::unordered_set<uint64_t> triple_set;
    triple_set.reserve(num_triples * 2);
    for (auto& t : triples) triple_set.insert(pack(t[0], t[1], t[2]));

    // -------------------- build the learned index --------------------
    std::cout << "\nBuilding DualIndex via build_learned_index() ..." << std::endl;
    auto bt0 = clk::now();
    db->begin_transaction();
    db->build_learned_index();
    db->commit_transaction();
    auto bt1 = clk::now();
    double build_time_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(bt1 - bt0).count() / 1000.0;
    size_t index_size_bytes = db->learned_index_size();

    std::cout << "\n[Phase 1 validation]" << std::endl;
    std::cout << "  nodes              = " << n_nodes << std::endl;
    std::cout << "  edges              = " << n_edges << std::endl;
    std::cout << "  triples extracted  = " << num_triples << std::endl;
    std::cout << "  build time (ms)    = " << build_time_ms << std::endl;
    std::cout << "  index size (bytes) = " << index_size_bytes << std::endl;

    if (num_triples == 0) {
        std::cerr << "FATAL: 0 triples extracted -- graph not loaded correctly. Aborting."
                  << std::endl;
        graph_pool::destroy(pool);
        return 1;
    }
    double EXPECTED = expected_triples(dataset_name);
    if (EXPECTED > 0) {
        double dev = std::abs((double)num_triples - EXPECTED) / EXPECTED;
        std::cout << "  expected ~" << (long)EXPECTED << " (deviation " << (dev * 100.0)
                  << "%) -> " << (dev <= 0.10 ? "PASS" : "WARN (outside 10%)") << std::endl;
    } else {
        std::cout << "  expected ~ (unknown dataset; validation skipped)" << std::endl;
    }

    // -------------------- Phase 2: query workloads --------------------
    std::cout << "\n[Phase 2] Generating workloads (seed=" << SEED << ") ..." << std::endl;
    std::mt19937 rng(SEED);

    // point: true positives sampled from extracted triples
    std::vector<std::array<uint32_t, 3>> pos_q;
    pos_q.reserve(N_POINT_POS);
    {
        std::uniform_int_distribution<size_t> pick(0, triples.size() - 1);
        for (size_t i = 0; i < N_POINT_POS; i++) pos_q.push_back(triples[pick(rng)]);
    }

    // point: negatives (random triples not present)
    std::vector<std::array<uint32_t, 3>> neg_q;
    neg_q.reserve(N_POINT_NEG);
    {
        std::uniform_int_distribution<uint32_t> nid(0, (uint32_t)n_nodes - 1);
        while (neg_q.size() < N_POINT_NEG) {
            uint32_t s = nid(rng), h1 = nid(rng), h2 = nid(rng);
            if (triple_set.find(pack(s, h1, h2)) == triple_set.end())
                neg_q.push_back({s, h1, h2});
        }
    }

    // single-hop range: sources sampled uniformly over distinct sources
    std::vector<uint32_t> sources;
    sources.reserve(range_gt.size());
    for (auto& kv : range_gt) sources.push_back(kv.first);
    std::vector<uint32_t> range_q;
    range_q.reserve(N_RANGE);
    {
        std::uniform_int_distribution<size_t> pick(0, sources.size() - 1);
        for (size_t i = 0; i < N_RANGE; i++) range_q.push_back(sources[pick(rng)]);
    }

    // multi-hop: (source, hop1) pairs sampled from existing triples
    std::vector<std::pair<uint32_t, uint32_t>> mh_q;
    mh_q.reserve(N_RANGE);
    {
        std::uniform_int_distribution<size_t> pick(0, triples.size() - 1);
        for (size_t i = 0; i < N_RANGE; i++) {
            auto& t = triples[pick(rng)];
            mh_q.emplace_back(t[0], t[1]);
        }
    }
    std::cout << "  point(+)=" << pos_q.size() << " point(-)=" << neg_q.size()
              << " range=" << range_q.size() << " multi_hop=" << mh_q.size()
              << " (distinct sources=" << sources.size() << ")" << std::endl;

    std::vector<Metrics> results;

    // ---- point queries (positive) ----
    {
        for (size_t i = 0; i < N_WARMUP && i < pos_q.size(); i++)
            db->learned_point_lookup(pos_q[i][0], pos_q[i][1], pos_q[i][2]);

        std::vector<double> lat;
        lat.reserve(pos_q.size());
        size_t found = 0;
        for (auto& q : pos_q) {
            auto t0 = clk::now();
            offset_t r = db->learned_point_lookup(q[0], q[1], q[2]);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (r != UNKNOWN) found++;
        }
        Metrics m;
        m.name = "point";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * found / pos_q.size(); // true positives should be found
        m.avg_results = (double)found / pos_q.size();
        results.push_back(m);
        std::cout << "  [point] found " << found << "/" << pos_q.size() << std::endl;
    }

    // ---- point queries (negative) ----
    {
        std::vector<double> lat;
        lat.reserve(neg_q.size());
        size_t found = 0;
        for (auto& q : neg_q) {
            auto t0 = clk::now();
            offset_t r = db->learned_point_lookup(q[0], q[1], q[2]);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (r != UNKNOWN) found++;
        }
        Metrics m;
        m.name = "point_negative";
        finalize_latency(lat, m);
        // correct = correctly reported NOT found
        m.correct_pct = 100.0 * (neg_q.size() - found) / neg_q.size();
        m.avg_results = (double)found / neg_q.size(); // = false-positive rate
        results.push_back(m);
        std::cout << "  [point_negative] false positives " << found << "/" << neg_q.size()
                  << " (hit rate " << m.avg_results << ")" << std::endl;
    }

    // ---- single-hop range queries ----
    {
        for (size_t i = 0; i < N_WARMUP && i < range_q.size(); i++)
            db->learned_range_query(range_q[i]);

        std::vector<double> lat;
        lat.reserve(range_q.size());
        double total_results = 0;
        for (uint32_t s : range_q) {
            auto t0 = clk::now();
            auto res = db->learned_range_query(s);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            total_results += res.size();
        }
        // untimed correctness pass vs ground truth (multiset compare on hop2)
        size_t exact = 0;
        for (uint32_t s : range_q) {
            auto res = db->learned_range_query(s);
            std::vector<uint32_t> got;
            got.reserve(res.size());
            for (offset_t v : res) got.push_back((uint32_t)v);
            std::vector<uint32_t> exp = range_gt[s];
            std::sort(got.begin(), got.end());
            std::sort(exp.begin(), exp.end());
            if (got == exp) exact++;
        }
        Metrics m;
        m.name = "single_hop";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * exact / range_q.size();
        m.avg_results = total_results / range_q.size();
        results.push_back(m);
        std::cout << "  [single_hop] exact matches " << exact << "/" << range_q.size()
                  << ", avg results " << m.avg_results << std::endl;
    }

    // ---- multi-hop range queries: (src, hop1) pinned, hop2 open ----
    {
        for (size_t i = 0; i < N_WARMUP && i < mh_q.size(); i++)
            db->learned_multi_hop_query(mh_q[i].first, mh_q[i].second);

        std::vector<double> lat;
        lat.reserve(mh_q.size());
        double total_results = 0;
        for (auto& q : mh_q) {
            auto t0 = clk::now();
            auto res = db->learned_multi_hop_query(q.first, q.second);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            total_results += res.size();
        }
        // untimed correctness pass vs (src,hop1) ground truth (multiset of hop2)
        size_t exact = 0;
        for (auto& q : mh_q) {
            auto res = db->learned_multi_hop_query(q.first, q.second);
            std::vector<uint32_t> got;
            got.reserve(res.size());
            for (offset_t v : res) got.push_back((uint32_t)v);
            std::vector<uint32_t> exp = mh_gt[pack2(q.first, q.second)];
            std::sort(got.begin(), got.end());
            std::sort(exp.begin(), exp.end());
            if (got == exp) exact++;
        }
        Metrics m;
        m.name = "multi_hop";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * exact / mh_q.size();
        m.avg_results = total_results / mh_q.size();
        results.push_back(m);
        std::cout << "  [multi_hop] exact matches " << exact << "/" << mh_q.size()
                  << ", avg results " << m.avg_results << std::endl;
    }

    // ==================== Phase 3: Native Baselines ====================
    std::cout << "\n[Phase 3] Native baselines (B+-tree + pointer-chasing) ..." << std::endl;

    // ---- Phase 3a: build a native im_btree on packed (src,hop1,hop2) keys ----
    auto bt_b0 = clk::now();
    auto btree = make_im_btree();
    for (size_t i = 0; i < triples.size(); i++) {
        uint64_t key = pack(triples[i][0], triples[i][1], triples[i][2]);
        btree->insert(key, (offset_t)i);
    }
    auto bt_b1 = clk::now();
    double btree_build_ms =
        std::chrono::duration_cast<std::chrono::microseconds>(bt_b1 - bt_b0).count() / 1000.0;
    // payload estimate (key + value per entry); the tree's internal node overhead
    // is on top of this, so this is a lower bound reported for comparison.
    size_t btree_size_bytes = triples.size() * (sizeof(uint64_t) + sizeof(offset_t));
    std::cout << "  B+-tree built: " << triples.size() << " keys, build " << btree_build_ms
              << " ms" << std::endl;

    // ---- B+-tree positive point queries (same pos_q as Phase 2) ----
    {
        for (size_t i = 0; i < N_WARMUP && i < pos_q.size(); i++) {
            offset_t v;
            btree->lookup(pack(pos_q[i][0], pos_q[i][1], pos_q[i][2]), &v);
        }
        std::vector<double> lat;
        lat.reserve(pos_q.size());
        size_t found = 0;
        for (auto& q : pos_q) {
            uint64_t key = pack(q[0], q[1], q[2]);
            auto t0 = clk::now();
            offset_t v;
            bool ok = btree->lookup(key, &v);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (ok) found++;
        }
        Metrics m;
        m.name = "btree_point";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * found / pos_q.size();
        m.avg_results = (double)found / pos_q.size();
        results.push_back(m);
        std::cout << "  [btree_point] found " << found << "/" << pos_q.size() << std::endl;
    }

    // ---- B+-tree negative point queries (same neg_q as Phase 2) ----
    {
        std::vector<double> lat;
        lat.reserve(neg_q.size());
        size_t found = 0;
        for (auto& q : neg_q) {
            uint64_t key = pack(q[0], q[1], q[2]);
            auto t0 = clk::now();
            offset_t v;
            bool ok = btree->lookup(key, &v);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (ok) found++;
        }
        Metrics m;
        m.name = "btree_point_negative";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * (neg_q.size() - found) / neg_q.size();
        m.avg_results = (double)found / neg_q.size();
        results.push_back(m);
        std::cout << "  [btree_point_negative] false positives " << found << "/" << neg_q.size()
                  << std::endl;
    }

    // ---- Phase 3b/3c: pointer-chasing over Poseidon's linked lists (needs a tx) ----
    db->begin_transaction();

    // single-hop: walk s -> from_rship_list -> each h1 -> from_rship_list -> h2
    auto walk_single = [&](uint32_t s, std::vector<uint32_t>& out) {
        offset_t rid = db->node_by_id(s).from_rship_list;
        while (rid != UNKNOWN) {
            auto& r1 = db->rship_by_id(rid);
            offset_t h1 = r1.dest_node;
            offset_t rid2 = db->node_by_id(h1).from_rship_list;
            while (rid2 != UNKNOWN) {
                auto& r2 = db->rship_by_id(rid2);
                out.push_back((uint32_t)r2.dest_node);
                rid2 = r2.next_src_rship;
            }
            rid = r1.next_src_rship;
        }
    };
    // multi-hop: scan s's out-edges for the one(s) to h1, then walk h1's out-edges
    auto walk_multi = [&](uint32_t s, uint32_t h1, std::vector<uint32_t>& out) {
        offset_t rid = db->node_by_id(s).from_rship_list;
        while (rid != UNKNOWN) {
            auto& r1 = db->rship_by_id(rid);
            if ((uint32_t)r1.dest_node == h1) {
                offset_t rid2 = db->node_by_id(h1).from_rship_list;
                while (rid2 != UNKNOWN) {
                    auto& r2 = db->rship_by_id(rid2);
                    out.push_back((uint32_t)r2.dest_node);
                    rid2 = r2.next_src_rship;
                }
            }
            rid = r1.next_src_rship;
        }
    };
    // point: does (s -> h1 -> h2) exist? scan for h1, then scan h1's out-edges for h2
    auto point_exists = [&](uint32_t s, uint32_t h1, uint32_t h2) -> bool {
        offset_t rid = db->node_by_id(s).from_rship_list;
        while (rid != UNKNOWN) {
            auto& r1 = db->rship_by_id(rid);
            if ((uint32_t)r1.dest_node == h1) {
                offset_t rid2 = db->node_by_id(h1).from_rship_list;
                while (rid2 != UNKNOWN) {
                    auto& r2 = db->rship_by_id(rid2);
                    if ((uint32_t)r2.dest_node == h2) return true;
                    rid2 = r2.next_src_rship;
                }
            }
            rid = r1.next_src_rship;
        }
        return false;
    };

    // ---- native single-hop range (same range_q) ----
    {
        for (size_t i = 0; i < N_WARMUP && i < range_q.size(); i++) {
            std::vector<uint32_t> tmp;
            walk_single(range_q[i], tmp);
        }
        std::vector<double> lat;
        lat.reserve(range_q.size());
        double total_results = 0;
        for (uint32_t s : range_q) {
            std::vector<uint32_t> out;
            auto t0 = clk::now();
            walk_single(s, out);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            total_results += out.size();
        }
        size_t exact = 0;
        for (uint32_t s : range_q) {
            std::vector<uint32_t> out;
            walk_single(s, out);
            std::vector<uint32_t> exp = range_gt[s];
            std::sort(out.begin(), out.end());
            std::sort(exp.begin(), exp.end());
            if (out == exp) exact++;
        }
        Metrics m;
        m.name = "native_single_hop";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * exact / range_q.size();
        m.avg_results = total_results / range_q.size();
        results.push_back(m);
        std::cout << "  [native_single_hop] exact " << exact << "/" << range_q.size()
                  << ", avg " << m.avg_results << std::endl;
    }

    // ---- native multi-hop range (same mh_q) ----
    {
        for (size_t i = 0; i < N_WARMUP && i < mh_q.size(); i++) {
            std::vector<uint32_t> tmp;
            walk_multi(mh_q[i].first, mh_q[i].second, tmp);
        }
        std::vector<double> lat;
        lat.reserve(mh_q.size());
        double total_results = 0;
        for (auto& q : mh_q) {
            std::vector<uint32_t> out;
            auto t0 = clk::now();
            walk_multi(q.first, q.second, out);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            total_results += out.size();
        }
        size_t exact = 0;
        for (auto& q : mh_q) {
            std::vector<uint32_t> out;
            walk_multi(q.first, q.second, out);
            std::vector<uint32_t> exp = mh_gt[pack2(q.first, q.second)];
            std::sort(out.begin(), out.end());
            std::sort(exp.begin(), exp.end());
            if (out == exp) exact++;
        }
        Metrics m;
        m.name = "native_multi_hop";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * exact / mh_q.size();
        m.avg_results = total_results / mh_q.size();
        results.push_back(m);
        std::cout << "  [native_multi_hop] exact " << exact << "/" << mh_q.size() << ", avg "
                  << m.avg_results << std::endl;
    }

    // ---- native point query (same pos_q): traverse + check, no index ----
    {
        for (size_t i = 0; i < N_WARMUP && i < pos_q.size(); i++)
            point_exists(pos_q[i][0], pos_q[i][1], pos_q[i][2]);
        std::vector<double> lat;
        lat.reserve(pos_q.size());
        size_t found = 0;
        for (auto& q : pos_q) {
            auto t0 = clk::now();
            bool ok = point_exists(q[0], q[1], q[2]);
            auto t1 = clk::now();
            lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
            if (ok) found++;
        }
        Metrics m;
        m.name = "native_point";
        finalize_latency(lat, m);
        m.correct_pct = 100.0 * found / pos_q.size(); // positives -> should all be found
        m.avg_results = (double)found / pos_q.size();
        results.push_back(m);
        std::cout << "  [native_point] found " << found << "/" << pos_q.size() << std::endl;
    }

    db->commit_transaction();

    // per-row build-time / index-size selectors (DualIndex vs B+-tree vs native)
    auto row_build = [&](const std::string& nm) -> double {
        if (nm.rfind("btree", 0) == 0) return btree_build_ms;
        if (nm.rfind("native", 0) == 0) return 0.0; // pointer-chasing: no build, reuses graph
        return build_time_ms;
    };
    auto row_size = [&](const std::string& nm) -> size_t {
        if (nm.rfind("btree", 0) == 0) return btree_size_bytes;
        if (nm.rfind("native", 0) == 0) return 0;
        return index_size_bytes;
    };

    // -------------------- output --------------------
    std::error_code ec;
    std::filesystem::create_directories(RESULTS_DIR, ec);

    // per-dataset CSV (11 columns, as in prior versions)
    {
        std::ofstream csv(csv_path);
        csv << "query_type,num_queries,mean_us,p50_us,p95_us,p99_us,throughput_qps,"
               "correct_pct,build_time_ms,index_size_bytes,avg_results\n";
        csv.setf(std::ios::fixed);
        for (auto& m : results) {
            csv << m.name << "," << m.n << "," << m.mean_us << "," << m.p50 << "," << m.p95
                << "," << m.p99 << "," << m.throughput_qps << "," << m.correct_pct << ","
                << row_build(m.name) << "," << row_size(m.name) << "," << m.avg_results << "\n";
        }
    }

    // unified CSV (append; header written once), adds dataset + num_triples columns
    {
        bool exists = std::filesystem::exists(UNIFIED_CSV);
        std::ofstream u(UNIFIED_CSV, std::ios::app);
        u.setf(std::ios::fixed);
        if (!exists) {
            u << "dataset,query_type,num_queries,mean_us,p50_us,p95_us,p99_us,"
                 "throughput_qps,correct_pct,build_time_ms,index_size_bytes,avg_results,"
                 "num_triples\n";
        }
        for (auto& m : results) {
            u << dataset_name << "," << m.name << "," << m.n << "," << m.mean_us << ","
              << m.p50 << "," << m.p95 << "," << m.p99 << "," << m.throughput_qps << ","
              << m.correct_pct << "," << row_build(m.name) << "," << row_size(m.name) << ","
              << m.avg_results << "," << num_triples << "\n";
        }
    }

    std::cout << "\n================== SUMMARY (" << dataset_name << ") =================="
              << std::endl;
    std::cout << "nodes=" << n_nodes << " edges=" << n_edges << " triples=" << num_triples
              << "  build_time_ms=" << build_time_ms << "  index_size_bytes="
              << index_size_bytes << std::endl;
    printf("%-16s %9s %10s %10s %10s %10s %14s %10s %12s\n", "query_type", "n", "mean_us",
           "p50_us", "p95_us", "p99_us", "qps", "correct%", "avg_results");
    for (auto& m : results) {
        printf("%-16s %9zu %10.3f %10.3f %10.3f %10.3f %14.1f %10.3f %12.4f\n", m.name.c_str(),
               m.n, m.mean_us, m.p50, m.p95, m.p99, m.throughput_qps, m.correct_pct,
               m.avg_results);
    }
    std::cout << "\nper-dataset CSV  : " << csv_path << std::endl;
    std::cout << "unified CSV (app): " << UNIFIED_CSV << std::endl;

    // -------------------- DualIndex vs Native comparison --------------------
    auto mean_of = [&](const std::string& nm) -> double {
        for (auto& m : results)
            if (m.name == nm) return m.mean_us;
        return 0.0;
    };
    double zm = mean_of("point"), bt = mean_of("btree_point"), npnt = mean_of("native_point");
    double fl_s = mean_of("single_hop"), nat_s = mean_of("native_single_hop");
    double fl_m = mean_of("multi_hop"), nat_m = mean_of("native_multi_hop");
    auto spd = [](double slow, double fast) { return fast > 0 ? slow / fast : 0.0; };

    std::cout << "\n=========== DualIndex vs Native (" << dataset_name
              << ", mean us) ===========" << std::endl;
    printf("%-12s %14s %14s %14s\n", "query", "DualIndex", "native", "speedup(D over N)");
    printf("%-12s %14.3f %14.3f %14.3fx   [vs B+-tree %.3fx]\n", "point", zm, npnt,
           spd(npnt, zm), spd(bt, zm));
    printf("%-12s %14.3f %14.3f %14.3fx\n", "single_hop", fl_s, nat_s, spd(nat_s, fl_s));
    printf("%-12s %14.3f %14.3f %14.3fx\n", "multi_hop", fl_m, nat_m, spd(nat_m, fl_m));
    printf("B+-tree build = %.3f ms (vs DualIndex build %.3f ms)\n", btree_build_ms,
           build_time_ms);
    std::cout << "(speedup > 1 means DualIndex is faster; < 1 means slower)" << std::endl;

    graph_pool::destroy(pool);
    std::cout << "\n=== Done (" << dataset_name << ") ===" << std::endl;
    return 0;
}
