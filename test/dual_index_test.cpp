/*	End-to-end integration test on a 5-node Person/KNOWS graph.*/

#include <iostream>
#include "graph_db.hpp"
#include "graph_pool.hpp"

const std::string test_path = PMDK_PATH("dualindex_tst");

int main() {
    std::cout << "=== DualIndex + Poseidon Integration Test ===" << std::endl;

    auto pool = graph_pool::create(test_path);
    auto db = pool->create_graph("test_graph");

    auto alice   = db->import_node("Person", {{"name", std::any(std::string("Alice"))}});
    auto bob     = db->import_node("Person", {{"name", std::any(std::string("Bob"))}});
    auto charlie = db->import_node("Person", {{"name", std::any(std::string("Charlie"))}});
    auto diana   = db->import_node("Person", {{"name", std::any(std::string("Diana"))}});
    auto eve     = db->import_node("Person", {{"name", std::any(std::string("Eve"))}});

    std::cout << "Created 5 nodes: Alice=" << alice
              << " Bob=" << bob
              << " Charlie=" << charlie
              << " Diana=" << diana
              << " Eve=" << eve << std::endl;

    db->import_relationship(alice, bob, "KNOWS", {});
    db->import_relationship(alice, charlie, "KNOWS", {});
    db->import_relationship(bob, diana, "KNOWS", {});
    db->import_relationship(bob, eve, "KNOWS", {});
    db->import_relationship(charlie, eve, "KNOWS", {});

    std::cout << "Created 5 relationships" << std::endl;

    std::cout << "\nBuilding learned index..." << std::endl;
    db->begin_transaction();
    db->build_learned_index();
    db->commit_transaction();

    std::cout << "\n--- Point Query Tests ---" << std::endl;

    auto r1 = db->learned_point_lookup(alice, bob, diana);
    std::cout << "Path (Alice->Bob->Diana): "
              << (r1 != UNKNOWN ? "FOUND" : "NOT FOUND") << std::endl;

    auto r2 = db->learned_point_lookup(alice, bob, charlie);
    std::cout << "Path (Alice->Bob->Charlie): "
              << (r2 != UNKNOWN ? "FOUND" : "NOT FOUND") << std::endl;

    std::cout << "\n--- Range Query Test ---" << std::endl;

    auto results = db->learned_range_query(alice);
    std::cout << "2-hop paths from Alice: " << results.size() << " results" << std::endl;

    graph_pool::destroy(pool);

    std::cout << "\n=== Test Complete ===" << std::endl;
    return 0;
}