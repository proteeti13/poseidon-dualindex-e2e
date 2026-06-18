/*
 * Copyright (C) 2019-2023 DBIS Group - TU Ilmenau, All Rights Reserved.
 *
 * This file is part of the Poseidon package.
 *
 * Poseidon is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Poseidon is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Poseidon. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef graph_db_hpp_
#define graph_db_hpp_

#include <any>
#include <map>
#include <mutex>
#include <string>

#include "dict.hpp"
#include "exceptions.hpp"
#include "nodes.hpp"
#include "properties.hpp"
#include "relationships.hpp"
#include "vec.hpp"
#include "transaction.hpp"
#include "btree.hpp"
#include "index_map.hpp"
#include "dual_index.hpp"
#ifdef USE_PMDK
#include "pm_ulog.hpp"
#elif defined(USE_PFILES)
#include "walog.hpp"
#endif
#include "gc.hpp"
#include "robin_hood.h"
#include "bufferpool.hpp"

#include "analytics.hpp"
#include "parser.hpp"

#if defined CSR_DELTA
#include "csr_delta.hpp"
#endif

/**
 * graph_db represents a graph consisting of nodes and relationships with
 * properties stored in a database. The class provides methods for constructing
 * and querying the graph.
 */
class graph_db {
  friend struct query_ctx;
public:
  /**
   * mapping_t is used during importing data from CSV files to map node names to
   * internal ids which are required for creating relationships.
   */
  using mapping_t = robin_hood::unordered_map<std::string, node::id_t>;

  using typespec_t = robin_hood::unordered_map<std::string, p_item::p_typecode>;

  using node_consumer_func = std::function<void(node &)>;
  using rship_consumer_func = std::function<void(relationship &)>;

  static void destroy(p_ptr<graph_db> gp);

  /**
   * Constructor for a new empty graph database.
   */
  graph_db(const std::string &db_name = "", const std::string &pool_path = "", std::size_t bpool_size = DEFAULT_BUFFER_SIZE);

  /**
   * Destructor.
   */
  ~graph_db();

  /* -------------- transaction management -------------- */

  /**
   * Starts a new transaction. This transaction is associated with the 
   * current thread and stored there as thread_local property.
   */
  void begin_transaction();

  /**
   * Commits the currently active transaction associated with this thread.
   */
  bool commit_transaction();

  /**
   * Aborts the currently active transaction associated with this thread.
   */
  bool abort_transaction();

  /**
   * Encapsulated code for execution a transaction. If body returns true then
   * the transaction is committed, otherwise the transaction is aborted.
   */
  bool run_transaction(std::function<bool()> body);

  /* ---------------- graph construction ---------------- */

  /**
   * Add a new node to the graph with the given label (type) and the
   * set of properties (key-value pairs). The method returns the node identifier
   * which can be used to create a relationship from or to this node.
   */
  node::id_t add_node(const std::string &label, const properties_t &props,
                      bool append_only = false);

  /**
   * Add a new node to the graph with the given label (type) and the
   * set of properties (key-value pairs) without transactional support.
   * The method returns the node identifier which can be used to create a
   * relationship from or to this node.
   */
  node::id_t import_node(const std::string &label, const properties_t &props);

  node::id_t import_typed_node(dcode_t label, const std::vector<dcode_t> &keys,
                              const std::vector<p_item::p_typecode>& typelist, 
                              const std::vector<std::any>& values);

  node::id_t import_typed_node(dcode_t label, const std::vector<dcode_t> &keys,
                              const std::vector<p_item::p_typecode>& typelist,
							  const std::vector<std::string>& values,dict_ptr &dict);

  /**
   * Add a new relationship to the graph that connects from_node and to_node.
   * This relationship has initialized with the given label and properties.
   */
  relationship::id_t add_relationship(node::id_t from_node, node::id_t to_node,
                                      const std::string &label,
                                      const properties_t &props,
                                      bool append_only = false);

