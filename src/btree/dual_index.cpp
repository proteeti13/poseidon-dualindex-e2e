/* The DualIndex wrapper class.
 Holds two learned indexes side by side over (source, hop1, hop2) triples 
and exposes a clean 3-method API. */


#include "dual_index.hpp"
#include <limits>
#include "type.hpp"

void DualIndex::build(const Points& triples){

    /*make 2 copies of RAW DATA, because zmindex constructor wants a non const reference*/
    /* mostly for flood as it ACTUALLY modifies the data (sorts)*/
    zm_data_= triples; 
    flood_data_ = triples;


    /*creates a new ZMIndex object, passes zm_data_ to constructor, 
    ZMIndex reads every triple, converts triples to Morton Code (flattens into 1D), 
    trains PGM line segments for the sorted morton codes, 
    stores these line segments as the learned model,
    example: zm_index_= [ trained ZM model with xyz number of line segments ] */
    zm_index_ = std::make_unique<bench::index::ZMIndex<3,64>>(zm_data_);

    /* created FloodSourceSort object, passes flood_data_ to constructor.
       FloodSourceSort sorts within each grid cell by SourceID (dim 0) — the
       column every range query filters — so single-hop "neighbors of src"
       scans only the matching rows instead of a whole grid column.
       K=4 (small) is optimal here; stock Flood used K=20. */
    flood_index_ = std::make_unique<bench::index::FloodSourceSort<3,4,64>>(flood_data_);
}

offset_t DualIndex::point_lookup(offset_t src, offset_t hop1, offset_t hop2) const{

    Triple query ={ static_cast<double>(src),
                    static_cast<double>(hop1),
                    static_cast<double>(hop2)};
                    
    auto result = zm_index_->point_lookup(query);

    if (result.found){
        return src;
    }

    return UNKNOWN;


}

std::vector<offset_t> DualIndex::range_query(offset_t src) const{
    Triple min_corner = {static_cast<double>(src), 0.0, 0.0};
    Triple max_corner = {static_cast<double>(src), 
                         std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max()};

    box_t<3> box(min_corner, max_corner);

    Points results = flood_index_->range_query(box);

    std::vector<offset_t> offsets;
    for (const auto& triple : results) {
        offsets.push_back(static_cast<offset_t>(triple[2]));
    }

    return offsets;

}

std::vector<offset_t> DualIndex::multi_hop_query(offset_t src, offset_t hop1) const{
    /* pin both SourceID (dim 0) and Hop1_ID (dim 1); leave Hop2_ID (dim 2) open.
       FloodSourceSort applies an exact is_in_box filter, so only the precise
       (src, hop1, *) rows survive. */
    Triple min_corner = {static_cast<double>(src),
                         static_cast<double>(hop1),
                         0.0};
    Triple max_corner = {static_cast<double>(src),
                         static_cast<double>(hop1),
                         std::numeric_limits<double>::max()};

    box_t<3> box(min_corner, max_corner);

    Points results = flood_index_->range_query(box);

    std::vector<offset_t> offsets;
    for (const auto& triple : results) {
        offsets.push_back(static_cast<offset_t>(triple[2]));
    }

    return offsets;
}


std::vector<offset_t> DualIndex::query(const std::array<offset_t, 3>& min_corner,
                                       const std::array<offset_t, 3>& max_corner) const{
    /* Count pinned dimensions (min == max), same is_point() logic as
       indexes/router.hpp. The number of pinned dims selects the access path. */
    size_t pinned = 0;
    for (size_t d = 0; d < 3; ++d) {
        if (min_corner[d] == max_corner[d]) ++pinned;
    }

    if (pinned == 3) {
        /* all three dims pinned -> exact point lookup via ZM-Index.
           point_lookup returns src when found, UNKNOWN otherwise; surface that
           as a single-element vector (or empty) so the router has one return type. */
        offset_t hit = point_lookup(min_corner[0], min_corner[1], min_corner[2]);
        if (hit == UNKNOWN) return {};
        return { hit };
    }

    if (pinned == 2) {
        /* src + hop1 pinned, hop2 open -> multi-hop range via FloodSourceSort. */
        return multi_hop_query(min_corner[0], min_corner[1]);
    }

    /* only src pinned (or fewer) -> single-hop range via FloodSourceSort. */
    return range_query(min_corner[0]);
}


size_t DualIndex::index_size() const{
    size_t total = 0;
    if(zm_index_){
        total += zm_index_->index_size();
    }

    if(flood_index_){
        total +=flood_index_->index_size();
    }

    return total;


}