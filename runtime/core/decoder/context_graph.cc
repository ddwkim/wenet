// Copyright (c) 2021 Mobvoi Inc (Zhendong Peng)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "decoder/context_graph.h"

#include <utility>

#include "fst/determinize.h"

#include "utils/string.h"
#include "utils/utils.h"

namespace wenet {

ContextGraph::ContextGraph(ContextConfig config) : config_(config) {}

void ContextGraph::BuildContextGraph(
    const std::vector<std::string>& query_contexts,
    const std::shared_ptr<fst::SymbolTable>& symbol_table) {
  CHECK(symbol_table != nullptr) << "Symbols table should not be nullptr!";
  start_tag_id_ = symbol_table->AddSymbol("<context>");
  end_tag_id_ = symbol_table->AddSymbol("</context>");
  symbol_table_ = symbol_table;
  if (query_contexts.empty()) {
    if (graph_ != nullptr) graph_.reset();
    return;
  }

  std::unique_ptr<fst::StdVectorFst> ofst(new fst::StdVectorFst());
  // State 0 is the start state and the final state.
  int start_state = ofst->AddState();
  ofst->SetStart(start_state);
  ofst->SetFinal(start_state, fst::StdArc::Weight::One());

  LOG(INFO) << "Contexts count size: " << query_contexts.size();
  int count = 0;
  for (const auto& context : query_contexts) {
    if (context.size() > config_.max_context_length) {
      LOG(INFO) << "Skip long context: " << context;
      continue;
    }
    if (++count > config_.max_contexts) break;

    std::vector<std::string> words;
    // Split context to words by symbol table, and build the context graph.
    bool no_oov = SplitUTF8StringToWords(Trim(context), symbol_table, &words);
    if (!no_oov) {
      LOG(WARNING) << "Ignore unknown word found during compilation.";
      continue;
    }

    int prev_state = start_state;
    int next_state = start_state;
    float escape_score = 0;
    for (size_t i = 0; i < words.size(); ++i) {
      int word_id = symbol_table_->Find(words[i]);
      float score =
          (i * config_.incremental_context_score + config_.context_score) *
          UTF8StringLength(words[i]);
      if (IsAlpha(words[i]) || words[i][0] == kSpaceSymbol[0]) {
        score = i * config_.incremental_context_score + config_.context_score;
      }
      next_state = (i < words.size() - 1) ? ofst->AddState() : start_state;
      ofst->AddArc(prev_state,
                   fst::StdArc(word_id, word_id, score, next_state));
      // Add escape arc to clean the previous context score.
      if (i > 0) {
        // ilabel and olabel of the escape arc is 0 (<epsilon>).
        ofst->AddArc(prev_state, fst::StdArc(0, 0, -escape_score, start_state));
      }
      prev_state = next_state;
      escape_score += score;
    }
  }
  std::unique_ptr<fst::StdVectorFst> det_fst(new fst::StdVectorFst());
  fst::Determinize(*ofst, det_fst.get());
  graph_ = std::move(det_fst);
}

int ContextGraph::GetNextState(int cur_state, int word_id, float* score,
                               bool* is_start_boundary, bool* is_end_boundary) {
  int next_state = 0;
  for (fst::ArcIterator<fst::StdFst> aiter(*graph_, cur_state); !aiter.Done();
       aiter.Next()) {
    const fst::StdArc& arc = aiter.Value();
    if (arc.ilabel == 0) {
      // escape score, will be overwritten when ilabel equals to word id.
      *score = arc.weight.Value();
    } else if (arc.ilabel == word_id) {
      next_state = arc.nextstate;
      *score = arc.weight.Value();
      if (cur_state == 0) {
        *is_start_boundary = true;
      }
      if (graph_->Final(arc.nextstate) == fst::StdArc::Weight::One()) {
        *is_end_boundary = true;
      }
      break;
    }
  }
  if (next_state != 0) {
    return next_state;
  }
  for (fst::ArcIterator<fst::StdFst> aiter(*graph_, 0); !aiter.Done();
       aiter.Next()) {
    const fst::StdArc& arc = aiter.Value();
    if (arc.ilabel == word_id) {
      next_state = arc.nextstate;
      *score += arc.weight.Value();
      if (cur_state == 0) {
        *is_start_boundary = true;
      }
      if (graph_->Final(arc.nextstate) == fst::StdArc::Weight::One()) {
        *is_end_boundary = true;
      }
      break;
    }
  }
  return next_state;
}

bool ContextGraph::SplitUTF8StringToWords(
    const std::string& str,
    const std::shared_ptr<fst::SymbolTable>& symbol_table,
    std::vector<std::string>* words) {
  std::vector<std::string> chars;
  SplitUTF8StringToChars(Trim(str), &chars);

  bool no_oov = true;
  bool beginning = true;
  for (size_t start = 0; start < chars.size();) {
    for (size_t end = chars.size(); end > start; --end) {
      std::string word;
      for (size_t i = start; i < end; i++) {
        word += chars[i];
      }
      // Skip space.
      if (word == " ") {
        start = end;
        beginning = true;
        continue;
      }
      // Add '▁' at the beginning of English word.
      if (IsAlpha(word) && beginning) {
        word = kSpaceSymbol + word;
      }

      if (symbol_table->Find(word) != -1) {
        words->emplace_back(word);
        start = end;
        beginning = false;
        continue;
      }

      if (end == start + 1) {
        // Matching using '▁' separately for English
        if (word[0] == kSpaceSymbol[0]) {
          words->emplace_back(string(kSpaceSymbol));
          beginning = false;
          break;
        }
        ++start;
        no_oov = false;
        LOG(WARNING) << word << " is oov.";
      }
    }
  }
  return no_oov;
}

}  // namespace wenet
