/*The bridge to Poseidon's graph storage — 
implements the graph_db methods that walk the real relationship lists, 
build triples, and delegate to DualIndex.*/


#include "graph_db.hpp"
#include <iostream>

void graph_db::build_learned_index() {
    DualIndex::Points triples;

    auto& cv_nodes = nodes_->as_vec();
    offset_t max_nodes = cv_nodes.last_used() + 1;

    for (offset_t i = 0; i < max_nodes; i++) {

        if (!cv_nodes.is_used(i)) continue;

        auto& src_node = node_by_id(i);

        offset_t rship_id = src_node.from_rship_list;
        while (rship_id != UNKNOWN) {
            auto& r1 = rship_by_id(rship_id);
            offset_t hop1 = r1.dest_node;

            auto& hop1_node = node_by_id(hop1);
            offset_t rship_id2 = hop1_node.from_rship_list;
            while (rship_id2 != UNKNOWN) {
                auto& r2 = rship_by_id(rship_id2);
                offset_t hop2 = r2.dest_node;

                triples.push_back({static_cast<double>(i),
                                   static_cast<double>(hop1),
                                   static_cast<double>(hop2)});

                rship_id2 = r2.next_src_rship;
            }

            rship_id = r1.next_src_rship;
        }
    }

    std::cout << "Extracted " << triples.size() << " 2-hop path triples" << std::endl;

    dual_index_ = std::make_unique<DualIndex>();
    dual_index_->build(triples);

    std::cout << "DualIndex built. Size: " << dual_index_->index_size() << " bytes" << std::endl;
}

offset_t graph_db::learned_point_lookup(offset_t src, offset_t hop1, offset_t hop2) {
    if (!dual_index_) return UNKNOWN;
    return dual_index_->point_lookup(src, hop1, hop2);
}

std::vector<offset_t> graph_db::learned_range_query(offset_t src) {
    if (!dual_index_) return {};
    return dual_index_->range_query(src);
}