  /**
   * Add a new relationship to the graph that connects from_node and to_node
   * without transactional support. This relationship has initialized with the
   * given label and properties.
   */
  relationship::id_t import_relationship(node::id_t from_node,
                                         node::id_t to_node,
                                         const std::string &label,
                                         const properties_t &props);

  relationship::id_t import_typed_relationship(node::id_t from_node,
                                         node::id_t to_node,
                                         dcode_t label, 
                                         const std::vector<dcode_t> &keys,
                                         const std::vector<p_item::p_typecode>& typelist, 
					  const std::vector<std::string>& values,dict_ptr &dict);


  relationship::id_t import_typed_relationship(node::id_t from_node,
                                         node::id_t to_node,
                                         dcode_t label,
                                         const std::vector<dcode_t> &keys,
                                         const std::vector<p_item::p_typecode>& typelist,
                                         const std::vector<std::any>& values);

  /* --------------- node/relationship information --------------- */

  /**
   * Returns a description of the node, i.e. with all decoded labels and
   * properties.
   */
  node_description get_node_description(node::id_t nid);

  /**
   * Returns a description of the relationship, i.e. with all decoded labels and
   * properties.
   */
  rship_description get_rship_description(relationship::id_t rid);

  /**
   * Returns the decoded label of the relationship.
   */
  const char *get_relationship_label(const relationship &r);

  /**
   * Returns the node identified by the given id.
   */
  node &node_by_id(node::id_t id);

  /**
   * Returns the relationship identified by the given id.
   */
  relationship &rship_by_id(relationship::id_t id);

  /* --------------- graph updates --------------- */
 
  /**
   * Updates the given node by changing the given
   * properties and replacing the label with the given one.
   */
  void update_node(node &n, const properties_t &props,
                   const std::string &label = "");

  /**
   * Updates the given relationship by changing the given
   * properties and replacing the label with the given one.
   */
  void update_relationship(relationship &r, const properties_t &props,
                           const std::string &label = "");


  /**
   * Deletes the node and its properties identified by the given id.
   */
  void delete_node(node::id_t id);

  /**
   * Delets the node identified by the given id, its properties and all the relationships of this node.
   */
  void detach_delete_node(node::id_t id);
  
  /**
   * Deletes the relationship and its properties identified by the given id.
   */
  void delete_relationship(relationship::id_t id);

  /**
   * Deletes the relationship and its properties identified by the given 
   * source and destination node ids.
   */
  void delete_relationship(node::id_t src, node::id_t dest);

  /* ---------------- CSV data import ---------------- */

  /**
   * Read the list of nodes from the given CSV file. The file is in ldbc
   * format with the given delimiter.
   */
  std::size_t import_nodes_from_csv(const std::string &label,
                                    const std::string &filename, char delim,
                                    std::optional<mapping_t> &m, std::mutex *mtx = nullptr);

  std::size_t import_typed_nodes_from_csv(const std::string &label,
                                    const std::string &filename, char delim,
                                    std::optional<mapping_t> &m, typespec_t &ty, std::mutex *mtx = nullptr);

  /**
   * Read the list of relationships from the given CSV file. The file is in
   * ldbc format with the given delimiter.
   */
  std::size_t import_relationships_from_csv(const std::string &label, const std::string &filename,
                                            char delim, std::optional<mapping_t> &m, std::mutex *mtx = nullptr);

  std::size_t import_typed_relationships_from_csv(const std::string &label, const std::string &filename,
                                            char delim, std::optional<mapping_t> &m, typespec_t &ty, std::mutex *mtx = nullptr);

  /* ---------------- helper ---------------- */

  /**
   * Performs initialization steps after starting the database.
   */
  void runtime_initialize();

  /**
   * Returns a reference to the dictionary of string codes.
   */
  p_ptr<dict> &get_dictionary() { return dict_; }

