#include "fts_tokenizer.hpp"

#include "fts_vector.hpp"

#include "fts_opensearch_tokenizer.hpp"
#include "re2/re2.h"
#include "utf8proc.hpp"

#include <cassert>
#include <deque>
#include <memory>
#include <type_traits>

namespace duckdb {

int32_t Utf8Proc::CodepointToLower(int32_t codepoint) {
  return utf8proc_tolower(codepoint);
}

namespace {

// One entry per normalized codepoint or contiguous run with unchanged byte
// widths.
struct NormalizedPiece {
  uint32_t norm_start;
  uint32_t norm_size;
  uint32_t orig_start;
  uint32_t orig_size;
};

struct NormalizedText {
  string text;
  vector<NormalizedPiece> pieces;
  idx_t source_size;
};

static bool IsASCII(const char *data, idx_t size) {
  for (idx_t i = 0; i < size; i++) {
    if (data[i] & 0x80) {
      return false;
    }
  }
  return true;
}

template <bool WITH_OFFSETS>
static NormalizedText NormalizeWithMap(const char *data, idx_t size,
                                       bool strip_accents, bool lower) {
  NormalizedText result;
  result.source_size = size;
  if (IsASCII(data, size)) {
    // ASCII normalization preserves every byte boundary. Copy/fold the entire
    // string rather than decoding and encoding each byte as a codepoint.
    result.text.assign(data, size);
    if (lower) {
      for (auto &c : result.text) {
        c = ToLowerASCII(c);
      }
    }
    if (WITH_OFFSETS && size > 0) {
      result.pieces.push_back({0, UnsafeNumericCast<uint32_t>(size), 0,
                               UnsafeNumericCast<uint32_t>(size)});
    }
    return result;
  }
  // Analyzer-version compatibility: SQL strip_accents truncates non-ASCII
  // inputs at NUL, even when the non-ASCII character follows that NUL.
  if (strip_accents) {
    auto nul = static_cast<const char *>(memchr(data, 0, size));
    if (nul) {
      size = UnsafeNumericCast<idx_t>(nul - data);
      result.source_size = size;
    }
  }
  result.text.reserve(size);
  const auto options = utf8proc_option_t(UTF8PROC_STABLE | UTF8PROC_COMPOSE |
                                         UTF8PROC_STRIPMARK);
  int32_t pending = -1;
  uint32_t pending_start = 0;
  uint32_t pending_end = 0;

  auto flush = [&]() {
    if (pending < 0) {
      return;
    }
    auto codepoint = lower ? Utf8Proc::CodepointToLower(pending) : pending;
    char encoded[4];
    int encoded_size = 0;
    if (!Utf8Proc::CodepointToUtf8(codepoint, encoded_size, encoded)) {
      throw InvalidInputException("Invalid codepoint in FTS normalization");
    }
    auto width = UnsafeNumericCast<uint32_t>(encoded_size);
    if (WITH_OFFSETS) {
      bool extended = false;
      if (!result.pieces.empty()) {
        auto &run = result.pieces.back();
        if (run.norm_size == run.orig_size &&
            width == pending_end - pending_start &&
            run.orig_start + run.orig_size == pending_start) {
          run.norm_size += width;
          run.orig_size += width;
          extended = true;
        }
      }
      if (!extended) {
        result.pieces.push_back(
            {UnsafeNumericCast<uint32_t>(result.text.size()), width,
             pending_start, pending_end - pending_start});
      }
    }
    result.text.append(encoded, width);
  };

  auto append = [&](int32_t codepoint, uint32_t start, uint32_t end) {
    if (strip_accents) {
      // STRIPMARK removes every nonzero combining class, so no canonical
      // reordering remains. Let utf8proc compose adjacent starters.
      assert(utf8proc_get_property(codepoint)->combining_class == 0);
      if (pending >= 0 && codepoint >= 128) {
        int32_t pair[] = {pending, codepoint};
        if (utf8proc_normalize_utf32(pair, 2, options) == 1) {
          pending = pair[0];
          pending_end = end;
          return;
        }
      }
    }
    flush();
    pending = codepoint;
    pending_start = start;
    pending_end = end;
  };

  vector<int32_t> decomposition;
  for (idx_t pos = 0; pos < size;) {
    int char_size = 1;
    auto codepoint = int32_t(static_cast<unsigned char>(data[pos]));
    if (codepoint >= 128) {
      codepoint = Utf8Proc::UTF8ToCodepoint(data + pos, char_size, size - pos);
      if (codepoint < 0 || char_size <= 0) {
        throw InvalidInputException("Invalid UTF-8 in FTS input");
      }
    }
    auto start = UnsafeNumericCast<uint32_t>(pos);
    pos += UnsafeNumericCast<idx_t>(char_size);
    auto end = UnsafeNumericCast<uint32_t>(pos);
    if (!strip_accents || codepoint < 128) {
      append(codepoint, start, end);
      continue;
    }
    int32_t buffer[4];
    auto count =
        utf8proc_decompose_char(codepoint, buffer, 4, options, nullptr);
    if (count < 0) {
      throw InvalidInputException(string(utf8proc_errmsg(count)));
    }
    if (count == 0) {
      // A removed mark still belongs to the original span of its base.
      pending_end = end;
      continue;
    }
    auto decomposed = buffer;
    if (count > 4) {
      decomposition.resize(UnsafeNumericCast<idx_t>(count));
      utf8proc_decompose_char(codepoint, decomposition.data(), count, options,
                              nullptr);
      decomposed = decomposition.data();
    }
    for (utf8proc_ssize_t i = 0; i < count; i++) {
      append(decomposed[i], start, end);
    }
  }
  flush();
  return result;
}

static const NormalizedPiece *PieceContaining(const NormalizedText &normalized,
                                              uint32_t norm_position) {
  auto it = std::upper_bound(normalized.pieces.begin(), normalized.pieces.end(),
                             norm_position,
                             [](uint32_t value, const NormalizedPiece &piece) {
                               return value < piece.norm_start;
                             });
  if (it == normalized.pieces.begin()) {
    return nullptr;
  }
  return &*(it - 1);
}

struct TokenSpan {
  uint32_t norm_start;
  uint32_t norm_size;
};

static bool IsUtf8CharacterStart(char c) { return (c & 0xC0) != 0x80; }

struct CachedRegex {
  string delimiter;
  std::unique_ptr<duckdb_re2::RE2> regex;

