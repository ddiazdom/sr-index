//
// Created by Dustin Cobas <dustin.cobas@gmail.com> on 8/27/20.
//

#ifndef SRI_R_INDEX_H_
#define SRI_R_INDEX_H_

#include <string>
#include <vector>
#include <memory>

#include <sdsl/csa_alphabet_strategy.hpp>

#include "index_base.h"
#include "alphabet.h"
#include "rle_string.hpp"
#include "lf.h"
#include "sequence_ops.h"
#include "construct.h"
#include "config.h"

namespace sri {

    struct nullstream : std::ostream {
        struct nullbuf: std::streambuf {
            int overflow(int c)
            {
                return traits_type::not_eof(c);
            }
            int xputc(int) { return 0; }
            std::streamsize xsputn(char const*, std::streamsize n) { return n; }
            int sync() { return 0; }
        } m_sbuf;
        nullstream(): std::ios(&m_sbuf), std::ostream(&m_sbuf), m_sbuf() {}
    };

template<typename TStorage = GenericStorage,
    typename TAlphabet = Alphabet<>,
    typename TBwtRLE = RLEString<>,
    typename TBvMark = sdsl::sd_vector<>,
    typename TMarkToSampleIdx = sdsl::int_vector<>,
    typename TSample = sdsl::int_vector<>>
class RIndex : public IndexBaseWithExternalStorage<TStorage> {
 public:
  using Base = IndexBaseWithExternalStorage<TStorage>;

  explicit RIndex(const TStorage &t_storage) : Base(t_storage) {}

  RIndex() = default;

  void load(Config t_config) override {
    TSource source(std::ref(t_config));
    loadInner(source);
  }

  void load(std::istream &in) override {
    TSource source(std::ref(in));
    loadInner(source);
  }

  std::vector<std::pair<std::string, size_t>> breakdown() const override {

      std::vector<std::pair<std::string, size_t>> parts;
      auto child = sdsl::structure_tree::add_child(nullptr, "", sdsl::util::class_name(*this));
      nullstream nulls;
      size_t written_bytes;

      written_bytes = this->template serializeItem<TAlphabet>(key(ItemKey::ALPHABET), nulls, child, "alphabet");
      parts.emplace_back("alphabet", written_bytes);

      written_bytes = this->template serializeItem<TBwtRLE>(key(ItemKey::NAVIGATE), nulls, child, "bwt");
      parts.emplace_back("bwt", written_bytes);

      written_bytes = this->template serializeItem<TSample>(key(ItemKey::SAMPLES), nulls, child, "samples");
      parts.emplace_back("samples", written_bytes);

      written_bytes = this->template serializeItem<TBvMark>(key(ItemKey::MARKS), nulls, child, "marks");
      parts.emplace_back("marks", written_bytes);

      written_bytes = this->template serializeRank<TBvMark>(key(ItemKey::MARKS), nulls, child, "marks_rank");
      parts.emplace_back("marks_rank", written_bytes);

      written_bytes = this->template serializeSelect<TBvMark>(key(ItemKey::MARKS), nulls, child, "marks_select");
      parts.emplace_back("marks_select", written_bytes);

      written_bytes = this->template serializeItem<TMarkToSampleIdx>(key(ItemKey::MARK_TO_SAMPLE), nulls, child, "mark_to_sample");
      parts.emplace_back("mark_to_sample", written_bytes);

      return parts;
  }

  using typename Base::size_type;
  using typename Base::ItemKey;
  size_type serialize(std::ostream &out, sdsl::structure_tree_node *v, const std::string &name) const override {
    auto child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));

    size_type written_bytes = 0;
    written_bytes += this->template serializeItem<TAlphabet>(key(ItemKey::ALPHABET), out, child, "alphabet");

    written_bytes += this->template serializeItem<TBwtRLE>(key(ItemKey::NAVIGATE), out, child, "bwt");

    written_bytes += this->template serializeItem<TSample>(key(ItemKey::SAMPLES), out, child, "samples");