  /**
   * Returns a reference to the node property list of this graph.
   */
  const auto& get_node_properties() { return node_properties_; }

  /**
   * Returns a reference to the relationship property list of this graph.
   */
  const auto& get_rship_properties() { return rship_properties_; }

  /**
   * Returns a reference to the node list of this graph.
   */  

  const auto& get_nodes() { return nodes_; }

  /**
   * Returns a reference to the relationship list of this graph.
   */
  const auto& get_relationships() { return rships_; }

  /**
   * Access to node and relationship properties.
   */
  p_item get_property_value(const node &n, const std::string& pkey);
  p_item get_property_value(const node &n, dcode_t pcode);

  p_item get_property_value(const relationship &r, const std::string& pkey);
  p_item get_property_value(const relationship &r, dcode_t pcode);

  /**
   * Return the node version from the dirty list that is valid for the
   * transaction identified by xid.
   */
  node &get_valid_node_version(node &n, xid_t xid);

  /**
   * Return the relationship version from the dirty list that is valid for the
   * transaction identified by xid.
   */
  relationship &get_valid_rship_version(relationship &r, xid_t xid);

  /**
   * Returns the string value encoded with the given dictionary code.
   */
  const char *get_string(dcode_t c);

  /**
   * Returns the dictionary code for the given string.
   */
  dcode_t get_code(const std::string &s);

  /**
   * Prints the graph (nodes, relationships) to standard output.
   */
  void dump();

  /**
   * Generate a DOT file with the given name representing the entire graph.
   */
  void dump_dot(const std::string& fname);

  /**
   * Print some stats about memory usage.
   */
  void print_stats();

  /**
   * Perform recovery using the undo log.
   */ 
  void apply_log();

  /* ---------------- index management ---------------- */
  
  /**
   * Create an index on the nodes table for all nodes with the given label and
   * the property. The resulting index allows lookup and range scans on values 
   * of this property.
   */
  index_id create_index(const std::string& node_label, const std::string& prop_name);

  /**
   * Returns true if an index exists for node_label + prop_name.
   */
  bool has_index(const std::string& node_label, const std::string& prop_name);

  /**
   * Return the id of the index for the given label/property combination. Raises an
   * exception of no corresponding index exists.
   */
  index_id get_index(const std::string& node_label, const std::string& prop_name);

  /**
   * Delete the given index.
   */
  void drop_index(const std::string& node_label, const std::string& prop_name);

  /**
   * Perform an index lookup on the given index for the given property value key. 
   * For each matching node the consumer function is called.
   */
  void index_lookup(index_id idx, uint64_t key, node_consumer_func consumer);

  void index_lookup(std::list<index_id> &idxs, uint64_t key, node_consumer_func consumer);

  /**
   * Perform an index lookup on the given index for the given property value key. 
   * Returns the node id of the corresponding node but without considering
   * transactions.
   */
  node::id_t node_id_from_index(index_id idx_ptr, uint64_t key);


  /* ---------------- learned index (DualIndex) ---------------- */
  /*Declares the 3 new graph_db methods + a dual_index_ member; includes dual_index.hpp.*/

  /**
   * Build the learned index from the current relationship data.
   */
  void build_learned_index();

  /**
   * Point query: does the 2-hop path (src -> hop1 -> hop2) exist?
   */
  offset_t learned_point_lookup(offset_t src, offset_t hop1, offset_t hop2);

  /**
   * Range query: find all 2-hop paths starting from src.
   */
  std::vector<offset_t> learned_range_query(offset_t src);

  /**
   * Multi-hop range query: find all hop2 endpoints of paths (src -> hop1 -> *).
   */
  std::vector<offset_t> learned_multi_hop_query(offset_t src, offset_t hop1);

  /**
   * Returns the in-memory size (bytes) of the learned index, or 0 if not built.
   */
  size_t learned_index_size();













/* ---------------- Analytics support ---------------- */

