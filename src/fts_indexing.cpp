#include "fts_indexing.hpp"
#include "fts_index_builder.hpp"
#include "fts_index_common.hpp"
#include "fts_index_maintenance.hpp"

#include <cstdlib>

namespace duckdb {

namespace {

//! Looks up what the PRAGMAs need to know about the catalog. The lookups run
//! on a connection that runs in the transaction of the PRAGMA, so they see the
//! changes of the transaction and resolve names as the PRAGMA does.
class FTSCatalog {
public:
  explicit FTSCatalog(cxx::Context &context)
      : connection(context.ConnectInTransaction()) {}

  //! The rows of a query that returns VARCHAR columns, NULL as an empty string
  vector<vector<string>> Rows(const string &sql) {
    vector<vector<string>> rows;
    auto result = connection.Execute(sql);
    while (auto chunk = result.FetchChunk()) {
      auto column_count = chunk.GetVectorCount();
      vector<cxx::VectorView> views;
      for (idx_t column = 0; column < column_count; column++) {
        views.push_back(chunk.GetVector(column).GetView());
      }
      for (idx_t row = 0; row < chunk.GetRowCount(); row++) {
        vector<string> values;
        for (auto &view : views) {
          values.push_back(
              view.IsValid(row)
                  ? string(view.Data<cxx::varchar_t>()[view.SelAt(row)].view())
                  : string());
        }
        rows.push_back(std::move(values));
      }
    }
    return rows;
  }

  bool Exists(const string &sql) { return !Rows(sql).empty(); }

  string Scalar(const string &sql) {
    auto rows = Rows(sql);
    return rows.empty() ? string() : rows[0][0];
  }

  //! The catalog a name without a catalog refers to
  string DefaultCatalog() { return Scalar("SELECT current_database()::VARCHAR"); }

  string DefaultSchema() { return Scalar("SELECT current_schema()::VARCHAR"); }

  bool CatalogExists(const string &catalog) {
    return Exists("SELECT 1 FROM duckdb_databases() WHERE lower(database_name) "
                  "= lower(" +
                  QuoteLiteral(catalog) + ")");
  }

  //! A schema of a catalog; without a catalog, of the catalogs that names
  //! without a catalog refer to
  bool SchemaExists(const string &catalog, const string &schema) {
    return Exists("SELECT 1 FROM duckdb_schemas() WHERE parent_schema IS NULL "
                  "AND lower(schema_name) = lower(" +
                  QuoteLiteral(schema) + ") AND " + CatalogCondition(catalog));
  }

  //! Whether `parent.child` is a nested schema of the default catalog
  bool NestedSchemaExists(const string &parent, const string &child) {
    return Exists("SELECT 1 FROM duckdb_schemas() WHERE lower(parent_schema) = "
                  "lower(" +
                  QuoteLiteral(parent) + ") AND lower(schema_name) = lower(" +
                  QuoteLiteral(child) + ") AND " + CatalogCondition(string()));
  }

  bool TableExists(const FTSTableName &name) {
    return Exists("SELECT 1 FROM duckdb_tables() WHERE " + TableCondition(name,
                                                                           "table_name"));
  }

  bool TableMacroExists(const FTSTableName &name) {
    return Exists("SELECT 1 FROM duckdb_functions() WHERE function_type = "
                  "'table_macro' AND " +
                  TableCondition(name, "function_name", false));
  }

  //! The columns of a table: name, type and whether they are NOT NULL, in
  //! the order of the table
  struct Column {
    string name;
    string type;
    bool not_null;
  };

  vector<Column> Columns(const FTSTableName &name) {
    vector<Column> columns;
    auto rows = Rows(
        "SELECT column_name, data_type, (NOT is_nullable)::VARCHAR FROM "
        "duckdb_columns() WHERE " +
        TableCondition(name, "table_name") + " ORDER BY column_index");
    for (auto &row : rows) {
      columns.push_back({row[0], row[1], row[2] == "true"});
    }
    // a primary key column cannot hold NULL either
    auto keys = Rows("SELECT unnest(constraint_column_names)::VARCHAR FROM "
                     "duckdb_constraints() WHERE constraint_type = 'PRIMARY "
                     "KEY' AND " +
                     TableCondition(name, "table_name"));
    for (auto &key : keys) {
      for (auto &column : columns) {
        if (LowerASCII(column.name) == LowerASCII(key[0])) {
          column.not_null = true;
        }
      }
    }
    return columns;
  }

