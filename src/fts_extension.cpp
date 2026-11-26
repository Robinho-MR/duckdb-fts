#include "duckdb_cpp_extension.hpp"

#include "fts_common.hpp"
#include "fts_extension.hpp"
#include "fts_highlight.hpp"
#include "fts_indexing.hpp"
#include "fts_near.hpp"
#include "fts_opensearch_tokenizer.hpp"
#include "fts_pattern.hpp"
#include "fts_tokenizer.hpp"
#include "fts_unicode_classifier.hpp"
#include "fts_vector.hpp"
#include "libstemmer.h"

namespace duckdb {

static void OpenSearchStandardTokenizeFunction(
    cxx::ScalarFunction::ExecInput &input) {
  auto count = input.GetRowCount();
  FTSInput<cxx::varchar_t> inputs(input.GetArg(0));

  struct Token {
    const char *data;
    uint32_t size;
  };
  vector<Token> tokens;
  vector<cxx::list_entry_t> entries(count);
  vector<bool> is_null(count, false);
  for (idx_t i = 0; i < count; i++) {
    entries[i].offset = tokens.size();
    if (!inputs.IsValid(i)) {
      is_null[i] = true;
      continue;
    }
    const auto &value = inputs.Get(i);
    ScanOpenSearchStandardTokens(value.data(), value.size(),
                                 [&](idx_t start, idx_t size) {
                                   tokens.push_back(
                                       {value.data() + start,
                                        UnsafeNumericCast<uint32_t>(size)});
                                 });
    entries[i].length = tokens.size() - entries[i].offset;
  }

  auto result = input.GetResult();
  result.SetSize(count);
  auto result_entries = result.GetDataMutable<cxx::list_entry_t>();
  FTSNulls nulls(result, count);
  for (idx_t i = 0; i < count; i++) {
    result_entries[i] = entries[i];
    if (is_null[i]) {
      nulls.SetNull(i);
    }
  }
  auto child = result.GetChild(0);
  child.SetSize(tokens.size());
  child.GetValidityMutable().SetAllValid(tokens.size());
  for (idx_t i = 0; i < tokens.size(); i++) {
    child.AssignStringUnsafe(
        i, std::string_view(tokens[i].data, tokens[i].size));
  }
}

static void StemFunction(cxx::ScalarFunction::ExecInput &input) {
  auto count = input.GetRowCount();
  FTSInput<cxx::varchar_t> inputs(input.GetArg(0));
  FTSInput<cxx::varchar_t> stemmers(input.GetArg(1));

  auto result = input.GetResult();
  result.SetSize(count);
  FTSNulls nulls(result, count);
  for (idx_t i = 0; i < count; i++) {
    if (!inputs.IsValid(i) || !stemmers.IsValid(i)) {
      result.AssignString(i, "");
      nulls.SetNull(i);
      continue;
    }
    auto value = inputs.Get(i);
    auto stemmer = string(stemmers.Get(i).view());
    if (stemmer == "none") {
      result.AssignString(i, value.view());
      continue;
    }
    struct sb_stemmer *s = sb_stemmer_new(stemmer.c_str(), "UTF_8");
    if (s == nullptr) {
      const char **stemmers_list = sb_stemmer_list();
      size_t n_stemmers = 0;
      while (stemmers_list[n_stemmers] != nullptr) {
        n_stemmers++;
      }
      Throw<InvalidInputException>(
          "Unrecognized stemmer '%s'. Supported stemmers are: ['%s'], or use "
          "'none' for no stemming",
          stemmer,
          Join(stemmers_list, n_stemmers, "', '",
               [](const char *st) { return string(st); }));
    }
    auto output_data = reinterpret_cast<const char *>(sb_stemmer_stem(
        s, reinterpret_cast<const sb_symbol *>(value.data()), value.size()));
    auto output_size = sb_stemmer_length(s);
    try {
      result.AssignString(
          i, std::string_view(output_data, static_cast<size_t>(output_size)));
    } catch (...) {
      sb_stemmer_delete(s);
      throw;
    }
    sb_stemmer_delete(s);
  }
}

void RegisterFTSScalarFunctions(cxx::Extension &extension,
                                cxx::Factory &factory) {
  auto varchar = factory.ParseType("VARCHAR");
  auto positional = cxx::FunctionParameterKind::POSITIONAL_ONLY;

  auto stem_function = cxx::ScalarFunction::Create(factory);
  stem_function.SetName("stem").SetExecCallback(StemFunction).WithSignature(
      [&](cxx::FunctionSignature &signature) {
        signature.AddParameter("input", varchar, positional)
            .AddParameter("stemmer", varchar, positional)
            .SetReturnType(varchar);
      });
  extension.Register(stem_function);

  auto tokenize_function = cxx::ScalarFunction::Create(factory);
  tokenize_function.SetName("fts_tokenize_opensearch_standard")
      .SetExecCallback(OpenSearchStandardTokenizeFunction)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        signature.AddParameter("input", varchar, positional)
            .SetReturnType(factory.ParseType("VARCHAR[]"));
      });
  extension.Register(tokenize_function);

  RegisterFTSRenderHighlight(extension, factory);
  RegisterFTSTokenize(extension, factory);
  RegisterFTSAnalyzePattern(extension, factory);
  RegisterFTSNearTf(extension, factory);
}

} // namespace duckdb

DUCKDB_CPP_EXTENSION_ENTRYPOINT(duckdb::cxx::Extension &extension,
                                duckdb::cxx::Context &context) {
  auto &factory = context.GetFactory();
  duckdb::RegisterFTSScalarFunctions(extension, factory);
  duckdb::FTSIndexing::Register(extension, factory);
}