  /**
   * Converts the graph data to a CSR representation. If the CSR delta store is enabled 
   * and a CSR exists already for the graph, it updates the CSR so that it reflects the 
   * latest snapshot of the graph data. Otherwise, it builds the CSR by scanning the entire 
   * graph data.
   * The weight of a traversed relationship is calculated from the weight function. 
   * The bidirectional flag determines whether only outgoing relationships are considered 
   * (bidirectional = false) or both outgoing and incoming relationships (bidirectional = true).
   */
  void poseidon_to_csr(csr_arrays &csr, rship_weight weight_func, bool bidirectional = false);

#ifndef USE_GUNROCK
  /**
   * Converts the graph data to a CSR representation by scanning the entire graph data.
   */
  void host_csr_build(csr_arrays &csr, rship_weight weight_func, bool bidirectional = false);

  /**
   * Converts the graph data to a CSR representation from a parallel scan of the entire graph data.
   */
  void parallel_host_csr_build(csr_arrays &csr, rship_weight weight_func, bool bidirectional = false);

#if defined CSR_DELTA
  /**
   * Updates the existing CSR using the appropriate deltas in the CSR delta store, such that it 
   * represents the latest snapshot of the graph data.
   */
  void host_csr_update_with_delta(csr_arrays &csr);
#endif
#endif

#if defined CSR_DELTA
  /**
   * Returns a reference to the CSR delta store.
   */
  const p_ptr<delta_store>& csr_delta_store() { return delta_store_; }
#endif

  /**
   * Converts the graph data to a COO representation.
   * The weight of a traversed relationship is calculated from the weight function. 
   * The bidirectional flag determines whether only outgoing relationships are considered 
   * (bidirectional = false) or both outgoing and incoming relationships (bidirectional = true).
   */
  void poseidon_to_coo(edge_coords* edge_coordinates, float* edge_values, rship_weight weight_func, bool bidirectional = false);

  /**
   * Writes all modified pages from the bufferpool back to disk.
   */
  void flush();

  /**
   * 
   */
  void purge_bufferpool() { bpool_.purge(); }

  /**
   * 
   */
  void close_files();

bool has_page(paged_file::page_id pid) { return bpool_.has_page(pid); }

private:
  friend struct scan_task;
  friend struct recover_scan;

  std::pair<index_id, int> get_index(dcode_t label, std::list<p_item>& props);
  void index_update(std::pair<index_id, int>& idx, offset_t id, std::list<p_item>& old_props, std::list<p_item>& new_props);
  void index_insert(std::pair<index_id, int>& idx, offset_t id, std::list<p_item>& props);
  void index_delete(std::pair<index_id, int>& idx, offset_t id, std::list<p_item>& props);

  /**
   * 
   */
  void flush(const std::set<offset_t>& dirty_chunks);

  /**
   * Update the given node as the FROM node of the relationship. The relationship was already
   * created in the same transaction.
   */
  void update_from_node(transaction_ptr tx, node &n, relationship& r);

  /**
   * Update the given node as the TO node of the relationship. The relationship was already
   * created in the same transaction.
   */
  void update_to_node(transaction_ptr tx, node &n, relationship& r);

  /**
   * Handle the commit of a node from the dirty list.
   */
  void commit_dirty_node(transaction_ptr tx, node::id_t node_id);

  /**
   * Handle the commit of a relationship from the dirty list.
   */
  void commit_dirty_relationship(transaction_ptr tx, relationship::id_t rship_id);

  /**
   * Copy the properties from the dirty node to the nodes_ and properties_
   * tables.
   */
  void copy_properties(node &n, const dirty_node_ptr& dn);

  /**
   * Copy the properties from the dirty relationship to the rships_ and
   * properties_ tables.
   */
  void copy_properties(relationship &r, const dirty_rship_ptr& dr);

  /**
   * Check if the node still has valid FROM relationships.
   */
  bool has_valid_from_rships(node &n, xid_t tx);

