#include "fts_highlight.hpp"

#include "fts_vector.hpp"


#include <algorithm>

namespace duckdb {

namespace {

struct HighlightSpan {
  uint32_t start;
  uint32_t end;
};

// Overlapping or touching spans render as one, the way FTS5 merges a phrase
// with a term inside it or with an overlapping phrase.
static void MergeSpans(vector<HighlightSpan> &spans) {
  std::sort(spans.begin(), spans.end(),
            [](const HighlightSpan &a, const HighlightSpan &b) {
              return a.start < b.start || (a.start == b.start && a.end < b.end);
            });
  idx_t merged = 0;
  for (idx_t i = 0; i < spans.size(); i++) {
    if (merged > 0 && spans[i].start <= spans[merged - 1].end) {
      spans[merged - 1].end = MaxValue(spans[merged - 1].end, spans[i].end);
    } else {
      spans[merged++] = spans[i];
    }
  }
  spans.resize(merged);
}

} // namespace

static void RenderHighlightFunction(cxx::ScalarFunction::ExecInput &input) {
  auto count = input.GetRowCount();
  FTSInput<cxx::varchar_t> texts(input.GetArg(0));
  FTSListInput<uint32_t> start_lists(input.GetArg(1));
  FTSListInput<uint32_t> end_lists(input.GetArg(2));
  FTSInput<cxx::varchar_t> befores(input.GetArg(3));
  FTSInput<cxx::varchar_t> afters(input.GetArg(4));
  FTSInput<uint32_t> window_froms(input.GetArg(5));
  FTSInput<uint32_t> window_tos(input.GetArg(6));
  FTSInput<bool> leadings(input.GetArg(7));
  FTSInput<bool> trailings(input.GetArg(8));
  FTSInput<cxx::varchar_t> ellipses(input.GetArg(9));

  auto result = input.GetResult();
  result.SetSize(count);
  FTSNulls nulls(result, count);
  for (idx_t i = 0; i < count; i++) {
    if (!texts.IsValid(i) || !start_lists.IsValid(i) ||
        !end_lists.IsValid(i) || !befores.IsValid(i) || !afters.IsValid(i) ||
        !window_froms.IsValid(i) || !window_tos.IsValid(i) ||
        !leadings.IsValid(i) || !trailings.IsValid(i) ||
        !ellipses.IsValid(i)) {
      nulls.SetNull(i);
      continue;
    }
    auto text = texts.Get(i);
    auto window_from = window_froms.Get(i);
    auto window_to =
        MinValue(window_tos.Get(i), UnsafeNumericCast<uint32_t>(text.size()));
    window_from = MinValue(window_from, window_to);

    vector<uint32_t> span_starts;
    vector<uint32_t> span_ends;
    span_starts.reserve(start_lists.Length(i));
    span_ends.reserve(end_lists.Length(i));
    for (idx_t j = 0; j < start_lists.Length(i); j++) {
      span_starts.push_back(start_lists.ElementIsValid(i, j)
                                ? start_lists.Element(i, j)
                                : window_to);
    }
    for (idx_t j = 0; j < end_lists.Length(i); j++) {
      span_ends.push_back(end_lists.ElementIsValid(i, j)
                              ? end_lists.Element(i, j)
                              : window_from);
    }
    vector<HighlightSpan> spans;
    auto span_count = MinValue<idx_t>(span_starts.size(), span_ends.size());
    for (idx_t j = 0; j < span_count; j++) {
      auto span_start = MaxValue(span_starts[j], window_from);
      auto span_end = MinValue(span_ends[j], window_to);
      if (span_start < span_end) {
        spans.push_back({span_start, span_end});
      }
    }
    MergeSpans(spans);

    auto data = text.data();
    auto before = string(befores.Get(i).view());
    auto after = string(afters.Get(i).view());
    auto ellipsis = string(ellipses.Get(i).view());
    string rendered;
    if (leadings.Get(i)) {
      rendered += ellipsis;
    }
    auto cursor = window_from;
    for (auto &span : spans) {
      rendered.append(data + cursor, span.start - cursor);
      rendered += before;
      rendered.append(data + span.start, span.end - span.start);
      rendered += after;
      cursor = span.end;
    }
    rendered.append(data + cursor, window_to - cursor);
    if (trailings.Get(i)) {
      rendered += ellipsis;
    }
    // cutting the text can split a character: functions that decode check
    result.AssignStringUnsafe(i, rendered);
  }
}

void RegisterFTSRenderHighlight(cxx::Extension &extension,
                                cxx::Factory &factory) {
  auto function = cxx::ScalarFunction::Create(factory);
  function.SetName("fts_render_highlight")
      .SetExecCallback(RenderHighlightFunction)
      .SetNullHandling(cxx::FunctionNullHandling::SPECIAL)
      .SetFallibility(cxx::FunctionFallibility::INFALLIBLE)
      .WithSignature([&](cxx::FunctionSignature &signature) {
        using cxx::FunctionParameterKind;
        auto varchar = factory.ParseType("VARCHAR");
        auto uinteger = factory.ParseType("UINTEGER");
        auto boolean = factory.ParseType("BOOLEAN");
        auto uinteger_list = factory.ParseType("UINTEGER[]");
        auto positional = FunctionParameterKind::POSITIONAL_ONLY;
        signature.AddParameter("text", varchar, positional)
            .AddParameter("starts", uinteger_list, positional)
            .AddParameter("ends", uinteger_list, positional)
            .AddParameter("before", varchar, positional)
            .AddParameter("after", varchar, positional)
            .AddParameter("window_from", uinteger, positional)
            .AddParameter("window_to", uinteger, positional)
            .AddParameter("leading", boolean, positional)
            .AddParameter("trailing", boolean, positional)
            .AddParameter("ellipsis", varchar, positional)
            .SetReturnType(varchar);
      });
  extension.Register(function);
}

} // namespace duckdb
