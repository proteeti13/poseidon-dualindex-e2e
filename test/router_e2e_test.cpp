/*
 * End-to-end test for DualIndex's query() router, exercised through Poseidon's
 * graph_db::learned_query(). Loads a SNAP edge list natively, builds the learned
 * index, then issues point and range queries THROUGH THE ROUTER and verifies the
 * router dispatches to the same answer as the three direct methods.
 *
 * Usage: ./router_e2e_test [dataset_path]
 *   default dataset_path = /home/proteeti/RSMI/datasets/wiki-Vote.txt
 *
 * Does not modify any existing test/benchmark code.
 */

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <algorithm>
#include <limits>
#include <unordered_map>

#include "graph_db.hpp"
#include "graph_pool.hpp"
#include "defs.hpp"

static const offset_t MX = std::numeric_limits<offset_t>::max();

static std::vector<offset_t> sorted(std::vector<offset_t> v) {
    std::sort(v.begin(), v.end());
    return v;
}

int main(int argc, char** argv) {
    const std::string dataset_path =
        (argc >= 2) ? argv[1] : "/home/proteeti/RSMI/datasets/wiki-Vote.txt";
    const std::string test_path = PMDK_PATH("router_e2e");

    std::cout << "=== DualIndex router end-to-end test ===" << std::endl;
    std::cout << "    dataset = " << dataset_path << std::endl;

    std::ifstream in(dataset_path);
    if (!in) { std::cerr << "ERROR: cannot open " << dataset_path << std::endl; return 1; }

    std::vector<std::pair<long,long>> edges;
    {
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line[0] == '#') continue;
            std::istringstream ss(line);
            long a, b;
            if (ss >> a >> b) edges.emplace_back(a, b);
        }
    }
    std::cout << "Parsed " << edges.size() << " edges" << std::endl;

    auto pool = graph_pool::create(test_path);
    auto db = pool->create_graph("router_graph");

    std::unordered_map<long, offset_t> snap2pos;
    auto get_pid = [&](long raw) -> offset_t {
        auto it = snap2pos.find(raw);
        if (it != snap2pos.end()) return it->second;
        offset_t pid = db->import_node("Node", {});
        snap2pos.emplace(raw, pid);
        return pid;
    };
    for (auto& e : edges) {
        offset_t s = get_pid(e.first), t = get_pid(e.second);
        db->import_relationship(s, t, "EDGE", {});
    }
    std::cout << "Imported " << snap2pos.size() << " nodes" << std::endl;

    // build the learned index
    db->begin_transaction();
    db->build_learned_index();
    db->commit_transaction();

    // pick one real 2-hop path (src -> h1 -> h2) to use as a known-existing key
    offset_t S = UNKNOWN, H1 = UNKNOWN, H2 = UNKNOWN;
    db->begin_transaction();
    {
        auto& cv = db->get_nodes()->as_vec();
        offset_t maxn = cv.last_used() + 1;
        for (offset_t i = 0; i < maxn && S == UNKNOWN; i++) {
            if (!cv.is_used(i)) continue;
            auto& src = db->node_by_id(i);
            offset_t rid = src.from_rship_list;
            while (rid != UNKNOWN && S == UNKNOWN) {
                auto& r1 = db->rship_by_id(rid);
                offset_t h1 = r1.dest_node;
                auto& h1n = db->node_by_id(h1);
                offset_t rid2 = h1n.from_rship_list;
                if (rid2 != UNKNOWN) {
                    auto& r2 = db->rship_by_id(rid2);
                    S = i; H1 = h1; H2 = r2.dest_node;
                }
                rid = r1.next_src_rship;
            }
        }
    }
    db->commit_transaction();

    if (S == UNKNOWN) { std::cerr << "no 2-hop path found in graph" << std::endl; return 1; }
    std::cout << "Sample existing path: " << S << " -> " << H1 << " -> " << H2 << std::endl;

    int failures = 0;
    auto check = [&](const std::string& name, bool ok) {
        std::cout << "  [" << (ok ? "PASS" : "FAIL") << "] " << name << std::endl;
        if (!ok) failures++;
    };

    std::cout << "\n--- POINT query via router (all 3 dims pinned) ---" << std::endl;
    // existing path: router must return exactly what point_lookup says (1 hit)
    auto r_pt   = db->learned_query({S, H1, H2}, {S, H1, H2});
    auto d_pt   = db->learned_point_lookup(S, H1, H2);
    check("router routes full-pinned box to point_lookup (existing -> 1 result)",
          r_pt.size() == 1 && d_pt != UNKNOWN);
    check("router point result matches direct point_lookup value",
          r_pt.size() == 1 && r_pt[0] == d_pt);

    // non-existing path: H2 replaced with a node that is not a 2nd hop of (S,H1)
    offset_t BAD = (H2 == 0 ? MX - 1 : 0);
    auto r_neg  = db->learned_query({S, H1, BAD}, {S, H1, BAD});
    auto d_neg  = db->learned_point_lookup(S, H1, BAD);
    check("router routes negative point to empty result",
          r_neg.empty() && d_neg == UNKNOWN);

    std::cout << "\n--- RANGE query via router (only src pinned) ---" << std::endl;
    auto r_rng  = db->learned_query({S, 0, 0}, {S, MX, MX});
    auto d_rng  = db->learned_range_query(S);
    check("router single-hop result count matches direct range_query",
          r_rng.size() == d_rng.size());
    check("router single-hop result set matches direct range_query",
          sorted(r_rng) == sorted(d_rng));
    std::cout << "    (src " << S << " -> " << r_rng.size() << " single-hop results)" << std::endl;

    std::cout << "\n--- MULTI-HOP query via router (src + hop1 pinned) ---" << std::endl;
    auto r_mh   = db->learned_query({S, H1, 0}, {S, H1, MX});
    auto d_mh   = db->learned_multi_hop_query(S, H1);
    check("router multi-hop result count matches direct multi_hop_query",
          r_mh.size() == d_mh.size());
    check("router multi-hop result set matches direct multi_hop_query",
          sorted(r_mh) == sorted(d_mh));
    check("multi-hop results are a subset of single-hop results (sanity)",
          r_mh.size() <= r_rng.size());
    std::cout << "    ((" << S << "," << H1 << ") -> " << r_mh.size()
              << " multi-hop results)" << std::endl;

    std::cout << "\n=== " << (failures == 0 ? "ALL ROUTER CHECKS PASSED" : "ROUTER TEST FAILED")
              << " (" << failures << " failures) ===" << std::endl;

    graph_pool::destroy(pool);
    return failures == 0 ? 0 : 1;
}
