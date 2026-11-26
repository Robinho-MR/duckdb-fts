#pragma once

#include "fts_common.hpp"

#include "fts_index_common.hpp"


namespace duckdb {

struct FTSIndexMaintenanceConfig {
  FTSAnalyzerConfig analyzer;
  bool cluster_terms = false;
  bool layered_search = false;
};

class FTSIndexMaintenance {
public:
  static string DropTriggers(const FTSTableName &qname);
  static string Create(const FTSTableName &qname, const string &input_id,
                       const vector<string> &input_values,
                       const FTSIndexMaintenanceConfig &config);
};

} // namespace duckdb