    written_bytes += this->template serializeItem<TBvMark>(key(ItemKey::MARKS), out, child, "marks");
    written_bytes += this->template serializeRank<TBvMark>(key(ItemKey::MARKS), out, child, "marks_rank");
    written_bytes += this->template serializeSelect<TBvMark>(key(ItemKey::MARKS), out, child, "marks_select");

    written_bytes +=
        this->template serializeItem<TMarkToSampleIdx>(key(ItemKey::MARK_TO_SAMPLE), out, child, "mark_to_sample");

    return written_bytes;
  }

 protected:

  using typename Base::TSource;

  virtual void loadInner(TSource &t_source) {
    setupKeyNames();
    loadAllItems(t_source);
    constructIndex(t_source);
  }

  using Base::key;
  virtual void setupKeyNames() {
    key(ItemKey::ALPHABET) = conf::KEY_ALPHABET;
    key(ItemKey::NAVIGATE) = conf::KEY_BWT_RLE;
    key(ItemKey::SAMPLES) = conf::KEY_BWT_RUN_LAST_TEXT_POS;
    key(ItemKey::MARKS) = conf::KEY_BWT_RUN_FIRST_TEXT_POS;
    key(ItemKey::MARK_TO_SAMPLE) = conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_TO_LAST_IDX;
  }

  virtual void loadAllItems(TSource &t_source) {
    this->template loadItem<TAlphabet>(key(ItemKey::ALPHABET), t_source);

    this->template loadItem<TBwtRLE>(key(ItemKey::NAVIGATE), t_source);

    this->template loadItem<TSample>(key(ItemKey::SAMPLES), t_source);

    this->template loadItem<TBvMark>(key(ItemKey::MARKS), t_source, true);
    this->template loadBVRank<TBvMark>(key(ItemKey::MARKS), t_source, true);
    this->template loadBVSelect<TBvMark>(key(ItemKey::MARKS), t_source, true);

    this->template loadItem<TMarkToSampleIdx>(key(ItemKey::MARK_TO_SAMPLE), t_source);
  }

  virtual void constructIndex(TSource &t_source) {
    this->index_.reset(new RIndexBase{
        constructLF(t_source),
        constructComputeDataBackwardSearchStep(t_source, constructCreateDataBackwardSearchStep()),
        constructComputeSAValues(constructPhiForRange(t_source), constructComputeToehold(t_source)),
        this->n_,
        [](const auto &tt_step) { return DataBackwardSearchStep{0, RunData{0, 0}}; },
        constructGetSymbol(t_source),
        [](auto tt_seq_size) { return Range{0, tt_seq_size}; },
        constructIsRangeEmpty()
    });
  }

  struct DataLF {
    std::size_t value = 0;

    struct Run {
      std::size_t rank = 0; // Rank of most-right run for the given symbol, that starts before the given position
      bool is_cover = false; // Run covers the original position (the  position matches the symbol for LF operation)
    } run;

    bool operator==(const DataLF &rhs) const {
      return value == rhs.value; // && run.rank == rhs.run.rank && run.is_cover == rhs.run.is_cover;
    }

    bool operator<(const DataLF &rhs) const {
      return value < rhs.value;
    }
  };

  using Position = std::size_t;
  struct RangeLF;

  struct Range {
    Position start;
    Position end;

    Range &operator=(const RangeLF &t_range) {
      start = t_range.start.value;
      end = t_range.end.value;
      return *this;
    }
  };

  struct RangeLF {
    DataLF start;
    DataLF end;
  };

  auto constructIsRangeEmpty() {
    return [](const Range &tt_range) { return !(tt_range.start < tt_range.end); };
  }

  auto constructLF(TSource &t_source) {
    auto cref_alphabet = this->template loadItem<TAlphabet>(key(ItemKey::ALPHABET), t_source);
    auto cumulative = RandomAccessForCRefContainer(std::cref(cref_alphabet.get().C));
    this->n_ = cumulative[cref_alphabet.get().sigma];

    auto cref_bwt_rle = this->template loadItem<TBwtRLE>(key(ItemKey::NAVIGATE), t_source);
    auto bwt_rank = [cref_bwt_rle](const auto &tt_c, const auto &tt_pos) {
      DataLF data;
      auto report = [&data](const auto &tt_rank, const auto &tt_run_rank, const auto &tt_is_cover) {
        data = DataLF{tt_rank, {tt_run_rank, tt_is_cover}};
      };
      cref_bwt_rle.get().rank(tt_pos, tt_c, report);
      return data;
    };

    auto create_range = [](auto tt_c_before_sp, auto tt_c_until_ep, const auto &tt_smaller_c) -> RangeLF {
      tt_c_before_sp.value += tt_smaller_c;
      tt_c_until_ep.value += tt_smaller_c - !tt_c_until_ep.run.is_cover + 1;
      return {tt_c_before_sp, tt_c_until_ep};
    };

    RangeLF empty_range;

    auto lf = LF(bwt_rank, cumulative, create_range, empty_range);
    return [lf](const Range &tt_range, const Char &tt_c) { return lf(tt_range.start, tt_range.end - 1, tt_c); };
  }

  using Char = typename TAlphabet::comp_char_type;

  struct RunData {
    Char c; // Character for LF step in the range
    std::size_t last_run_rnk; // Rank of the last run

    RunData(Char t_c, std::size_t t_end_run_rnk) : c{t_c}, last_run_rnk{t_end_run_rnk} {}
    virtual ~RunData() = default;
  };

  using DataBackwardSearchStep = sri::DataBackwardSearchStep<RunData>;

  auto constructCreateDataBackwardSearchStep() {
    return [](const auto &tt_range, const auto &tt_c, const auto &tt_next_range, const auto &tt_step) {
      const auto &[next_start, next_end] = tt_next_range;
      return DataBackwardSearchStep{tt_step, RunData{tt_c, next_end.run.rank}};
    };
  }

  template<typename TCreateDataBackwardSearchStep>
  auto constructComputeDataBackwardSearchStep(TSource &t_source, const TCreateDataBackwardSearchStep &tt_create_data) {
    auto is_lf_trivial = [](const auto &tt_range, const auto &tt_c, const auto &tt_next_range) {
      const auto &[next_start, next_end] = tt_next_range;
      return !(next_start < next_end) || next_end.run.is_cover;
    };

    return ComputeDataBackwardSearchStep(is_lf_trivial, tt_create_data);
  }

  auto constructComputeToehold(TSource &t_source) {
    auto cref_bwt_rle = this->template loadItem<TBwtRLE>(key(ItemKey::NAVIGATE), t_source);

    auto cref_samples = this->template loadItem<TSample>(key(ItemKey::SAMPLES), t_source);
    auto get_sa_value_for_run_data = [cref_bwt_rle, cref_samples](const RunData &tt_run_data) {
      auto run = cref_bwt_rle.get().selectOnRuns(tt_run_data.last_run_rnk, tt_run_data.c);
      return cref_samples.get()[run] + 1;
    };

    return ComputeToehold(get_sa_value_for_run_data, cref_bwt_rle.get().size());
  }

  auto constructGetMarkToSampleIdx(TSource &t_source, bool t_default_validity) {
    auto cref_mark_to_sample_idx = this->template loadItem<TMarkToSampleIdx>(key(ItemKey::MARK_TO_SAMPLE), t_source);
    return RandomAccessForTwoContainersDefault(cref_mark_to_sample_idx, t_default_validity);
  }

  template<typename TGetMarkToSampleIdx>
  auto constructPhi(TSource &t_source, const TGetMarkToSampleIdx &t_get_mark_to_sample_idx) {
    return constructPhi(t_source, t_get_mark_to_sample_idx, SampleValidatorDefault());
  }

  template<typename TGetMarkToSampleIdx, typename TSampleValidator>
  auto constructPhi(TSource &t_source,
                    const TGetMarkToSampleIdx &t_get_mark_to_sample_idx,
                    const TSampleValidator &t_sample_validator) {
    auto bv_mark_rank = this->template loadBVRank<TBvMark>(key(ItemKey::MARKS), t_source, true);
    auto bv_mark_select = this->template loadBVSelect<TBvMark>(key(ItemKey::MARKS), t_source, true);
    auto predecessor = CircularPredecessor(bv_mark_rank, bv_mark_select, this->n_);

    auto cref_samples = this->template loadItem<TSample>(key(ItemKey::SAMPLES), t_source);
    auto get_sample = RandomAccessForCRefContainer(cref_samples);

    return buildPhiBackward(predecessor, t_get_mark_to_sample_idx, get_sample, t_sample_validator, this->n_);
  }

  auto constructPhiForRange(TSource &t_source) {
    auto phi = constructPhi(t_source, constructGetMarkToSampleIdx(t_source, true));
    auto phi_for_range = [phi](const auto &t_range, std::size_t t_k, auto t_report) {
      auto k = t_k;
      const auto &[start, end] = t_range;
      for (auto i = start; i < end; ++i) {
        k = phi(k).first;
        t_report(k);
      }
    };

    return phi_for_range;
  }

  template<typename TPhiRange, typename TComputeToehold>
  auto constructComputeSAValues(const TPhiRange &t_phi_range, const TComputeToehold &t_compute_toehold) {
    auto update_range = [](Range tt_range) {
      auto &[start, end] = tt_range;
      --end;
      return tt_range;
    };

    return ComputeAllValuesWithPhiForRange(t_phi_range, t_compute_toehold, update_range);
  }

  auto constructGetSymbol(TSource &t_source) {
    auto cref_alphabet = this->template loadItem<TAlphabet>(key(ItemKey::ALPHABET), t_source);

    auto get_symbol = [cref_alphabet](char tt_c) { return cref_alphabet.get().char2comp[(uint8_t) tt_c]; };
    return get_symbol;
  }

};

