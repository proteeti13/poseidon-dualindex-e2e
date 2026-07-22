/*
 * End-to-end benchmark: a SNAP edge list loaded *natively* into Poseidon,
 * 2-hop path triples extracted by graph_db::build_learned_index(), then the
 * DualIndex (ZM-Index point queries + FloodSourceSort range queries) is
 * exercised with sampled query workloads, following the SOSD / ALEX /
 * LearnedBench benchmark protocol:
 *
 *   - Workloads are generated ONCE (with SEED=42) into persisted binary files
 *     under <queries_dir>/<dataset>_<type>.bin (--gen_queries mode). Every
 *     record carries the ground-truth result count from the C++ walker.
 *   - Benchmark mode LOADS those files (and fails loudly if they are missing);
 *     the learned index and both native baselines (B+-tree, pointer-chasing)
 *     consume the exact same query stream.
 *   - Each timed phase runs N_WARMUP=500 untimed warm-up queries drawn from
 *     the persisted file, then times every query exactly once (no repeats).
 *   - CSV output follows the column order of results/zmindex_all_results_v3.csv
 *     and adds scan_overhead, dataset_fingerprint and (optionally) per-batch
 *     throughput rows. Each CSV carries a provenance comment header.
 *   - --perf uses perf_event_open() to count LLC-loads / LLC-load-misses /
 *     cycles / instructions around each timed phase (learned AND native).
 *
 * IMPORTANT: this file is the benchmark HARNESS only. It does not modify any
 * index or traversal logic. In particular --flood_k and --zm_epsilon are
 * provenance flags: the DualIndex is built from compile-time template params
 * (ZMIndex<3,64,false>, FloodSourceSort<3,4,64>), so these flags are echoed
 * for the record and a warning is printed if they differ from the compiled
 * configuration -- they cannot reconfigure the index without recompiling.
 *
 * Usage:
 *   Generate queries once:
 *     ./bench_dualindex --gen_queries --dataset_file <snap.txt> \
 *                       --dataset_name wiki_vote [--num_queries N] [--queries_dir queries]
 *   Run the benchmark (queries must already exist):
 *     ./bench_dualindex --dataset_file <snap.txt> --dataset_name wiki_vote \
 *                       [--num_queries N] [--queries_dir queries] \
 *                       [--flood_k 4] [--zm_epsilon 64] [--batch_size 10000] \
 *                       [--print_batch_stats] [--perf] [--dualindex_repo <path>]
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
#include <functional>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cstdio>

#if defined(__linux__)
#include <linux/perf_event.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#endif

#include "graph_db.hpp"
#include "graph_pool.hpp"
#include "defs.hpp"

using clk = std::chrono::high_resolution_clock;

static const std::string RESULTS_DIR = "results";
static const std::string UNIFIED_CSV = "results/poseidon_e2e_all_snap.csv";
static const unsigned SEED = 42;
// WARM-UP POLICY: before timing a workload we run this many untimed warm-up
// queries drawn from the head of the persisted file. They are NOT counted in
// any latency/throughput/correctness result. After warm-up every query in the
// file is timed exactly once (no repeats).
static const size_t N_WARMUP = 500;

// compiled-in DualIndex configuration (template params in dual_index.hpp).
// --flood_k / --zm_epsilon are compared against these for provenance.
static const int COMPILED_FLOOD_K = 4;
static const int COMPILED_ZM_EPSILON = 64;

// compile-time provenance for the metadata header.
static const char* COMPILE_FLAGS = "-mbmi2 -O3 -DNDEBUG";
static const char* DEFAULT_DUALINDEX_REPO = "/home/proteeti/DualIndex";

// pack three node ids (< 2^21) into one 64-bit key
static inline uint64_t pack(uint32_t s, uint32_t h1, uint32_t h2) {
    return (uint64_t(s) << 42) | (uint64_t(h1) << 21) | uint64_t(h2);
}
// pack a (src, hop1) pair into one 64-bit key (for multi-hop ground truth)
static inline uint64_t pack2(uint32_t s, uint32_t h1) {
    return (uint64_t(s) << 21) | uint64_t(h1);
}

// ============================ CLI parsing ============================
struct Config {
    bool gen_queries = false;
    std::string dataset_file;
    std::string dataset_name;
    std::string queries_dir = "queries";
    size_t num_queries = 100000;
    int flood_k = COMPILED_FLOOD_K;
    int zm_epsilon = COMPILED_ZM_EPSILON;
    size_t batch_size = 10000;
    bool print_batch_stats = false;
    bool perf = false;
    bool measure_scan_overhead = false;
    std::string dualindex_repo = DEFAULT_DUALINDEX_REPO;
};

static void usage(const char* argv0) {
    std::cerr <<
        "usage: " << argv0 << " [--gen_queries] --dataset_file <path> --dataset_name <name>\n"
        "         [--queries_dir <dir>=queries] [--num_queries <n>=100000]\n"
        "         [--flood_k <n>=4] [--zm_epsilon <n>=64] [--batch_size <n>=10000]\n"
        "         [--print_batch_stats] [--perf] [--dualindex_repo <path>]\n"
        "         [--measure_scan_overhead]   (SCAN_DEBUG build only: untimed pass that\n"
        "                                      writes queries/<name>_scan_overhead.txt)\n";
}

static bool parse_args(int argc, char** argv, Config& c) {
    auto need = [&](int& i) -> const char* {
        if (i + 1 >= argc) { std::cerr << "ERROR: " << argv[i] << " needs a value\n"; return nullptr; }
        return argv[++i];
    };
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--gen_queries") c.gen_queries = true;
        else if (a == "--print_batch_stats") c.print_batch_stats = true;
        else if (a == "--perf") c.perf = true;
        else if (a == "--measure_scan_overhead") c.measure_scan_overhead = true;
        else if (a == "--dataset_file") { auto v = need(i); if (!v) return false; c.dataset_file = v; }
        else if (a == "--dataset_name") { auto v = need(i); if (!v) return false; c.dataset_name = v; }
        else if (a == "--queries_dir") { auto v = need(i); if (!v) return false; c.queries_dir = v; }
        else if (a == "--num_queries") { auto v = need(i); if (!v) return false; c.num_queries = std::stoull(v); }
        else if (a == "--flood_k") { auto v = need(i); if (!v) return false; c.flood_k = std::stoi(v); }
        else if (a == "--zm_epsilon") { auto v = need(i); if (!v) return false; c.zm_epsilon = std::stoi(v); }
        else if (a == "--batch_size") { auto v = need(i); if (!v) return false; c.batch_size = std::stoull(v); }
        else if (a == "--dualindex_repo") { auto v = need(i); if (!v) return false; c.dualindex_repo = v; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); return false; }
        else { std::cerr << "ERROR: unknown flag " << a << "\n"; usage(argv[0]); return false; }
    }
    if (c.dataset_file.empty() || c.dataset_name.empty()) {
        std::cerr << "ERROR: --dataset_file and --dataset_name are required\n";
        usage(argv[0]);
        return false;
    }
    return true;
}

// ============================ query persistence ============================
// Binary layout (little-endian, native):
//   [Header][record][record]...
// where record is 4x uint32 for point workloads (src,hop1,hop2,gt_count),
// 2x uint32 for single_hop (src,gt_count) and 3x uint32 for multi_hop
// (src,hop1,gt_count). gt_count is the ground-truth result count from the C++
// 2-hop walker: for point workloads it is 1 (present) or 0 (absent).
enum QType : uint32_t { QT_POINT_POS = 0, QT_POINT_NEG = 1, QT_SINGLE_HOP = 2, QT_MULTI_HOP = 3 };

struct QHeader {
    char     magic[4];      // "PQBF"
    uint32_t version;       // 1
    uint32_t type;          // QType
    uint32_t seed;          // SEED
    uint32_t rec_count;     // number of records
    uint32_t rec_stride;    // bytes per record
    uint64_t triple_count;  // dataset fingerprint part 1
    uint64_t xor_checksum;  // dataset fingerprint part 2
};
static const uint32_t QBF_VERSION = 1;

static const char* qtype_name(uint32_t t) {
    switch (t) {
        case QT_POINT_POS:  return "point_pos";
        case QT_POINT_NEG:  return "point_neg";
        case QT_SINGLE_HOP: return "single_hop";
        case QT_MULTI_HOP:  return "multi_hop";
        default:            return "unknown";
    }
}
static std::string qfile_path(const Config& c, uint32_t t) {
    return c.queries_dir + "/" + c.dataset_name + "_" + qtype_name(t) + ".bin";
}
// sidecar written by the untimed --measure_scan_overhead pass and read back by
// the timed benchmark to populate the scan_overhead column (measured, not
// analytic). Lives in queries/ so the "wipe only dualindex_<ds>/" rule keeps it.
static std::string scan_overhead_path(const Config& c) {
    return c.queries_dir + "/" + c.dataset_name + "_scan_overhead.txt";
}

// dataset fingerprint: (triple count, XOR-fold checksum over all packed triples)
struct Fingerprint { uint64_t triple_count; uint64_t xor_checksum; };
static Fingerprint compute_fingerprint(const std::vector<std::array<uint32_t,3>>& triples) {
    Fingerprint fp{triples.size(), 0};
    for (auto& t : triples) fp.xor_checksum ^= pack(t[0], t[1], t[2]);
    return fp;
}

// generic writer: rec_words uint32 words per record, packed contiguously.
static bool write_query_file(const std::string& path, uint32_t type,
                             const std::vector<uint32_t>& words, uint32_t rec_words,
                             const Fingerprint& fp) {
    std::ofstream f(path, std::ios::binary);
    if (!f) { std::cerr << "ERROR: cannot write " << path << std::endl; return false; }
    QHeader h;
    std::memcpy(h.magic, "PQBF", 4);
    h.version = QBF_VERSION;
    h.type = type;
    h.seed = SEED;
    h.rec_count = (uint32_t)(words.size() / rec_words);
    h.rec_stride = rec_words * (uint32_t)sizeof(uint32_t);
    h.triple_count = fp.triple_count;
    h.xor_checksum = fp.xor_checksum;
    f.write(reinterpret_cast<const char*>(&h), sizeof(h));
    f.write(reinterpret_cast<const char*>(words.data()), words.size() * sizeof(uint32_t));
    return (bool)f;
}

// generic reader: validates magic/type/stride and the dataset fingerprint.
static bool read_query_file(const std::string& path, uint32_t expect_type, uint32_t rec_words,
                            const Fingerprint& fp, std::vector<uint32_t>& words) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "ERROR: query file missing: " << path << "\n"
                  << "       run with --gen_queries first." << std::endl;
        return false;
    }
    QHeader h;
    f.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!f || std::memcmp(h.magic, "PQBF", 4) != 0) {
        std::cerr << "ERROR: bad magic in " << path << std::endl; return false;
    }
    if (h.version != QBF_VERSION || h.type != expect_type ||
        h.rec_stride != rec_words * sizeof(uint32_t)) {
        std::cerr << "ERROR: header mismatch in " << path << " (type/version/stride)" << std::endl;
        return false;
    }
    if (h.triple_count != fp.triple_count || h.xor_checksum != fp.xor_checksum) {
        std::cerr << "ERROR: dataset fingerprint mismatch for " << path << "\n"
                  << "       queries were generated for a different graph; re-run --gen_queries.\n"
                  << "       file(tc=" << h.triple_count << ",xor=" << h.xor_checksum
                  << ") vs dataset(tc=" << fp.triple_count << ",xor=" << fp.xor_checksum << ")"
                  << std::endl;
        return false;
    }
    words.resize((size_t)h.rec_count * rec_words);
    f.read(reinterpret_cast<char*>(words.data()), words.size() * sizeof(uint32_t));
    return (bool)f;
}

// ============================ latency stats ============================
struct Metrics {
    std::string name;
    size_t n = 0;
    double mean_us = 0, p50 = 0, p95 = 0, p99 = 0;
    double throughput_qps = 0;
    double correct_pct = 0;
    double avg_results = 0;
    double scan_overhead = 0;   // entries examined / entries returned (range workloads)
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

// per-batch throughput row (emitted when --print_batch_stats)
struct BatchStat {
    std::string workload;
    size_t batch_index;
    size_t batch_n;
    double batch_throughput_qps;
    double batch_mean_us;
};

// ============================ perf counters ============================
// perf_event_open wrapper. Counts LLC-loads, LLC-load-misses, cycles and
// instructions for a timed region (build + warm-up excluded). Degrades
// gracefully: if the kernel denies perf_event_open (perf_event_paranoid),
// counters read as -1 and the benchmark continues.
struct PerfSample {
    long long llc_loads = -1, llc_misses = -1, cycles = -1, instructions = -1;
    bool ok = false;
};

class PerfGroup {
#if defined(__linux__)
    int fds[4];
    static long perf_open(uint32_t type, uint64_t config, int group) {
        perf_event_attr attr{};
        attr.type = type;
        attr.size = sizeof(attr);
        attr.config = config;
        attr.disabled = (group == -1) ? 1 : 0;
        attr.exclude_kernel = 1;
        attr.exclude_hv = 1;
        return syscall(__NR_perf_event_open, &attr, 0, -1, group, 0);
    }
public:
    bool enabled = false;
    PerfGroup() { for (int i = 0; i < 4; i++) fds[i] = -1; }
    bool open() {
        fds[0] = perf_open(PERF_TYPE_HW_CACHE,
            (PERF_COUNT_HW_CACHE_LL) | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
            (PERF_COUNT_HW_CACHE_RESULT_ACCESS << 16), -1);
        if (fds[0] < 0) return false;
        fds[1] = perf_open(PERF_TYPE_HW_CACHE,
            (PERF_COUNT_HW_CACHE_LL) | (PERF_COUNT_HW_CACHE_OP_READ << 8) |
            (PERF_COUNT_HW_CACHE_RESULT_MISS << 16), (int)fds[0]);
        fds[2] = perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES, (int)fds[0]);
        fds[3] = perf_open(PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS, (int)fds[0]);
        enabled = true;
        return true;
    }
    void start() {
        if (!enabled) return;
        ioctl(fds[0], PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP);
        ioctl(fds[0], PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP);
    }
    void stop(PerfSample& s) {
        if (!enabled) return;
        ioctl(fds[0], PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP);
        long long v;
        auto rd = [&](int fd) -> long long {
            long long out = -1;
            if (fd >= 0 && read(fd, &v, sizeof(v)) == sizeof(v)) out = v;
            return out;
        };
        s.llc_loads = rd(fds[0]);
        s.llc_misses = rd(fds[1]);
        s.cycles = rd(fds[2]);
        s.instructions = rd(fds[3]);
        s.ok = true;
    }
    ~PerfGroup() { for (int i = 0; i < 4; i++) if (fds[i] >= 0) close(fds[i]); }
#else
public:
    bool enabled = false;
    bool open() { return false; }
    void start() {}
    void stop(PerfSample&) {}
#endif
};

// A timed region wrapper: measures wall time and (optionally) perf counters
// around the loop body, and records bytes-touched for a bandwidth estimate.
struct PhasePerf {
    std::string workload;
    PerfSample sample;
    double elapsed_s = 0;
    double bandwidth_gbps = -1;  // bytes-touched / elapsed, where measurable
    bool measured = false;
};

// ============================ metadata header ============================
static std::string run_cmd(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return "unknown";
    char buf[256];
    while (fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out.empty() ? "unknown" : out;
}
static std::string cpu_model() {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("model name", 0) == 0) {
            auto pos = line.find(':');
            if (pos != std::string::npos) {
                std::string v = line.substr(pos + 1);
                size_t s = v.find_first_not_of(" \t");
                return s == std::string::npos ? "unknown" : v.substr(s);
            }
        }
    }
    return "unknown";
}
static std::string iso_timestamp() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", std::localtime(&t));
    return buf;
}
struct RunMeta {
    std::string cpu, compiler, flags, timestamp, git_repo, git_dualindex;
};
static RunMeta gather_meta(const Config& c) {
    RunMeta m;
    m.cpu = cpu_model();
    m.compiler = __VERSION__;
    m.flags = COMPILE_FLAGS;
    m.timestamp = iso_timestamp();
    m.git_repo = run_cmd("git rev-parse HEAD 2>/dev/null");
    m.git_dualindex = run_cmd("git -C '" + c.dualindex_repo + "' rev-parse HEAD 2>/dev/null");
    return m;
}
// write the '#'-prefixed provenance comment block at the top of a CSV.
static void write_meta_header(std::ostream& os, const RunMeta& m, const Config& c,
                             bool note_scan_overhead = false) {
    os << "# poseidon DualIndex end-to-end benchmark\n";
    os << "# cpu_model=" << m.cpu << "\n";
    os << "# compiler=" << m.compiler << "\n";
    os << "# compile_flags=" << m.flags << "\n";
    os << "# timestamp=" << m.timestamp << "\n";
    os << "# git_poseidon_core=" << m.git_repo << "\n";
    os << "# git_dualindex=" << m.git_dualindex << "\n";
    os << "# flood_k=" << c.flood_k << " zm_epsilon=" << c.zm_epsilon
       << " (compiled: flood_k=" << COMPILED_FLOOD_K
       << " zm_epsilon=" << COMPILED_ZM_EPSILON << ")\n";
    if (note_scan_overhead) {
        os << "# scan_overhead=measured via instrumented untimed SCAN_DEBUG pass "
              "(FloodSourceSort cell-scan counter, entries examined / entries returned); "
              "populated for learned single_hop and multi_hop only, 0 = N/A. "
              "The timed benchmark itself is counter-free.\n";
    }
}

// known expected triple counts (for the +/-10% Phase 1 validation)
static double expected_triples(const std::string& name) {
    if (name == "wiki_vote")  return 4542805.0;
    if (name == "roadnet_ca") return 17523394.0;
    if (name == "web_google") return 60687836.0;
    return -1.0; // unknown -> skip validation
}

// ==========================================================================
int main(int argc, char** argv) {
    Config cfg;
    if (!parse_args(argc, argv, cfg)) return 2;

    const std::string& dataset_path = cfg.dataset_file;
    const std::string& dataset_name = cfg.dataset_name;
    const size_t NQ = cfg.num_queries;
    const size_t N_POINT_POS = NQ;
    const size_t N_POINT_NEG = std::max<size_t>(1, NQ / 10);
    const size_t N_RANGE = NQ;

    if (cfg.flood_k != COMPILED_FLOOD_K || cfg.zm_epsilon != COMPILED_ZM_EPSILON) {
        std::cerr << "WARNING: --flood_k/--zm_epsilon (" << cfg.flood_k << "/" << cfg.zm_epsilon
                  << ") differ from the compiled DualIndex config ("
                  << COMPILED_FLOOD_K << "/" << COMPILED_ZM_EPSILON << ").\n"
                  << "         These flags are provenance-only; the index is built from\n"
                  << "         compile-time template params and will NOT be reconfigured.\n";
    }

    const std::string csv_path = "results/poseidon_e2e_" + dataset_name + ".csv";
    const std::string batches_csv_path = "results/poseidon_e2e_" + dataset_name + "_batches.csv";
    const std::string perf_csv_path = "results/perf_" + dataset_name + ".csv";
    const std::string test_path = PMDK_PATH(std::string("dualindex_") + dataset_name);

    std::cout << "=== Poseidon x DualIndex End-to-End: " << dataset_name << " ==="
              << std::endl;
    std::cout << "    mode         = " << (cfg.gen_queries ? "GEN_QUERIES" : "BENCHMARK") << std::endl;
    std::cout << "    dataset_file = " << dataset_path << std::endl;
    std::cout << "    dataset_name = " << dataset_name << std::endl;
    std::cout << "    queries_dir  = " << cfg.queries_dir << std::endl;
    std::cout << "    num_queries  = " << NQ << " (neg " << N_POINT_NEG << ")" << std::endl;
    std::cout << "    flood_k      = " << cfg.flood_k << "  zm_epsilon = " << cfg.zm_epsilon
              << "  batch_size = " << cfg.batch_size << std::endl;
    std::cout << "    print_batch_stats = " << (cfg.print_batch_stats ? "yes" : "no")
              << "  perf = " << (cfg.perf ? "yes" : "no") << std::endl;

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

    // map raw SNAP id -> contiguous Poseidon id, importing each node once.
    // NOTE: import order is deterministic (edge-file order), so the internal
    // ids produced here are identical between --gen_queries and benchmark runs;
    // the dataset fingerprint (below) guards against any drift.
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

    if (num_triples == 0) {
        std::cerr << "FATAL: 0 triples extracted -- graph not loaded correctly. Aborting."
                  << std::endl;
        graph_pool::destroy(pool);
        return 1;
    }

    // dataset fingerprint (used to bind query files to this exact graph)
    Fingerprint fp = compute_fingerprint(triples);
    std::cout << "Dataset fingerprint: triple_count=" << fp.triple_count
              << " xor_checksum=" << fp.xor_checksum << std::endl;

    // membership set for negative-query generation / point ground truth
    std::unordered_set<uint64_t> triple_set;
    triple_set.reserve(num_triples * 2);
    for (auto& t : triples) triple_set.insert(pack(t[0], t[1], t[2]));

    // =====================================================================
    // GEN_QUERIES MODE: generate the four workloads once (SEED=42), each
    // record annotated with its ground-truth result count, then exit.
    // =====================================================================
    if (cfg.gen_queries) {
        std::error_code ec;
        std::filesystem::create_directories(cfg.queries_dir, ec);
        std::cout << "\n[gen_queries] seed=" << SEED << " -> " << cfg.queries_dir << "/"
                  << std::endl;
        std::mt19937 rng(SEED);

        // point positives: sampled from extracted triples (gt_count always 1)
        {
            std::vector<uint32_t> words; words.reserve(N_POINT_POS * 4);
            std::uniform_int_distribution<size_t> pick(0, triples.size() - 1);
            for (size_t i = 0; i < N_POINT_POS; i++) {
                auto& t = triples[pick(rng)];
                words.push_back(t[0]); words.push_back(t[1]); words.push_back(t[2]);
                words.push_back(1u); // present
            }
            if (!write_query_file(qfile_path(cfg, QT_POINT_POS), QT_POINT_POS, words, 4, fp)) return 1;
            std::cout << "  wrote point_pos  (" << N_POINT_POS << ")" << std::endl;
        }
        // point negatives: random triples not present (gt_count always 0)
        {
            std::vector<uint32_t> words; words.reserve(N_POINT_NEG * 4);
            std::uniform_int_distribution<uint32_t> nid(0, (uint32_t)n_nodes - 1);
            size_t made = 0;
            while (made < N_POINT_NEG) {
                uint32_t s = nid(rng), h1 = nid(rng), h2 = nid(rng);
                if (triple_set.find(pack(s, h1, h2)) == triple_set.end()) {
                    words.push_back(s); words.push_back(h1); words.push_back(h2);
                    words.push_back(0u); // absent
                    made++;
                }
            }
            if (!write_query_file(qfile_path(cfg, QT_POINT_NEG), QT_POINT_NEG, words, 4, fp)) return 1;
            std::cout << "  wrote point_neg  (" << N_POINT_NEG << ")" << std::endl;
        }
        // single_hop: sources sampled uniformly over distinct sources;
        // gt_count = number of 2-hop results for that source.
        {
            std::vector<uint32_t> sources; sources.reserve(range_gt.size());
            for (auto& kv : range_gt) sources.push_back(kv.first);
            std::sort(sources.begin(), sources.end()); // deterministic order pre-sampling
            std::vector<uint32_t> words; words.reserve(N_RANGE * 2);
            std::uniform_int_distribution<size_t> pick(0, sources.size() - 1);
            for (size_t i = 0; i < N_RANGE; i++) {
                uint32_t s = sources[pick(rng)];
                words.push_back(s);
                words.push_back((uint32_t)range_gt[s].size());
            }
            if (!write_query_file(qfile_path(cfg, QT_SINGLE_HOP), QT_SINGLE_HOP, words, 2, fp)) return 1;
            std::cout << "  wrote single_hop (" << N_RANGE << ", distinct sources="
                      << sources.size() << ")" << std::endl;
        }
        // multi_hop: (src,hop1) pairs sampled from existing triples;
        // gt_count = number of hop2 for that (src,hop1).
        {
            std::vector<uint32_t> words; words.reserve(N_RANGE * 3);
            std::uniform_int_distribution<size_t> pick(0, triples.size() - 1);
            for (size_t i = 0; i < N_RANGE; i++) {
                auto& t = triples[pick(rng)];
                words.push_back(t[0]); words.push_back(t[1]);
                words.push_back((uint32_t)mh_gt[pack2(t[0], t[1])].size());
            }
            if (!write_query_file(qfile_path(cfg, QT_MULTI_HOP), QT_MULTI_HOP, words, 3, fp)) return 1;
            std::cout << "  wrote multi_hop  (" << N_RANGE << ")" << std::endl;
        }
        std::cout << "\n[gen_queries] done. Re-run without --gen_queries to benchmark."
                  << std::endl;
        graph_pool::destroy(pool);
        return 0;
    }

    // =====================================================================
    // BENCHMARK MODE: load the persisted workloads (fail if missing) and run.
    // =====================================================================
    std::cout << "\n[Phase 2] Loading persisted workloads from " << cfg.queries_dir << "/ ..."
              << std::endl;

    std::vector<uint32_t> w_pos, w_neg, w_single, w_multi;
    if (!read_query_file(qfile_path(cfg, QT_POINT_POS), QT_POINT_POS, 4, fp, w_pos)) return 1;
    if (!read_query_file(qfile_path(cfg, QT_POINT_NEG), QT_POINT_NEG, 4, fp, w_neg)) return 1;
    if (!read_query_file(qfile_path(cfg, QT_SINGLE_HOP), QT_SINGLE_HOP, 2, fp, w_single)) return 1;
    if (!read_query_file(qfile_path(cfg, QT_MULTI_HOP), QT_MULTI_HOP, 3, fp, w_multi)) return 1;

    // unpack into typed query vectors (+ stored ground-truth counts)
    std::vector<std::array<uint32_t, 3>> pos_q, neg_q;
    std::vector<uint32_t> pos_gt, neg_gt;
    for (size_t i = 0; i + 3 < w_pos.size(); i += 4) {
        pos_q.push_back({w_pos[i], w_pos[i+1], w_pos[i+2]});
        pos_gt.push_back(w_pos[i+3]);
    }
    for (size_t i = 0; i + 3 < w_neg.size(); i += 4) {
        neg_q.push_back({w_neg[i], w_neg[i+1], w_neg[i+2]});
        neg_gt.push_back(w_neg[i+3]);
    }
    std::vector<uint32_t> range_q, range_gtc;
    for (size_t i = 0; i + 1 < w_single.size(); i += 2) {
        range_q.push_back(w_single[i]);
        range_gtc.push_back(w_single[i+1]);
    }
    std::vector<std::pair<uint32_t, uint32_t>> mh_q;
    std::vector<uint32_t> mh_gtc;
    for (size_t i = 0; i + 2 < w_multi.size(); i += 3) {
        mh_q.emplace_back(w_multi[i], w_multi[i+1]);
        mh_gtc.push_back(w_multi[i+2]);
    }
    std::cout << "  point(+)=" << pos_q.size() << " point(-)=" << neg_q.size()
              << " single_hop=" << range_q.size() << " multi_hop=" << mh_q.size() << std::endl;

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
    double EXPECTED = expected_triples(dataset_name);
    if (EXPECTED > 0) {
        double dev = std::abs((double)num_triples - EXPECTED) / EXPECTED;
        std::cout << "  expected ~" << (long)EXPECTED << " (deviation " << (dev * 100.0)
                  << "%) -> " << (dev <= 0.10 ? "PASS" : "WARN (outside 10%)") << std::endl;
    } else {
        std::cout << "  expected ~ (unknown dataset; validation skipped)" << std::endl;
    }

    // ---- untimed measured scan_overhead pass (--measure_scan_overhead) ----
    // Option 1: a dedicated instrumented pass measures the TRUE scan_overhead
    // (entries examined inside FloodSourceSort::Bucket::search / entries
    // returned) for single_hop and multi_hop, writes it to a sidecar file, and
    // exits WITHOUT running any timed phase. The timed benchmark (built without
    // SCAN_DEBUG, so counter-free) later reads the sidecar. The analytic
    // formula was verified correct for single_hop (1.0) but wrong for multi_hop
    // (FloodSourceSort's hop1 grid prunes the src partition), so it is not used.
    if (cfg.measure_scan_overhead) {
#ifdef SCAN_DEBUG
        std::cout << "\n[scan_overhead] untimed instrumented pass over "
                  << range_q.size() << " single_hop + " << mh_q.size()
                  << " multi_hop queries ..." << std::endl;
        double s_ex = 0, s_ret = 0;
        for (uint32_t s : range_q) {
            bench::index::g_scan_examined = 0;
            auto res = db->learned_range_query(s);
            s_ex += bench::index::g_scan_examined;
            s_ret += res.size();
        }
        double m_ex = 0, m_ret = 0;
        for (auto& q : mh_q) {
            bench::index::g_scan_examined = 0;
            auto res = db->learned_multi_hop_query(q.first, q.second);
            m_ex += bench::index::g_scan_examined;
            m_ret += res.size();
        }
        double s_ovh = s_ret > 0 ? s_ex / s_ret : 0.0;
        double m_ovh = m_ret > 0 ? m_ex / m_ret : 0.0;

        std::error_code ec2;
        std::filesystem::create_directories(cfg.queries_dir, ec2);
        std::ofstream so(scan_overhead_path(cfg));
        so.setf(std::ios::fixed);
        so << "single_hop " << s_ovh << "\n";
        so << "multi_hop " << m_ovh << "\n";
        so.close();

        printf("  %-12s %14s %16s\n", "workload", "measured_ovh", "examined_total");
        printf("  %-12s %14.4f %16.0f\n", "single_hop", s_ovh, s_ex);
        printf("  %-12s %14.4f %16.0f\n", "multi_hop", m_ovh, m_ex);
        std::cout << "  wrote " << scan_overhead_path(cfg) << std::endl;
        graph_pool::destroy(pool);
        return 0;
#else
        std::cerr << "ERROR: --measure_scan_overhead requires a build with -DSCAN_DEBUG "
                     "(the instrumented FloodSourceSort counter). This binary is counter-free.\n";
        graph_pool::destroy(pool);
        return 1;
#endif
    }

    std::vector<Metrics> results;
    std::vector<BatchStat> batch_stats;
    std::vector<PhasePerf> perf_phases;

    // set up perf once (shared group re-armed per phase)
    PerfGroup perf;
    if (cfg.perf) {
        if (!perf.open()) {
            std::cerr << "WARNING: --perf requested but perf_event_open() failed (errno="
#if defined(__linux__)
                      << errno
#endif
                      << "). Continuing without hardware counters. Try lowering "
                         "/proc/sys/kernel/perf_event_paranoid." << std::endl;
        }
    }

    // Generic timed loop over a workload. `body(i)` performs ONE query and
    // returns its result count. Warm-up (first N_WARMUP) is untimed; every
    // remaining query is timed exactly once. Batch throughput captured when
    // requested. Perf counters wrap only the timed region.
    auto run_phase = [&](const std::string& name, size_t count,
                         double bytes_per_result,
                         const std::function<size_t(size_t)>& warm,
                         const std::function<size_t(size_t, double&)>& body,
                         Metrics& m, double* out_total_results) {
        for (size_t i = 0; i < N_WARMUP && i < count; i++) warm(i);

        std::vector<double> lat; lat.reserve(count);
        double total_results = 0;
        size_t batch_n = 0; double batch_sum_us = 0; size_t batch_idx = 0;

        PerfSample ps;
        perf.start();
        auto phase_t0 = clk::now();
        for (size_t i = 0; i < count; i++) {
            double us = 0;
            size_t rc = body(i, us);
            lat.push_back(us);
            total_results += rc;
            batch_n++; batch_sum_us += us;
            if (cfg.print_batch_stats && batch_n == cfg.batch_size) {
                double qps = batch_sum_us > 0 ? batch_n / (batch_sum_us / 1e6) : 0.0;
                batch_stats.push_back({name, batch_idx++, batch_n, qps, batch_sum_us / batch_n});
                batch_n = 0; batch_sum_us = 0;
            }
        }
        auto phase_t1 = clk::now();
        perf.stop(ps);
        if (cfg.print_batch_stats && batch_n > 0) {
            double qps = batch_sum_us > 0 ? batch_n / (batch_sum_us / 1e6) : 0.0;
            batch_stats.push_back({name, batch_idx++, batch_n, qps, batch_sum_us / batch_n});
        }

        m.name = name;
        finalize_latency(lat, m);
        if (out_total_results) *out_total_results = total_results;

        if (cfg.perf) {
            PhasePerf pp;
            pp.workload = name;
            pp.sample = ps;
            pp.elapsed_s = std::chrono::duration<double>(phase_t1 - phase_t0).count();
            // bytes-touched estimate where measurable: results x record size.
            if (bytes_per_result > 0 && pp.elapsed_s > 0)
                pp.bandwidth_gbps = (total_results * bytes_per_result) / pp.elapsed_s / 1e9;
            pp.measured = ps.ok;
            perf_phases.push_back(pp);
        }
        return total_results;
    };

    // ---- DualIndex answer dumps (for the Python oracle cross-check) ----
    // Per-query result counts in the SAME order as the persisted files.
    std::vector<uint32_t> ans_pos, ans_neg, ans_single, ans_multi;
    ans_pos.reserve(pos_q.size());  ans_neg.reserve(neg_q.size());
    ans_single.reserve(range_q.size()); ans_multi.reserve(mh_q.size());

    // ==================== DualIndex workloads ====================
    // NOTE: index/traversal calls below are UNCHANGED from the original bench.

    // ---- point queries (positive) ----
    {
        Metrics m;
        run_phase("point", pos_q.size(), 0,
            [&](size_t i){ return (size_t)(db->learned_point_lookup(pos_q[i][0], pos_q[i][1], pos_q[i][2]) != UNKNOWN); },
            [&](size_t i, double& us) -> size_t {
                auto t0 = clk::now();
                offset_t r = db->learned_point_lookup(pos_q[i][0], pos_q[i][1], pos_q[i][2]);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                size_t hit = (r != UNKNOWN) ? 1 : 0;
                ans_pos.push_back((uint32_t)hit);
                return hit;
            }, m, nullptr);
        size_t found = 0; for (auto v : ans_pos) found += v;
        m.correct_pct = 100.0 * found / pos_q.size();
        m.avg_results = (double)found / pos_q.size();
        results.push_back(m);
        std::cout << "  [point] found " << found << "/" << pos_q.size() << std::endl;
    }

    // ---- point queries (negative) ----
    {
        Metrics m;
        run_phase("point_negative", neg_q.size(), 0,
            [&](size_t){ return (size_t)0; }, // negatives share warm-up cost with positives
            [&](size_t i, double& us) -> size_t {
                auto t0 = clk::now();
                offset_t r = db->learned_point_lookup(neg_q[i][0], neg_q[i][1], neg_q[i][2]);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                size_t hit = (r != UNKNOWN) ? 1 : 0;
                ans_neg.push_back((uint32_t)hit);
                return hit;
            }, m, nullptr);
        size_t found = 0; for (auto v : ans_neg) found += v;
        m.correct_pct = 100.0 * (neg_q.size() - found) / neg_q.size();
        m.avg_results = (double)found / neg_q.size(); // false-positive rate
        results.push_back(m);
        std::cout << "  [point_negative] false positives " << found << "/" << neg_q.size()
                  << " (hit rate " << m.avg_results << ")" << std::endl;
    }

    // ---- single-hop range queries ----
    {
        Metrics m;
        double total_results = 0;
        run_phase("single_hop", range_q.size(), sizeof(uint32_t),
            [&](size_t i){ return db->learned_range_query(range_q[i]).size(); },
            [&](size_t i, double& us) -> size_t {
                auto t0 = clk::now();
                auto res = db->learned_range_query(range_q[i]);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                ans_single.push_back((uint32_t)res.size());
                return res.size();
            }, m, &total_results);
        // untimed correctness pass vs the ground truth we re-extracted here.
        // (scan_overhead is measured separately by the instrumented
        // --measure_scan_overhead pass and read from the sidecar below.)
        size_t exact = 0;
        for (uint32_t s : range_q) {
            auto res = db->learned_range_query(s);
            std::vector<uint32_t> got; got.reserve(res.size());
            for (offset_t v : res) got.push_back((uint32_t)v);
            std::vector<uint32_t> exp = range_gt[s];
            std::sort(got.begin(), got.end());
            std::sort(exp.begin(), exp.end());
            if (got == exp) exact++;
        }
        m.correct_pct = 100.0 * exact / range_q.size();
        m.avg_results = total_results / range_q.size();
        results.push_back(m);
        std::cout << "  [single_hop] exact " << exact << "/" << range_q.size()
                  << ", avg " << m.avg_results << std::endl;
    }

    // ---- multi-hop range queries: (src, hop1) pinned, hop2 open ----
    {
        Metrics m;
        double total_results = 0;
        run_phase("multi_hop", mh_q.size(), sizeof(uint32_t),
            [&](size_t i){ return db->learned_multi_hop_query(mh_q[i].first, mh_q[i].second).size(); },
            [&](size_t i, double& us) -> size_t {
                auto t0 = clk::now();
                auto res = db->learned_multi_hop_query(mh_q[i].first, mh_q[i].second);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                ans_multi.push_back((uint32_t)res.size());
                return res.size();
            }, m, &total_results);
        // correctness pass; scan_overhead is measured by the instrumented pass.
        size_t exact = 0;
        for (auto& q : mh_q) {
            auto res = db->learned_multi_hop_query(q.first, q.second);
            std::vector<uint32_t> got; got.reserve(res.size());
            for (offset_t v : res) got.push_back((uint32_t)v);
            std::vector<uint32_t> exp = mh_gt[pack2(q.first, q.second)];
            std::sort(got.begin(), got.end());
            std::sort(exp.begin(), exp.end());
            if (got == exp) exact++;
        }
        m.correct_pct = 100.0 * exact / mh_q.size();
        m.avg_results = total_results / mh_q.size();
        results.push_back(m);
        std::cout << "  [multi_hop] exact " << exact << "/" << mh_q.size()
                  << ", avg " << m.avg_results << std::endl;
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
    size_t btree_size_bytes = triples.size() * (sizeof(uint64_t) + sizeof(offset_t));
    std::cout << "  B+-tree built: " << triples.size() << " keys, build " << btree_build_ms
              << " ms" << std::endl;

    // ---- B+-tree positive point queries (same pos_q) ----
    {
        Metrics m;
        run_phase("btree_point", pos_q.size(), 0,
            [&](size_t i){ offset_t v; return (size_t)btree->lookup(pack(pos_q[i][0], pos_q[i][1], pos_q[i][2]), &v); },
            [&](size_t i, double& us) -> size_t {
                uint64_t key = pack(pos_q[i][0], pos_q[i][1], pos_q[i][2]);
                auto t0 = clk::now();
                offset_t v; bool ok = btree->lookup(key, &v);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                return ok ? 1 : 0;
            }, m, nullptr);
        size_t found = 0;
        for (auto& q : pos_q) { offset_t v; if (btree->lookup(pack(q[0], q[1], q[2]), &v)) found++; }
        m.correct_pct = 100.0 * found / pos_q.size();
        m.avg_results = (double)found / pos_q.size();
        results.push_back(m);
        std::cout << "  [btree_point] found " << found << "/" << pos_q.size() << std::endl;
    }

    // ---- B+-tree negative point queries (same neg_q) ----
    {
        Metrics m;
        run_phase("btree_point_negative", neg_q.size(), 0,
            [&](size_t){ return (size_t)0; },
            [&](size_t i, double& us) -> size_t {
                uint64_t key = pack(neg_q[i][0], neg_q[i][1], neg_q[i][2]);
                auto t0 = clk::now();
                offset_t v; bool ok = btree->lookup(key, &v);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                return ok ? 1 : 0;
            }, m, nullptr);
        size_t found = 0;
        for (auto& q : neg_q) { offset_t v; if (btree->lookup(pack(q[0], q[1], q[2]), &v)) found++; }
        m.correct_pct = 100.0 * (neg_q.size() - found) / neg_q.size();
        m.avg_results = (double)found / neg_q.size();
        results.push_back(m);
        std::cout << "  [btree_point_negative] false positives " << found << "/" << neg_q.size()
                  << std::endl;
    }

    // ---- Phase 3b/3c: pointer-chasing over Poseidon's linked lists (needs a tx) ----
    db->begin_transaction();

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
        Metrics m;
        double total_results = 0;
        run_phase("native_single_hop", range_q.size(), sizeof(uint32_t),
            [&](size_t i){ std::vector<uint32_t> t; walk_single(range_q[i], t); return t.size(); },
            [&](size_t i, double& us) -> size_t {
                std::vector<uint32_t> out;
                auto t0 = clk::now();
                walk_single(range_q[i], out);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                return out.size();
            }, m, &total_results);
        size_t exact = 0;
        for (uint32_t s : range_q) {
            std::vector<uint32_t> out; walk_single(s, out);
            std::vector<uint32_t> exp = range_gt[s];
            std::sort(out.begin(), out.end());
            std::sort(exp.begin(), exp.end());
            if (out == exp) exact++;
        }
        m.correct_pct = 100.0 * exact / range_q.size();
        m.avg_results = total_results / range_q.size();
        // scan_overhead left N/A (0): the measured counter instruments
        // FloodSourceSort, which the pointer-chasing baseline does not use.
        results.push_back(m);
        std::cout << "  [native_single_hop] exact " << exact << "/" << range_q.size()
                  << ", avg " << m.avg_results << std::endl;
    }

    // ---- native multi-hop range (same mh_q) ----
    {
        Metrics m;
        double total_results = 0;
        run_phase("native_multi_hop", mh_q.size(), sizeof(uint32_t),
            [&](size_t i){ std::vector<uint32_t> t; walk_multi(mh_q[i].first, mh_q[i].second, t); return t.size(); },
            [&](size_t i, double& us) -> size_t {
                std::vector<uint32_t> out;
                auto t0 = clk::now();
                walk_multi(mh_q[i].first, mh_q[i].second, out);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                return out.size();
            }, m, &total_results);
        size_t exact = 0;
        for (auto& q : mh_q) {
            std::vector<uint32_t> out; walk_multi(q.first, q.second, out);
            std::vector<uint32_t> exp = mh_gt[pack2(q.first, q.second)];
            std::sort(out.begin(), out.end());
            std::sort(exp.begin(), exp.end());
            if (out == exp) exact++;
        }
        m.correct_pct = 100.0 * exact / mh_q.size();
        m.avg_results = total_results / mh_q.size();
        // scan_overhead N/A for the native baseline (see native_single_hop).
        results.push_back(m);
        std::cout << "  [native_multi_hop] exact " << exact << "/" << mh_q.size() << ", avg "
                  << m.avg_results << std::endl;
    }

    // ---- native point query (same pos_q) ----
    {
        Metrics m;
        run_phase("native_point", pos_q.size(), 0,
            [&](size_t i){ return (size_t)point_exists(pos_q[i][0], pos_q[i][1], pos_q[i][2]); },
            [&](size_t i, double& us) -> size_t {
                auto t0 = clk::now();
                bool ok = point_exists(pos_q[i][0], pos_q[i][1], pos_q[i][2]);
                auto t1 = clk::now();
                us = std::chrono::duration<double, std::micro>(t1 - t0).count();
                return ok ? 1 : 0;
            }, m, nullptr);
        size_t found = 0;
        for (auto& q : pos_q) if (point_exists(q[0], q[1], q[2])) found++;
        m.correct_pct = 100.0 * found / pos_q.size();
        m.avg_results = (double)found / pos_q.size();
        results.push_back(m);
        std::cout << "  [native_point] found " << found << "/" << pos_q.size() << std::endl;
    }

    db->commit_transaction();

    // per-row build-time / index-size / scan_overhead selectors
    auto row_build = [&](const std::string& nm) -> double {
        if (nm.rfind("btree", 0) == 0) return btree_build_ms;
        if (nm.rfind("native", 0) == 0) return 0.0;
        return build_time_ms;
    };
    auto row_size = [&](const std::string& nm) -> size_t {
        if (nm.rfind("btree", 0) == 0) return btree_size_bytes;
        if (nm.rfind("native", 0) == 0) return 0;
        return index_size_bytes;
    };

    // -------------------- write DualIndex answer dumps --------------------
    // Consumed by scripts/oracle_verify.py to cross-check DualIndex vs the
    // C++ walker vs a brute-force Python oracle.
    auto dump_answers = [&](uint32_t type, const std::vector<uint32_t>& ans) {
        std::string path = cfg.queries_dir + "/" + dataset_name + "_" + qtype_name(type)
                         + "_answers.bin";
        std::ofstream f(path, std::ios::binary);
        uint32_t n = (uint32_t)ans.size();
        f.write(reinterpret_cast<const char*>(&n), sizeof(n));
        f.write(reinterpret_cast<const char*>(ans.data()), ans.size() * sizeof(uint32_t));
    };
    dump_answers(QT_POINT_POS, ans_pos);
    dump_answers(QT_POINT_NEG, ans_neg);
    dump_answers(QT_SINGLE_HOP, ans_single);
    dump_answers(QT_MULTI_HOP, ans_multi);

    // -------------------- measured scan_overhead (sidecar) --------------------
    // Populate the learned single_hop/multi_hop rows from the untimed
    // instrumented pass (queries/<ds>_scan_overhead.txt). Other rows (point,
    // btree, native) leave scan_overhead = 0 (N/A). If the sidecar is absent
    // (overhead pass not run), a warning is printed and the column stays 0.
    {
        std::ifstream so(scan_overhead_path(cfg));
        if (so) {
            std::string wl; double v;
            while (so >> wl >> v) {
                for (auto& m : results)
                    if (m.name == wl) m.scan_overhead = v;
            }
            std::cout << "  scan_overhead loaded from " << scan_overhead_path(cfg) << std::endl;
        } else {
            std::cerr << "WARNING: " << scan_overhead_path(cfg) << " missing; scan_overhead "
                         "column will be 0. Run --measure_scan_overhead (SCAN_DEBUG build) first."
                      << std::endl;
        }
    }

    // -------------------- output --------------------
    std::error_code ec;
    std::filesystem::create_directories(RESULTS_DIR, ec);
    RunMeta meta = gather_meta(cfg);

    // CSV SCHEMA: leading columns follow results/zmindex_all_results_v3.csv
    // column order (dataset, N, Q, epsilon, build, index, lat_mean/p50/p95/p99,
    // correctness_pct), with query_type inserted and the new required columns
    // appended (scan_overhead, avg_results, throughput_qps, dataset_fingerprint).
    // The reference 'resolution' column is replaced by 'flood_k' (our grid
    // analog) and 'avg_pgm_refine_window' is dropped (not exposed by the API).
    const char* SCHEMA_HEADER =
        "dataset,query_type,N,Q,epsilon,flood_k,build_ms,index_bytes,"
        "lat_mean_us,lat_p50_us,lat_p95_us,lat_p99_us,correctness_pct,"
        "scan_overhead,avg_results,throughput_qps,dataset_fingerprint\n";
    auto write_rows = [&](std::ostream& os, bool with_dataset) {
        os.setf(std::ios::fixed);
        for (auto& m : results) {
            if (with_dataset) os << dataset_name << ",";
            os << m.name << "," << num_triples << "," << m.n << ","
               << cfg.zm_epsilon << "," << cfg.flood_k << ","
               << row_build(m.name) << "," << row_size(m.name) << ","
               << m.mean_us << "," << m.p50 << "," << m.p95 << "," << m.p99 << ","
               << m.correct_pct << "," << m.scan_overhead << "," << m.avg_results << ","
               << m.throughput_qps << "," << fp.triple_count << ":" << fp.xor_checksum << "\n";
        }
    };

    // per-dataset CSV (truncated each run: full metadata header + schema)
    {
        std::ofstream csv(csv_path);
        write_meta_header(csv, meta, cfg, /*note_scan_overhead=*/true);
        csv << SCHEMA_HEADER;
        write_rows(csv, /*with_dataset=*/true);
    }

    // unified CSV (append; metadata + header written once)
    {
        bool exists = std::filesystem::exists(UNIFIED_CSV);
        std::ofstream u(UNIFIED_CSV, std::ios::app);
        if (!exists) {
            write_meta_header(u, meta, cfg, /*note_scan_overhead=*/true);
            u << SCHEMA_HEADER;
        }
        write_rows(u, /*with_dataset=*/true);
    }

    // per-batch throughput rows (only when --print_batch_stats)
    if (cfg.print_batch_stats) {
        std::ofstream b(batches_csv_path);
        write_meta_header(b, meta, cfg);
        b << "dataset,workload,batch_index,batch_n,batch_throughput_qps,batch_mean_us\n";
        b.setf(std::ios::fixed);
        for (auto& bs : batch_stats) {
            b << dataset_name << "," << bs.workload << "," << bs.batch_index << ","
              << bs.batch_n << "," << bs.batch_throughput_qps << "," << bs.batch_mean_us << "\n";
        }
        std::cout << "\nper-batch CSV    : " << batches_csv_path << " (" << batch_stats.size()
                  << " rows)" << std::endl;
    }

    // perf CSV (only when --perf)
    if (cfg.perf) {
        std::ofstream pf(perf_csv_path);
        write_meta_header(pf, meta, cfg);
        pf << "dataset,workload,llc_loads,llc_load_misses,cycles,instructions,"
              "elapsed_s,est_bandwidth_gbps\n";
        pf.setf(std::ios::fixed);
        for (auto& pp : perf_phases) {
            pf << dataset_name << "," << pp.workload << ","
               << pp.sample.llc_loads << "," << pp.sample.llc_misses << ","
               << pp.sample.cycles << "," << pp.sample.instructions << ","
               << pp.elapsed_s << ",";
            if (pp.bandwidth_gbps >= 0) pf << pp.bandwidth_gbps; else pf << "NA";
            pf << "\n";
        }
        std::cout << "perf CSV         : " << perf_csv_path << std::endl;
    }

    std::cout << "\n================== SUMMARY (" << dataset_name << ") =================="
              << std::endl;
    std::cout << "cpu=" << meta.cpu << std::endl;
    std::cout << "git_poseidon_core=" << meta.git_repo
              << "  git_dualindex=" << meta.git_dualindex << std::endl;
    std::cout << "nodes=" << n_nodes << " edges=" << n_edges << " triples=" << num_triples
              << "  fingerprint=" << fp.triple_count << ":" << fp.xor_checksum << std::endl;
    std::cout << "flood_k=" << cfg.flood_k << " zm_epsilon=" << cfg.zm_epsilon
              << " build_time_ms=" << build_time_ms << " index_size_bytes=" << index_size_bytes
              << std::endl;
    printf("%-20s %9s %10s %10s %10s %10s %14s %10s %8s %12s\n", "query_type", "n", "mean_us",
           "p50_us", "p95_us", "p99_us", "qps", "correct%", "scan_ov", "avg_results");
    for (auto& m : results) {
        printf("%-20s %9zu %10.3f %10.3f %10.3f %10.3f %14.1f %10.3f %8.3f %12.4f\n",
               m.name.c_str(), m.n, m.mean_us, m.p50, m.p95, m.p99, m.throughput_qps,
               m.correct_pct, m.scan_overhead, m.avg_results);
    }
    std::cout << "\nper-dataset CSV  : " << csv_path << std::endl;
    std::cout << "unified CSV (app): " << UNIFIED_CSV << std::endl;

    // -------------------- DualIndex vs Native comparison --------------------
    auto mean_of = [&](const std::string& nm) -> double {
        for (auto& m : results) if (m.name == nm) return m.mean_us;
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