  duckdb_re2::RE2 &Get(const string &pattern) {
    if (!regex || delimiter != pattern) {
      duckdb_re2::StringPiece pattern_piece(pattern);
      auto compiled = std::make_unique<duckdb_re2::RE2>(pattern_piece);
      if (!compiled->ok()) {
        throw InvalidInputException(string(compiled->error()));
      }
      delimiter = pattern;
      regex = std::move(compiled);
    }
    return *regex;
  }
};

struct TokenizeLocalState {
  CachedRegex regex;
};

// Mirrors StringSplitter::Split in DuckDB's string_split.cpp, spans included.
template <class EMIT>
static void SplitRegexWithSpans(const string &text, duckdb_re2::RE2 &regex,
                                EMIT &&emit) {
  auto base = text.data();
  auto input_data = base;
  auto input_size = UnsafeNumericCast<idx_t>(text.size());
  while (input_size > 0) {
    duckdb_re2::StringPiece match;
    if (!regex.Match(duckdb_re2::StringPiece(input_data, input_size), 0,
                     input_size, duckdb_re2::RE2::UNANCHORED, &match, 1)) {
      break;
    }
    auto match_size = UnsafeNumericCast<idx_t>(match.size());
    auto pos = UnsafeNumericCast<idx_t>(match.data() - input_data);
    if (match_size == 0 && pos == 0) {
      for (pos++; pos < input_size; pos++) {
        if (IsUtf8CharacterStart(input_data[pos])) {
          break;
        }
      }
      if (pos == input_size) {
        break;
      }
    }
    emit(UnsafeNumericCast<idx_t>(input_data - base), pos);
    input_data += pos + match_size;
    input_size -= pos + match_size;
  }
  emit(UnsafeNumericCast<idx_t>(input_data - base), input_size);
}

struct MappedToken {
  uint32_t norm_start;
  uint32_t norm_size;
  uint32_t orig_start;
  uint32_t orig_end;
};

static uint32_t MapWithinPiece(const NormalizedPiece &piece,
                               uint32_t norm_position) {
  // Inside a one-to-one run the mapping is exact; a width-changing codepoint
  // rounds to its original start.
  if (piece.norm_size == piece.orig_size) {
    return piece.orig_start + (norm_position - piece.norm_start);
  }
  return piece.orig_start;
}

static MappedToken MapSpan(const NormalizedText &normalized, TokenSpan span) {
  MappedToken token{span.norm_start, span.norm_size, 0, 0};
  if (span.norm_size == 0) {
    auto piece = span.norm_start < normalized.text.size()
                     ? PieceContaining(normalized, span.norm_start)
                     : nullptr;
    auto boundary = piece ? MapWithinPiece(*piece, span.norm_start)
                          : UnsafeNumericCast<uint32_t>(normalized.source_size);
    token.orig_start = boundary;
    token.orig_end = boundary;
    return token;
  }
  auto first = PieceContaining(normalized, span.norm_start);
  auto last = PieceContaining(normalized, span.norm_start + span.norm_size - 1);
  assert(first && last);
  token.orig_start = MapWithinPiece(*first, span.norm_start);
  token.orig_end = last->norm_size == last->orig_size
                       ? last->orig_start + (span.norm_start + span.norm_size -
                                             last->norm_start)
                       : last->orig_start + last->orig_size;
  return token;
}

//! The tokens of the rows of a chunk, which all point into `texts`
struct TokenizedRows {
  std::deque<string> texts;
  vector<TokenSpan> spans;
  vector<MappedToken> mapped;
  vector<const string *> owners;
};

} // namespace

template <bool WITH_OFFSETS>
static void TokenizeFunction(cxx::ScalarFunction::ExecInput &input) {
  auto count = input.GetRowCount();
  FTSInput<cxx::varchar_t> inputs(input.GetArg(0));
  FTSInput<cxx::varchar_t> tokenizers(input.GetArg(1));
  FTSInput<cxx::varchar_t> ignores(input.GetArg(2));
  FTSInput<bool> strip_flags(input.GetArg(3));
  FTSInput<bool> lower_flags(input.GetArg(4));

  auto &cached_regex = input.GetInitData<TokenizeLocalState>().regex;
  TokenizedRows rows;
  struct ListEntry {
    idx_t offset = 0;
    idx_t length = 0;
  };
  vector<ListEntry> entries(count);
  vector<bool> is_null(count, false);
  for (idx_t i = 0; i < count; i++) {
    entries[i].offset = rows.spans.size();
    if (!inputs.IsValid(i) || !tokenizers.IsValid(i) || !ignores.IsValid(i) ||
        !strip_flags.IsValid(i) || !lower_flags.IsValid(i)) {
      is_null[i] = true;
      continue;
    }
    auto input_value = inputs.Get(i);
    // One owned copy of the normalized text; tokens reference its substrings
    rows.texts.emplace_back();
    auto normalized = NormalizeWithMap<WITH_OFFSETS>(
        input_value.data(), input_value.size(), strip_flags.Get(i),
        lower_flags.Get(i));
    rows.texts.back() = normalized.text;
    auto owner = &rows.texts.back();
    auto emit = [&](idx_t start, idx_t size) {
      TokenSpan span{UnsafeNumericCast<uint32_t>(start),
                     UnsafeNumericCast<uint32_t>(size)};
      rows.spans.push_back(span);
      rows.owners.push_back(owner);
      if (WITH_OFFSETS) {
        rows.mapped.push_back(MapSpan(normalized, span));
      }
    };
    if (string(tokenizers.Get(i).view()) == "opensearch_standard") {
      ScanOpenSearchStandardTokens(normalized.text.data(),
                                   normalized.text.size(), emit);
    } else {
      auto delimiter = "(" + string(ignores.Get(i).view()) + ")|\\s+";
      SplitRegexWithSpans(normalized.text, cached_regex.Get(delimiter), emit);
    }
    entries[i].length = rows.spans.size() - entries[i].offset;
  }

  auto result = input.GetResult();
  result.SetSize(count);
  auto result_entries = result.GetDataMutable<cxx::list_entry_t>();
  FTSNulls nulls(result, count);
  for (idx_t i = 0; i < count; i++) {
    result_entries[i] = {entries[i].offset, entries[i].length};
    if (is_null[i]) {
      nulls.SetNull(i);
    }
  }
  auto child = result.GetChild(0);
  child.SetSize(rows.spans.size());
  child.GetValidityMutable().SetAllValid(rows.spans.size());
  if (WITH_OFFSETS) {
    auto start_column = child.GetChild(1);
    auto end_column = child.GetChild(2);
    start_column.GetValidityMutable().SetAllValid(rows.spans.size());
    end_column.GetValidityMutable().SetAllValid(rows.spans.size());
    auto start_offsets = start_column.template GetDataMutable<uint32_t>();
    auto end_offsets = end_column.template GetDataMutable<uint32_t>();
    for (idx_t j = 0; j < rows.spans.size(); j++) {
      start_offsets[j] = rows.mapped[j].orig_start;
      end_offsets[j] = rows.mapped[j].orig_end;
    }
    child.GetChild(0).GetValidityMutable().SetAllValid(rows.spans.size());
  }
  auto raw_terms = WITH_OFFSETS ? child.GetChild(0) : result.GetChild(0);
  for (idx_t j = 0; j < rows.spans.size(); j++) {
    raw_terms.AssignStringUnsafe(
        j, std::string_view(rows.owners[j]->data() + rows.spans[j].norm_start,
                            rows.spans[j].norm_size));
  }
}

template <bool WITH_OFFSETS>
static void RegisterTokenize(cxx::Extension &extension, cxx::Factory &factory) {
  auto function = cxx::ScalarFunction::Create(factory);
  function.SetName(WITH_OFFSETS ? "fts_tokenize_spans" : "fts_tokenize")
      .SetExecCallback(TokenizeFunction<WITH_OFFSETS>)
      .SetInitCallback([](cxx::ScalarFunction::InitInput &input) {
        input.SetInitData<TokenizeLocalState>();
      })
      .SetNullHandling(cxx::FunctionNullHandling::SPECIAL)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        auto varchar = factory.ParseType("VARCHAR");
        auto boolean = factory.ParseType("BOOLEAN");
        auto positional = cxx::FunctionParameterKind::POSITIONAL_ONLY;
        signature.AddParameter("input", varchar, positional)
            .AddParameter("tokenizer", varchar, positional)
            .AddParameter("ignore", varchar, positional)
            .AddParameter("strip_accents", boolean, positional)
            .AddParameter("lower", boolean, positional)
            .SetReturnType(factory.ParseType(
                WITH_OFFSETS
                    ? "STRUCT(raw_term VARCHAR, start_offset UINTEGER, "
                      "end_offset UINTEGER)[]"
                    : "VARCHAR[]"));
      });
  extension.Register(function);
}

void RegisterFTSTokenize(cxx::Extension &extension, cxx::Factory &factory) {
  RegisterTokenize<false>(extension, factory);
  RegisterTokenize<true>(extension, factory);
}

} // namespace duckdb