template<uint8_t t_width, typename TBvMark>
void constructRIndex(const std::string &t_data_path, sri::Config &t_config);

template<typename TStorage, template<uint8_t> typename TAlphabet, uint8_t t_width, typename TBwtRLE, typename TBvMark, typename TMarkToSampleIdx, typename TSample>
void construct(RIndex<TStorage, TAlphabet<t_width>, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample> &t_index,
               const std::string &t_data_path,
               sri::Config &t_config) {

  constructRIndex<t_width, TBvMark>(t_data_path, t_config);

  t_index.load(t_config);
}

template<uint8_t t_width, typename TBvMark>
void constructRIndex(const std::string &t_data_path, sri::Config &t_config) {
  constructIndexBaseItems<t_width>(t_data_path, t_config);

  {
    // Construct Links from Mark to Sample
    auto event = sdsl::memory_monitor::event("Mark2Sample Links");
    if (!cache_file_exists(conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_TO_LAST_IDX, t_config)) {
      constructMarkToSampleLinksForPhiBackward(t_config);
    }
  }

  std::size_t n;
  {
    sdsl::int_vector_buffer<t_width> bwt_buf(sdsl::cache_file_name(sdsl::key_bwt_trait<t_width>::KEY_BWT, t_config));
    n = bwt_buf.size();
  }

  {
    // Construct Predecessor on the text positions of BWT run first letter
    auto event = sdsl::memory_monitor::event("Predecessor");
    const auto key_marks = conf::KEY_BWT_RUN_FIRST_TEXT_POS;
    if (!sdsl::cache_file_exists<TBvMark>(key_marks, t_config)) {
      constructBitVectorFromIntVector<TBvMark>(key_marks, t_config, n, false);
    }
  }
}

}

#endif //SRI_R_INDEX_H_