  //! Whether triggers can be created in a catalog: an in-memory database or
  //! one with storage version v2.0.0 or higher
  bool SupportsTriggers(const string &catalog) {
    auto rows = Rows("SELECT coalesce(path, ''), coalesce(tags['storage_version'], "
                     "'') FROM duckdb_databases() WHERE " +
                     DatabaseCondition(catalog, "database_name"));
    if (rows.empty() || rows[0][0].empty()) {
      return true;
    }
    // the version is named like "v1.5.0+"
    auto version = rows[0][1];
    if (version.size() < 2 || version[0] != 'v') {
      return true;
    }
    return std::atoi(version.c_str() + 1) >= 2;
  }

private:
  string DatabaseCondition(const string &catalog, const string &column) {
    if (catalog.empty()) {
      return column + " IN (current_database(), 'temp')";
    }
    return "lower(" + column + ") = lower(" + QuoteLiteral(catalog) + ")";
  }

  string CatalogCondition(const string &catalog) {
    return DatabaseCondition(catalog, "database_name");
  }

  string TableCondition(const FTSTableName &name, const string &name_column,
                        bool has_schema_oid = true) {
    // schema_oid excludes the nested schemas that have the name of the schema
    auto schema_condition =
        has_schema_oid
            ? " AND schema_oid IN (SELECT oid FROM duckdb_schemas() WHERE "
              "parent_schema IS NULL AND lower(schema_name) = lower(" +
                  QuoteLiteral(name.Schema()) + "))"
            : " AND lower(schema_name) = lower(" +
                  QuoteLiteral(name.Schema()) + ")";
    return CatalogCondition(name.Catalog()) + schema_condition + " AND lower(" +
           name_column + ") = lower(" + QuoteLiteral(name.Name()) + ")";
  }

