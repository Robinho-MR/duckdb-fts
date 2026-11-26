#pragma once

#include "fts_common.hpp"


namespace duckdb {

enum class FTSStopwordsMode : uint8_t { NONE, ENGLISH, TABLE };

struct FTSIndexConfig {
  FTSTableName input_table;
  string input_id;
  vector<string> input_values;
  string stemmer;
  FTSStopwordsMode stopwords_mode = FTSStopwordsMode::ENGLISH;
  FTSTableName stopwords_table;
  string tokenizer;
  string ignore;
  bool strip_accents = true;
  bool lower = true;
  bool incremental = false;
  bool cluster_terms = false;
  bool layered_search = false;
  bool structured_queries = false;
};

class FTSIndexBuilder {
public:
  static string Create(const FTSIndexConfig &config,
                       bool drop_existing_triggers);
  static string CreateStructuredQueryMacros(const FTSTableName &qname);
};

} // namespace duckdb
