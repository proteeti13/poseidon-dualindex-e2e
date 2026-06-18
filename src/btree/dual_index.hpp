/* The DualIndex wrapper class.
 Holds two learned indexes side by side over (source, hop1, hop2) triples 
and exposes a clean 3-method API. */

#ifndef dual_index_hpp_  /*if not defined */
#define dual_index_hpp_

/*stl or system*/
#include <vector>
#include <array>
#include <cstddef>  /* size_t or uint64_t undee the hood*/
#include <memory>   /*for unique pointer*/

/*project folders*/
#include "learned/zmindex.hpp"
#include "learned/flood_sortsource.hpp"  /* SourceID as sort dim — fast on graph range queries */
#include "defs.hpp"  /* poseidon'ss basic type defintions like offset_t*/


class DualIndex {
public:
    /* Types*/
    using Triple = std::array<double, 3>;  /*source, hop1, hop2*/
    using Points = std::vector<Triple>;  /* a collection of graph paths*/

    void build(const Points& triples); /* takes 2 copies of the graph data for both of our models*/

    /*point query*/
    offset_t point_lookup(offset_t src, offset_t hop1, offset_t hop2) const; 

    /*range query return a list of offsets*/
    std::vector<offset_t> range_query(offset_t src) const;

    /*multi-hop range query: pin src AND hop1, return matching hop2 ids*/
    std::vector<offset_t> multi_hop_query(offset_t src, offset_t hop1) const;

    size_t index_size() const; /* return the size of the index in bytes*/

private:

    Points zm_data_;
    Points flood_data_;

    /* alotted slot for zmindex object <the line segnments>, to be filled by build().
    when we create _zm_index_, ZMIndex needs data due to its non-empty constructor,
    but as we don;t have any data yet because build() isn't called - we decide to use
    unique pointer. Because IT CAN BE EMPTY. 
    
    
    */
    std::unique_ptr<bench::index::ZMIndex<3,64>> zm_index_;
    /* FloodSourceSort: SourceID is the sort dimension, K=4 (optimal for this layout —
       small K wins because scans are exact; stock Flood used K=20). */
    std::unique_ptr<bench::index::FloodSourceSort<3,4,64>> flood_index_;

};

#endif