  cxx::Connection connection;
};

//! Splits a possibly qualified name at its dots: a double-quoted part may
//! contain dots and doubled quotes
vector<string> SplitName(const string &text) {
  vector<string> parts;
  string part;
  bool quoted = false;
  for (idx_t i = 0; i < text.size(); i++) {
    auto c = text[i];
    if (quoted) {
      if (c == '"') {
        if (i + 1 < text.size() && text[i + 1] == '"') {
          part += '"';
          i++;
        } else {
          quoted = false;
        }
      } else {
        part += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == '.') {
      parts.push_back(std::move(part));
      part.clear();
    } else {
      part += c;
    }
  }
  if (quoted) {
    Throw<InvalidInputException>("Unterminated quote in the name '%s'", text);
  }
  parts.push_back(std::move(part));
  for (auto &entry : parts) {
    if (entry.empty()) {
      Throw<InvalidInputException>("Zero-length identifier in the name '%s'",
                                   text);
    }
  }
  return parts;
}

//! The table a name refers to, completed with the default schema where it has
//! none
FTSTableName GetQualifiedName(FTSCatalog &catalog, const string &text) {
  auto parts = SplitName(text);
  auto nested_error = [&]() {
    Throw<NotImplementedException>(
        "FTS does not support nested schemas yet: '%s' - use at most "
        "catalog.schema.table",
        text);
  };
  if (parts.size() > 3) {
    nested_error();
  }
  if (parts.size() == 3) {
    // a.b.c is a nested schema in the default catalog, when `a` is not a
    // catalog
    if (!catalog.CatalogExists(parts[0]) &&
        catalog.NestedSchemaExists(parts[0], parts[1])) {
      nested_error();
    }
    return FTSTableName(parts[0], parts[1], parts[2]);
  }
  if (parts.size() == 2) {
    return FTSTableName(string(), parts[0], parts[1]);
  }
  return FTSTableName(string(), catalog.DefaultSchema(), parts[0]);
}

string DisplayName(const FTSTableName &name) {
  return name.Schema() + "." + name.Name();
}

//! The arguments of a call: a string or a boolean by position or by name
class FTSArguments {
public:
  explicit FTSArguments(cxx::PragmaFunction::QueryInput &input_p)
      : input(input_p), boolean(input_p.GetContext().GetFactory().ParseType(
                            "BOOLEAN")) {}

  string GetString(idx_t index) {
    return string(input.GetConstantArgument(index).Get<cxx::varchar_t>().view());
  }

  bool Has(const string &name) {
    auto index = input.FindArg(name);
    return index && !input.GetConstantArgument(*index).IsNull();
  }

  string GetString(const string &name, const string &default_value) {
    return Has(name) ? GetString(*input.FindArg(name)) : default_value;
  }

  bool GetBool(const string &name, bool default_value) {
    if (!Has(name)) {
      return default_value;
    }
    // options of type BOOLEAN accept what casts to it, e.g. overwrite=1
    return input.GetConstantArgument(*input.FindArg(name))
        .Cast(input.GetContext().GetFactory(), boolean)
        .Get<bool>();
  }

private:
  cxx::PragmaFunction::QueryInput &input;
  cxx::LogicalType boolean;
};

bool FTSSchemaExists(FTSCatalog &catalog, const FTSTableName &qname) {
  return catalog.SchemaExists(qname.Catalog(), GetFTSSchemaName(qname));
}

} // namespace

// ---------------------------------------------------------------------------
// PRAGMA implementations
// ---------------------------------------------------------------------------

string FTSIndexing::DropFTSIndexQuery(cxx::PragmaFunction::QueryInput &input) {
  FTSCatalog catalog(input.GetContext());
  FTSArguments arguments(input);
  auto qname = GetQualifiedName(catalog, arguments.GetString(0));
  string fts_schema = GetFTSSchema(qname);

  if (!FTSSchemaExists(catalog, qname)) {
    Throw<CatalogException>("a FTS index does not exist on table '%s.%s'. "
                            "Create one with 'PRAGMA create_fts_index()'.",
                            qname.Schema(), qname.Name());
  }

  string result;
  if (catalog.TableExists(qname) && catalog.SupportsTriggers(qname.Catalog())) {
    result += FTSIndexMaintenance::DropTriggers(qname);
    result += "\n";
  }
  result += Format("DROP SCHEMA %s CASCADE;", fts_schema);
  return result;
}

string
FTSIndexing::CreateFTSIndexQuery(cxx::PragmaFunction::QueryInput &input) {
  FTSCatalog catalog(input.GetContext());
  FTSArguments arguments(input);
  auto qname = GetQualifiedName(catalog, arguments.GetString(0));
  if (!catalog.TableExists(qname)) {
    Throw<CatalogException>("Table with name %s does not exist!",
                            DisplayName(qname));
  }

  const string stemmer = arguments.GetString("stemmer", "porter");
  const string tokenizer = arguments.GetString("tokenizer", "regex");
  const string stopwords = arguments.GetString("stopwords", "english");
  const string ignore = arguments.GetString(
      "ignore", "[0-9!@#$%^&*()_+={}\\[\\]:;<>,.?~\\\\/\\|''\"`-]+");
  if (tokenizer != "regex" && tokenizer != "opensearch_standard") {
    Throw<InvalidInputException>(
        "Unrecognized tokenizer '%s'. Supported tokenizers are: ['regex', "
        "'opensearch_standard']",
        tokenizer);
  }

  const bool strip_accents =
      arguments.GetBool("strip_accents", tokenizer != "opensearch_standard");
  const bool lower = arguments.GetBool("lower", true);
  const bool overwrite = arguments.GetBool("overwrite", false);
  const bool incremental = arguments.GetBool("incremental", false);
  const bool layered_search = arguments.GetBool("layered_search", false);
  const bool cluster_terms =
      arguments.GetBool("cluster_terms", false) || layered_search;
  const bool structured_queries = catalog.Exists(
      "SELECT 1 FROM duckdb_extensions() WHERE extension_name = 'json' AND "
      "loaded");

  if (tokenizer == "opensearch_standard" && strip_accents &&
      arguments.Has("strip_accents")) {
    Throw<InvalidInputException>(
        "strip_accents=true is not supported with tokenizer "
        "'opensearch_standard'. OpenSearch's standard analyzer lowercases "
        "tokens but does not strip accents; use strip_accents=false.");
  }

  FTSTableName stopwords_table;
  if (stopwords != "english" && stopwords != "none") {
    stopwords_table = GetQualifiedName(catalog, stopwords);
    if (!catalog.TableExists(stopwords_table)) {
      Throw<CatalogException>("Table with name %s does not exist!",
                              DisplayName(stopwords_table));
    }
  }

  const string fts_schema = GetFTSSchema(qname);
  if (FTSSchemaExists(catalog, qname) && !overwrite) {
    Throw<CatalogException>("a FTS index already exists on table '%s.%s'. "
                            "Supply 'overwrite=1' to overwrite, or "
                            "drop the existing index with 'PRAGMA "
                            "drop_fts_index()' before creating a new one.",
                            qname.Schema(), qname.Name());
  }

  // Positional parameters: table, id column, value column(s)
  const string doc_id = arguments.GetString(1);
  auto columns = catalog.Columns(qname);
  auto find_column = [&](const string &name) -> const FTSCatalog::Column * {
    for (auto &column : columns) {
      if (LowerASCII(column.name) == LowerASCII(name)) {
        return &column;
      }
    }
    return nullptr;
  };
  auto id_column = find_column(doc_id);
  if (!id_column) {
    Throw<CatalogException>("Table '%s.%s' does not have a column named '%s'!",
                            qname.Schema(), qname.Name(), doc_id);
  }
  vector<string> doc_values;
  auto value_count = input.GetArgumentCounts().NamedOffset();
  for (idx_t i = 2; i < value_count; i++) {
    const string col_name = arguments.GetString(i);
    if (col_name == "*") {
      doc_values.clear();
      for (auto &column : columns) {
        if (column.type == "VARCHAR") {
          doc_values.push_back(column.name);
        }
      }
      break;
    }
    if (!find_column(col_name)) {
      Throw<CatalogException>(
          "Table '%s.%s' does not have a column named '%s'!", qname.Schema(),
          qname.Name(), col_name);
    }
    doc_values.push_back(col_name);
  }
  if (doc_values.empty()) {
    Throw<InvalidInputException>(
        "at least one column must be supplied for indexing!");
  }
  auto supports_triggers = catalog.SupportsTriggers(qname.Catalog());
  if (incremental && !supports_triggers) {
    Throw<InvalidInputException>(
        "incremental FTS indexes require trigger support. Persistent DuckDB "
        "databases must use storage version v2.0.0 or higher; reattach/create "
        "the database with STORAGE_VERSION 'v2.0.0', or omit incremental=true "
        "to create a rebuild-only FTS index.");
  }
  if (incremental && !id_column->not_null) {
    Throw<InvalidInputException>("incremental FTS indexes require the document "
                                 "id column to be NOT NULL");
  }
  FTSIndexConfig config;
  config.input_table = qname;
  config.input_id = doc_id;
  config.input_values = std::move(doc_values);
  config.stemmer = stemmer;
  if (stopwords == "none") {
    config.stopwords_mode = FTSStopwordsMode::NONE;
  } else if (stopwords == "english") {
    config.stopwords_mode = FTSStopwordsMode::ENGLISH;
  } else {
    config.stopwords_mode = FTSStopwordsMode::TABLE;
    config.stopwords_table = std::move(stopwords_table);
  }
  config.tokenizer = tokenizer;
  config.ignore = ignore;
  config.strip_accents = strip_accents;
  config.lower = lower;
  config.incremental = incremental;
  config.cluster_terms = cluster_terms;
  config.layered_search = layered_search;
  config.structured_queries = structured_queries;
  auto drop_existing_triggers = catalog.TableExists(qname) && supports_triggers;
  return FTSIndexBuilder::Create(config, drop_existing_triggers);
}

string FTSIndexing::CreateFTSBooleanQueryMacrosQuery(
    cxx::PragmaFunction::QueryInput &input) {
  FTSCatalog catalog(input.GetContext());
  FTSArguments arguments(input);
  if (!catalog.Exists("SELECT 1 FROM duckdb_extensions() WHERE "
                      "extension_name = 'json' AND loaded")) {
    Throw<InvalidInputException>(
        "create_fts_boolean_query_macros requires the json extension to be "
        "loaded first");
  }

  auto qname = GetQualifiedName(catalog, arguments.GetString(0));
  if (!FTSSchemaExists(catalog, qname)) {
    Throw<CatalogException>("a FTS index does not exist on table '%s.%s'. "
                            "Create one with 'PRAGMA create_fts_index()'.",
                            qname.Schema(), qname.Name());
  }
  FTSTableName term_stats_name(qname.Catalog(), GetFTSSchemaName(qname),
                               "term_stats");
  if (!catalog.TableExists(term_stats_name)) {
    Throw<InvalidInputException>(
        "create_fts_boolean_query_macros requires an FTS index created with "
        "layered_search=true");
  }

  FTSTableName raw_term_grams_name(qname.Catalog(), GetFTSSchemaName(qname),
                                   "raw_term_grams");
  FTSTableName non_pattern_search_name(qname.Catalog(), GetFTSSchemaName(qname),
                                       "__search_layered_bm25_non_pattern");
  if (!catalog.TableExists(raw_term_grams_name) ||
      !catalog.TableMacroExists(non_pattern_search_name)) {
    Throw<InvalidInputException>(
        "create_fts_boolean_query_macros requires an FTS index using the "
        "current layered index format; drop and recreate the index with "
        "layered_search=true");
  }

  return FTSIndexBuilder::CreateStructuredQueryMacros(qname);
}

void FTSIndexing::Register(cxx::Extension &extension, cxx::Factory &factory) {
  auto varchar = factory.ParseType("VARCHAR");
  auto boolean = factory.ParseType("BOOLEAN");
  auto any = factory.CreateType(cxx::LogicalTypeId::ANY);
  auto named = cxx::FunctionParameterKind::NAMED_ONLY;
  auto null_value = [&](const cxx::LogicalType &type) {
    return cxx::Value::CreateNull(factory, type);
  };

  auto create_function = cxx::PragmaFunction::Create(factory);
  create_function.SetName("create_fts_index")
      .SetQueryCallback(CreateFTSIndexQuery)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        signature.AddParameter("input_table", varchar)
            .AddParameter("input_id", varchar)
            .AddArgs("input_values", varchar);
        for (auto option : {"stemmer", "tokenizer", "stopwords", "ignore"}) {
          signature.AddParameter(option, varchar, null_value(varchar), named);
        }
        for (auto option :
             {"strip_accents", "lower", "overwrite", "incremental",
              "cluster_terms", "layered_search"}) {
          signature.AddParameter(option, any, null_value(boolean), named);
        }
      });
  extension.Register(create_function);

  auto drop_function = cxx::PragmaFunction::Create(factory);
  drop_function.SetName("drop_fts_index")
      .SetQueryCallback(DropFTSIndexQuery)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        signature.AddParameter("input_table", varchar);
      });
  extension.Register(drop_function);

  auto macros_function = cxx::PragmaFunction::Create(factory);
  macros_function.SetName("create_fts_boolean_query_macros")
      .SetQueryCallback(CreateFTSBooleanQueryMacrosQuery)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        signature.AddParameter("input_table", varchar);
      });
  extension.Register(macros_function);
}

} // namespace duckdb