  /**
   * Check if the node still has valid TO relationships.
   */  
  bool has_valid_to_rships(node &n, xid_t tx);

   /**
    * Perform garbage collection.
    */
  void vacuum(xid_t tx);

  /**
   * 
   */
  void prepare_files(const std::string &pool_path, const std::string &prefix);

  /**
   * 
   */
  void restore_indexes(const std::string &pool_path, const std::string &prefix);

#ifdef USE_PFILES
  void apply_redo(wa_log& log, wa_log::log_iter& li);

  void apply_undo(wa_log& log, xid_t txid, offset_t pos);
#endif

  node::id_t node_id_from_field(const graph_db::mapping_t &m, 
                const std::string& str, const std::string &field);
  node::id_t node_id_from_db(const std::string& label, const std::string& column, const std::string &field);
  std::tuple<std::string, std::string> get_rship_node(const std::string& column);
  std::tuple<node::id_t, node::id_t> get_connected_node_ids(const std::string& src_node, 
                                                                const std::string& src_column,
                                                                const std::string& dest_node, 
                                                                const std::string& dest_column,
                                                                aria::csv::CsvParser::iterator::reference& row,
                                                                std::optional<mapping_t>& m, std::size_t line); 
  node::id_t get_node_id_for_property(dcode_t label, dcode_t prop, uint64_t val);

  std::string database_name_; //
  bufferpool bpool_; //
  std::shared_ptr<paged_file> node_file_, rship_file_, nprops_file_, rprops_file_; //
  std::list<std::shared_ptr<paged_file>> index_files_; //
  std::string pool_path_; //

#ifdef USE_PMDK
  p_ptr<node_list<nvm_chunked_vec> > nodes_; // the list of all nodes of the graph
  p_ptr<relationship_list<nvm_chunked_vec> > rships_; // the list of all relationships of the graph
  p_ptr<property_list<nvm_chunked_vec> >
      node_properties_;   // the list of all properties of nodes 
  p_ptr<property_list<nvm_chunked_vec> >
      rship_properties_;   // the list of all properties of relationships
  p_ptr<pm_ulog> ulog_; // the undo log 
#elif defined(USE_IN_MEMORY)
  p_ptr<node_list<mem_chunked_vec> > nodes_; // the list of all nodes of the graph
  p_ptr<relationship_list<mem_chunked_vec> > rships_; // the list of all relationships of the graph
  p_ptr<property_list<mem_chunked_vec> >
      node_properties_;   // the list of all properties of nodes 
  p_ptr<property_list<mem_chunked_vec> >
      rship_properties_;   // the list of all properties of relationships
#else // USE_PFILES
  p_ptr<node_list<buffered_vec> > nodes_; // the list of all nodes of the graph
  p_ptr<relationship_list<buffered_vec> > rships_; // the list of all relationships of the graph
  p_ptr<property_list<buffered_vec> >
      node_properties_;   // the list of all properties of nodes 
  p_ptr<property_list<buffered_vec> >
      rship_properties_;   // the list of all properties of relationships
  p_ptr<wa_log> walog_;
#endif
  p_ptr<dict> dict_; // the dictionary used for string compression

  p_ptr<index_map> index_map_; // the list of all exisiting indexes
  std::unique_ptr<DualIndex> dual_index_; // learned index for relationship path queries

#if defined CSR_DELTA
  p_ptr<delta_store> delta_store_; // the CSR delta store
#endif
  /**
   * These member variables are volatile and have to be reinitialized
   * during startup.
   */
  std::map<xid_t, transaction_ptr>
      *active_tx_;   // the list of all active transactions
  std::mutex *m_;    // mutex for accessing active_tx_
  xid_t oldest_xid_; // timestamp of the oldest transaction
  std::mutex *gcm_;
  gc_list *garbage_;
};

using graph_db_ptr = p_ptr<graph_db>;


#endif
