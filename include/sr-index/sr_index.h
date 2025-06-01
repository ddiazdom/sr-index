//
// Created by Dustin Cobas <dustin.cobas@gmail.com> on 1/19/22.
//

#ifndef SRI_SR_INDEX_H_
#define SRI_SR_INDEX_H_

#include <cstdint>

#include "r_index.h"
#include "sampling.h"

namespace sri {

template<typename TStorage = GenericStorage,
    typename TAlphabet = Alphabet<>,
    typename TBwtRLE = RLEString<>,
    typename TBvMark = sdsl::sd_vector<>,
    typename TMarkToSampleIdx = sdsl::int_vector<>,
    typename TSample = sdsl::int_vector<>,
    typename TBvSampleIdx = sdsl::sd_vector<>>
class SrIndex : public RIndex<TStorage, TAlphabet, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample> {
 public:
  using Base = RIndex<TStorage, TAlphabet, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample>;

  SrIndex(const TStorage &t_storage, std::size_t t_sr)
      : Base(t_storage), subsample_rate_{t_sr}, key_prefix_{std::to_string(subsample_rate_) + "_"} {}

  explicit SrIndex(std::size_t t_sr)
      : Base(), subsample_rate_{t_sr}, key_prefix_{std::to_string(subsample_rate_) + "_"} {}

  SrIndex() = default;

  std::size_t SubsampleRate() const { return subsample_rate_; }

  void load(Config t_config) override {
    TSource source(std::ref(t_config));
    this->loadInner(source);
  }

  void load(std::istream &in) override {
    sdsl::read_member(subsample_rate_, in);
    TSource source(std::ref(in));
    this->loadInner(source);
  }

  [[nodiscard]] std::vector<std::pair<std::string, size_t>> breakdown() const override{

      std::vector<std::pair<std::string, size_t>> parts = Base::breakdown();

      auto child = sdsl::structure_tree::add_child(nullptr, "", sdsl::util::class_name(*this));
      nullstream nulls;
      size_t written_bytes;
      written_bytes = this->template serializeItem<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), nulls, child, "samples_idx");
      parts.emplace_back("samples_idx", written_bytes);
      written_bytes = this->template serializeRank<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), nulls, child, "samples_idx_rank");
      parts.emplace_back("samples_idx_rank", written_bytes);
      return parts;
  }

  using typename Base::size_type;
  using typename Base::ItemKey;
  size_type serialize(std::ostream &out, sdsl::structure_tree_node *v, const std::string &name) const override {
    auto child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));

    size_type written_bytes = 0;
    written_bytes += sdsl::write_member(subsample_rate_, out, child, "subsample_rate");

    written_bytes += Base::serialize(out, v, name);
    written_bytes += this->template serializeItem<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), out, child, "samples_idx");
    written_bytes += this->template serializeRank<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), out, child, "samples_idx_rank");

    return written_bytes;
  }

 protected:

  using Base::key;
  void setupKeyNames() override {
    Base::setupKeyNames();
//    key(SrIndexKey::ALPHABET) = conf::KEY_ALPHABET;
//    key(SrIndexKey::NAVIGATE) = sdsl::conf::KEY_BWT_RLE;
    key(ItemKey::SAMPLES) = key_prefix_ + conf::KEY_BWT_RUN_LAST_TEXT_POS;
    key(ItemKey::MARKS) = key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS_BY_LAST;
    key(ItemKey::MARK_TO_SAMPLE) = key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_TO_LAST_IDX;
    key(ItemKey::SAMPLES_IDX) = key_prefix_ + conf::KEY_BWT_RUN_LAST_IDX;
  }

  using typename Base::TSource;
  using typename Base::Range;

  void loadAllItems(TSource &t_source) override {
    Base::loadAllItems(t_source);

    this->template loadItem<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), t_source, true);
    this->template loadBVRank<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), t_source, true);
  }

  void constructIndex(TSource &t_source) override {
    constructIndex(t_source,
                   [this](auto &t_source) { return this->constructPhiForRange(t_source); });
  }

  template<typename TConstructPhiForRange>
  void constructIndex(TSource &t_source, const TConstructPhiForRange &t_construct_phi_for_range) {
    this->index_.reset(new RIndexBase{
        this->constructLF(t_source),
        this->constructComputeDataBackwardSearchStep(t_source, constructCreateDataBackwardSearchStep()),
        this->constructComputeSAValues(t_construct_phi_for_range(t_source), constructComputeToehold(t_source)),
        this->n_,
        [](const auto &tt_step) { return DataBackwardSearchStep{0, RunDataExt{0, 0, false, 0}}; },
        this->constructGetSymbol(t_source),
        [](auto tt_seq_size) { return Range{0, tt_seq_size}; },
        this->constructIsRangeEmpty()
    });
  }

  using typename Base::RunData;
  using typename Base::Char;
  struct RunDataExt : public RunData {
    bool is_run_end; // If current position is the end of a run
    std::size_t next_pos; // Position for next LF step

    explicit RunDataExt(Char t_c, std::size_t t_end_run_rnk, bool t_is_run_end, std::size_t tt_next_pos)
        : RunData{t_c, t_end_run_rnk}, is_run_end{t_is_run_end}, next_pos{tt_next_pos} {}
    virtual ~RunDataExt() = default;
  };

  using DataBackwardSearchStep = sri::DataBackwardSearchStep<RunDataExt>;

  auto constructCreateDataBackwardSearchStep() {
    return [](const auto &tt_range, const auto &tt_c, const auto &tt_next_range, const auto &tt_step) {
      const auto &[next_start, next_end] = tt_next_range;
      return DataBackwardSearchStep{tt_step, RunDataExt{tt_c, next_end.run.rank, true, next_end.value - 1}};
    };
  }

  auto constructGetSample(TSource &t_source) {
    auto cref_samples = this->template loadItem<TSample>(key(ItemKey::SAMPLES), t_source);
    auto cref_bv_sample_idx = this->template loadItem<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), t_source, true);
    auto bv_sample_idx_rank = this->template loadBVRank<TBvSampleIdx>(key(ItemKey::SAMPLES_IDX), t_source, true);

    return [cref_samples, cref_bv_sample_idx, bv_sample_idx_rank](auto tt_run_rnk) {
      return cref_bv_sample_idx.get()[tt_run_rnk]
             ? std::optional<std::size_t>(cref_samples.get()[bv_sample_idx_rank(tt_run_rnk)])
             : std::nullopt;
    };
  }

  auto constructComputeToehold(TSource &t_source) {
    auto cref_bwt_rle = this->template loadItem<TBwtRLE>(key(ItemKey::NAVIGATE), t_source);

    auto get_sample = constructGetSample(t_source);
    auto get_sample_run_data = [get_sample](const RunDataExt &tt_run_data) {
      return tt_run_data.is_run_end ? get_sample(tt_run_data.last_run_rnk) : std::nullopt;
    };

    auto cref_alphabet = this->template loadItem<TAlphabet>(key(ItemKey::ALPHABET), t_source);
    auto lf_run_data = [cref_bwt_rle, cref_alphabet](RunDataExt &tt_run_data) {
      auto report = [&tt_run_data, cref_alphabet](
          auto tt_rnk, auto tt_c, auto tt_run_rnk, auto tt_run_start, auto tt_run_end, auto tt_symbol_run_rnk) {
        tt_run_data.c = tt_c;
        tt_run_data.last_run_rnk = tt_run_rnk;
        tt_run_data.is_run_end = tt_run_data.next_pos == tt_run_end - 1;
        tt_run_data.next_pos = cref_alphabet.get().C[tt_c] + tt_rnk;
      };

      cref_bwt_rle.get().rank(tt_run_data.next_pos, report);

      return tt_run_data;
    };

    auto compute_sa_value = buildComputeSAValueBackward(get_sample_run_data, lf_run_data, this->n_);
    auto compute_sa_value_for_run_data = [cref_bwt_rle, compute_sa_value](RunDataExt tt_run_data) {
      tt_run_data.last_run_rnk = cref_bwt_rle.get().selectOnRuns(tt_run_data.last_run_rnk, tt_run_data.c);
      return compute_sa_value(tt_run_data);
    };

    return ComputeToehold(compute_sa_value_for_run_data, cref_bwt_rle.get().size());
  }

  struct Run {
    std::size_t start;
    std::size_t end;
    Char c;
    std::size_t rnk;
    bool is_run_end;

    bool operator<(const Run &rhs) const {
      return start < rhs.start;
    }
  };

  auto constructSplitInBWTRuns(TSource &t_source) {
    auto cref_bwt_rle = this->template loadItem<TBwtRLE>(key(ItemKey::NAVIGATE), t_source);
    return [cref_bwt_rle](auto tt_first, auto tt_last) {
      std::vector<Run> runs;
      auto report = [&runs, tt_last](auto tt_rnk, auto tt_c, auto tt_start, auto tt_end) {
        runs.emplace_back(Run{tt_start, tt_end, tt_c, tt_rnk, tt_end <= tt_last});
      };
      cref_bwt_rle.get().splitInRuns(tt_first, tt_last, report);
      runs.front().start = tt_first;
      runs.back().end = tt_last;
      return runs;
    };
  }

  auto constructLFForPhi(TSource &t_source) {
    auto cref_alphabet = this->template loadItem<TAlphabet>(key(ItemKey::ALPHABET), t_source);
    auto cumulative = RandomAccessForCRefContainer(std::cref(cref_alphabet.get().C));
    this->n_ = cumulative[cref_alphabet.get().sigma];

    auto cref_bwt_rle = this->template loadItem<TBwtRLE>(key(ItemKey::NAVIGATE), t_source);
    auto bwt_rank = [cref_bwt_rle](const auto &tt_c, const auto &tt_pos) {
      return cref_bwt_rle.get().rank(tt_pos, tt_c);
    };

    auto create_range = [](auto tt_c_before_sp, auto tt_c_until_ep, const auto &tt_smaller_c) {
      return Run{tt_c_before_sp + tt_smaller_c, tt_c_until_ep + tt_smaller_c, 0, 0, false};
    };

    Run empty_range{0, 0, 0, 0, false};

    auto lf = LF(bwt_rank, cumulative, create_range, empty_range);
    return [lf](const Run &tt_run) {
      return lf(tt_run.start, tt_run.end, tt_run.c);
    };
  }

  auto constructPhiForRange(TSource &t_source) {
    auto phi = this->constructPhi(t_source, this->constructGetMarkToSampleIdx(t_source, true));
    auto phi_simple = [phi](const auto &tt_value) { return phi(tt_value).first; };
    auto get_sample = constructGetSample(t_source);
    auto get_sample_run =
        [get_sample](const Run &tt_run) { return tt_run.is_run_end ? get_sample(tt_run.rnk) : std::nullopt; };
    auto split = constructSplitInBWTRuns(t_source);
    auto split_range = [split](const Range &tt_range) { return split(tt_range.start, tt_range.end); };
    auto split_run = [split](const Run &tt_run) { return split(tt_run.start, tt_run.end); };
    auto lf = constructLFForPhi(t_source);
    auto is_range_empty = [](const Range &tt_range) { return !(tt_range.start < tt_range.end); };
    auto update_run = [](Run &tt_run) {
      --tt_run.end;
      tt_run.is_run_end = false;
    };
    auto is_run_empty = [](const Run &tt_run) { return !(tt_run.start < tt_run.end); };
    return PhiBackwardForRange(phi_simple,
                               get_sample_run,
                               split_range,
                               split_run,
                               lf,
                               subsample_rate_,
                               this->n_,
                               is_range_empty,
                               update_run,
                               is_run_empty);
  }

  std::size_t subsample_rate_ = 1;
  std::string key_prefix_;
};

template<typename TStorage = GenericStorage,
    typename TAlphabet = Alphabet<>,
    typename TBwtRLE = RLEString<>,
    typename TBvMark = sdsl::sd_vector<>,
    typename TMarkToSampleIdx = sdsl::int_vector<>,
    typename TSample = sdsl::int_vector<>,
    typename TBvSampleIdx = sdsl::sd_vector<>,
    typename TBvValidMark = sdsl::bit_vector>
class SrIndexValidMark : public SrIndex<
    TStorage, TAlphabet, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample, TBvSampleIdx> {
 public:
  using Base = SrIndex<TStorage, TAlphabet, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample, TBvSampleIdx>;

  SrIndexValidMark(const TStorage &t_storage, std::size_t t_sr) : Base(t_storage, t_sr) {}

  explicit SrIndexValidMark(std::size_t t_sr) : Base(t_sr) {}

  SrIndexValidMark() = default;

  [[nodiscard]] std::vector<std::pair<std::string, size_t>> breakdown() const override{

      std::vector<std::pair<std::string, size_t>> parts = Base::breakdown();

      auto child = sdsl::structure_tree::add_child(nullptr, "", sdsl::util::class_name(*this));
      nullstream nulls;
      size_t written_bytes;
      written_bytes = this->template serializeItem<TBvValidMark>(key(ItemKey::VALID_MARKS), nulls, child, "valid_marks");
      parts.emplace_back("valid_marks", written_bytes);
      return parts;
  }

  using typename Base::size_type;
  using typename Base::ItemKey;
  size_type serialize(std::ostream &out, sdsl::structure_tree_node *v, const std::string &name) const override {
    auto child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));

    size_type written_bytes = Base::serialize(out, v, name);

    written_bytes += this->template serializeItem<TBvValidMark>(key(ItemKey::VALID_MARKS), out, child, "valid_marks");

    return written_bytes;
  }

 protected:

  using Base::key;

  void setupKeyNames() override {
    Base::setupKeyNames();
//    key(SrIndexKey::ALPHABET) = conf::KEY_ALPHABET;
//    key(SrIndexKey::NAVIGATE) = sdsl::conf::KEY_PSI;
//    key(SrIndexKey::SAMPLES) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS;
//    key(SrIndexKey::MARKS) = this->key_prefix_ + conf::KEY_BWT_RUN_LAST_TEXT_POS_BY_FIRST;
//    key(SrIndexKey::MARK_TO_SAMPLE) = this->key_prefix_ + conf::KEY_BWT_RUN_LAST_TEXT_POS_SORTED_TO_FIRST_IDX;
//    key(SrIndexKey::SAMPLES_IDX) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_IDX;
    key(ItemKey::VALID_MARKS) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_MARK;
  }

  using typename Base::TSource;

  void loadAllItems(TSource &t_source) override {
    Base::loadAllItems(t_source);

    this->template loadItem<TBvValidMark>(key(ItemKey::VALID_MARKS), t_source, true);
  }

  void constructIndex(TSource &t_source) override {
    Base::constructIndex(t_source,
                         [this](auto &t_source) {
                           auto phi = this->constructPhi(t_source, constructGetMarkToSampleIdx(t_source));
                           return constructPhiForRange(t_source, phi);
                         });
  }

  using Base::constructIndex;

  auto constructGetMarkToSampleIdx(TSource &t_source) {
    auto cref_mark_to_sample_idx = this->template loadItem<TMarkToSampleIdx>(key(ItemKey::MARK_TO_SAMPLE), t_source);
    auto cref_bv_valid_mark = this->template loadItem<TBvValidMark>(key(ItemKey::VALID_MARKS), t_source, true);
    return RandomAccessForTwoContainers(cref_mark_to_sample_idx, cref_bv_valid_mark);
  }

  using typename Base::Range;
  using typename Base::Run;

  template<typename TPhi>
  auto constructPhiForRange(TSource &t_source, const TPhi &t_phi) {
    auto get_sample = this->constructGetSample(t_source);
    auto get_sample_run =
        [get_sample](const Run &tt_run) { return tt_run.is_run_end ? get_sample(tt_run.rnk) : std::nullopt; };
    auto split = this->constructSplitInBWTRuns(t_source);
    auto split_range = [split](const Range &tt_range) { return split(tt_range.start, tt_range.end); };
    auto split_run = [split](const Run &tt_run) { return split(tt_run.start, tt_run.end); };
    auto lf = this->constructLFForPhi(t_source);
    auto is_range_empty = [](const Range &tt_range) { return !(tt_range.start < tt_range.end); };
    auto update_run = [](Run &tt_run) {
      --tt_run.end;
      tt_run.is_run_end = false;
    };
    auto is_run_empty = [](const Run &tt_run) { return !(tt_run.start < tt_run.end); };
    return PhiBackwardForRangeWithValidity(t_phi,
                                           get_sample_run,
                                           split_range,
                                           split_run,
                                           lf,
                                           this->subsample_rate_,
                                           this->n_,
                                           is_range_empty,
                                           update_run,
                                           is_run_empty);
  }
};

template<typename TStorage = GenericStorage,
    typename TAlphabet = Alphabet<>,
    typename TBwtRLE = RLEString<>,
    typename TBvMark = sdsl::sd_vector<>,
    typename TMarkToSampleIdx = sdsl::int_vector<>,
    typename TSample = sdsl::int_vector<>,
    typename TBvSampleIdx = sdsl::sd_vector<>,
    typename TBvValidMark = sdsl::bit_vector,
    typename TValidArea = sdsl::int_vector<>>
class SrIndexValidArea : public SrIndexValidMark<
    TStorage, TAlphabet, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample, TBvSampleIdx, TBvValidMark> {
 public:
  using Base = SrIndexValidMark<
      TStorage, TAlphabet, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample, TBvSampleIdx, TBvValidMark>;

  SrIndexValidArea(const TStorage &t_storage, std::size_t t_sr) : Base(t_storage, t_sr) {}

  explicit SrIndexValidArea(std::size_t t_sr) : Base(t_sr) {}

  SrIndexValidArea() = default;

  [[nodiscard]] std::vector<std::pair<std::string, size_t>> breakdown() const override{
      std::vector<std::pair<std::string, size_t>> parts = Base::breakdown();
      auto child = sdsl::structure_tree::add_child(nullptr, "", sdsl::util::class_name(*this));
      nullstream nulls;
      size_t written_bytes;
      written_bytes = this->template serializeRank<TBvValidMark, typename TBvValidMark::rank_0_type>( key(ItemKey::VALID_MARKS), nulls, child, "valid_marks_rank");
      parts.emplace_back("valid_marks_rank", written_bytes);
      written_bytes = this->template serializeItem<TValidArea>(key(ItemKey::VALID_AREAS), nulls, child, "valid_areas");
      parts.emplace_back("valid_areas", written_bytes);
      return parts;
  }

  using typename Base::size_type;
  using typename Base::ItemKey;
  size_type serialize(std::ostream &out, sdsl::structure_tree_node *v, const std::string &name) const override {
    auto child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));
    size_type written_bytes = Base::serialize(out, v, name);
    written_bytes += this->template serializeRank<TBvValidMark, typename TBvValidMark::rank_0_type>( key(ItemKey::VALID_MARKS), out, child, "valid_marks_rank");
    written_bytes += this->template serializeItem<TValidArea>(key(ItemKey::VALID_AREAS), out, child, "valid_areas");
    return written_bytes;
  }

 protected:

  using Base::key;
  void setupKeyNames() override {
    Base::setupKeyNames();
//    key(SrIndexKey::ALPHABET) = conf::KEY_ALPHABET;
//    key(SrIndexKey::NAVIGATE) = sdsl::conf::KEY_PSI;
//    key(SrIndexKey::SAMPLES) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS;
//    key(SrIndexKey::MARKS) = this->key_prefix_ + conf::KEY_BWT_RUN_LAST_TEXT_POS_BY_FIRST;
//    key(SrIndexKey::MARK_TO_SAMPLE) = this->key_prefix_ + conf::KEY_BWT_RUN_LAST_TEXT_POS_SORTED_TO_FIRST_IDX;
//    key(SrIndexKey::SAMPLES_IDX) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_IDX;
//    key(SrIndexKey::VALID_MARKS) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_MARK;
    key(ItemKey::VALID_AREAS) = this->key_prefix_ + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_AREA;
  }

  using typename Base::TSource;

  void loadAllItems(TSource &t_source) override {
    Base::loadAllItems(t_source);

    this->template loadBVRank<TBvValidMark, typename TBvValidMark::rank_0_type>(
        key(ItemKey::VALID_MARKS), t_source, true);
    this->template loadItem<TValidArea>(key(ItemKey::VALID_AREAS), t_source);
  }

  void constructIndex(TSource &t_source) override {
    Base::constructIndex(t_source,
                         [this](auto &t_source) {
                           auto phi = this->constructPhi(t_source,
                                                         constructGetMarkToSampleIdx(t_source),
                                                         constructValidateSample(t_source));
                           return this->constructPhiForRange(t_source, phi);
                         });
  }

  auto constructGetMarkToSampleIdx(TSource &t_source) {
    auto cref_mark_to_sample_idx = this->template loadItem<TMarkToSampleIdx>(key(ItemKey::MARK_TO_SAMPLE), t_source);
    auto cref_bv_valid_mark = this->template loadItem<TBvValidMark>(key(ItemKey::VALID_MARKS), t_source, true);
    return RandomAccessForTwoContainers(cref_mark_to_sample_idx, cref_bv_valid_mark);
  }

  auto constructValidateSample(TSource &t_source) {
    auto bv_valid_mark_rank = this->template loadBVRank<TBvValidMark, typename TBvValidMark::rank_0_type>(
        key(ItemKey::VALID_MARKS), t_source, true);
    auto cref_valid_area = this->template loadItem<TValidArea>(key(ItemKey::VALID_AREAS), t_source);
    auto get_valid_area = RandomAccessForCRefContainer(cref_valid_area);

    return SampleValidator(bv_valid_mark_rank, get_valid_area);
  }
};

template<uint8_t t_width, typename TBvMark, typename TBvSampleIdx>
void constructSRI(const std::string &t_data_path, std::size_t t_subsample_rate, sri::Config &t_config);

template<typename TStorage, template<uint8_t> typename TAlphabet, uint8_t t_width, typename TBwtRLE, typename TBvMark, typename TMarkToSampleIdx, typename TSample, typename TBvSampleIdx>
void construct(SrIndex<
    TStorage, TAlphabet<t_width>, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample, TBvSampleIdx> &t_index,
               const std::string &t_data_path,
               sri::Config &t_config) {
  constructSRI<t_width, TBvMark, TBvSampleIdx>(t_data_path, t_index.SubsampleRate(), t_config);

  t_index.load(t_config);
}

template<uint8_t t_width, typename TBvMark, typename TBvSampleIdx, typename TBvValidMark>
void constructSRIValidMark(const std::string &t_data_path, std::size_t t_subsample_rate, sri::Config &t_config);

template<typename TStorage, template<uint8_t> typename TAlphabet, uint8_t t_width, typename TBwtRLE, typename TBvMark, typename TMarkToSampleIdx, typename TSample, typename TBvSampleIdx, typename TBvValidMark>
void construct(SrIndexValidMark<
    TStorage, TAlphabet<t_width>, TBwtRLE, TBvMark, TMarkToSampleIdx, TSample, TBvSampleIdx, TBvValidMark> &t_index,
               const std::string &t_data_path,
               sri::Config &t_config) {
  constructSRIValidMark<t_width, TBvMark, TBvSampleIdx, TBvValidMark>(t_data_path, t_index.SubsampleRate(), t_config);

  t_index.load(t_config);
}

template<typename TStorage, template<uint8_t> typename TAlphabet, uint8_t t_width, typename TBwtRLE, typename TBvMark, typename TMarkToSampleIdx, typename TSample, typename TBvSampleIdx, typename TBvValidMark, typename TValidArea>
void construct(SrIndexValidArea<TStorage,
                                TAlphabet<t_width>,
                                TBwtRLE,
                                TBvMark,
                                TMarkToSampleIdx,
                                TSample,
                                TBvSampleIdx,
                                TBvValidMark,
                                TValidArea> &t_index,
               const std::string &t_data_path,
               sri::Config &t_config) {
  constructSRIValidMark<t_width, TBvMark, TBvSampleIdx, TBvValidMark>(t_data_path, t_index.SubsampleRate(), t_config);

  t_index.load(t_config);
}

void constructSubsamplingForwardSamplesForPhiBackward(std::size_t t_subsample_rate, sdsl::cache_config &t_config);

void constructSubsamplingForwardMarksForPhiBackward(std::size_t t_subsample_rate, sdsl::cache_config &t_config);

template<uint8_t t_width, typename TBvMark, typename TBvSampleIdx>
void constructSRI(const std::string &t_data_path, std::size_t t_subsample_rate, sri::Config &t_config) {
  constructRIndex<t_width, TBvMark>(t_data_path, t_config);

  auto prefix = std::to_string(t_subsample_rate) + "_";

  {
    // Sort samples (BWT-run last letter) by its text positions
    auto event = sdsl::memory_monitor::event("Subsampling");
    const auto key = conf::KEY_BWT_RUN_LAST_TEXT_POS_SORTED_IDX;
    if (!sdsl::cache_file_exists(key, t_config)) {
      constructSortedIndices(conf::KEY_BWT_RUN_LAST_TEXT_POS, t_config, key);
    }
  }

  {
    // Construct subsampling forward of samples (text positions of BWT-run last letter)
    auto event = sdsl::memory_monitor::event("Subsampling");
    auto key = prefix + conf::KEY_BWT_RUN_LAST_IDX;
    if (!sdsl::cache_file_exists(key, t_config)) {
      constructSubsamplingForwardSamplesForPhiBackward(t_subsample_rate, t_config);
    }

    if (!sdsl::cache_file_exists<TBvSampleIdx>(key, t_config)) {
      std::size_t r;
      {
        sdsl::int_vector_buffer<> bwt(sdsl::cache_file_name(conf::KEY_BWT_RUN_LAST, t_config));
        r = bwt.size();
      }

      constructBitVectorFromIntVector<TBvSampleIdx>(key, t_config, r, false);
    }
  }

  {
    // Construct subsampling forward of marks (text positions of BWT-run first letter)
    auto event = sdsl::memory_monitor::event("Subsampling");
    if (!sdsl::cache_file_exists(prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_TO_LAST_IDX, t_config)) {
      constructSubsamplingForwardMarksForPhiBackward(t_subsample_rate, t_config);
    }
  }

  {
    // Construct predecessor on the text positions of sub-sampled BWT-run first letter
    auto event = sdsl::memory_monitor::event("Predecessor");
    const auto key = prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_BY_LAST;
    if (!sdsl::cache_file_exists<TBvMark>(key, t_config)) {
      std::size_t n;
      {
        sdsl::int_vector_buffer<t_width> bwt(sdsl::cache_file_name(sdsl::key_bwt_trait<t_width>::KEY_BWT, t_config));
        n = bwt.size();
      }
      constructBitVectorFromIntVector<TBvMark>(key, t_config, n, false);
    }
  }

}

void constructSubsamplingForwardMarksValidity(std::size_t t_subsample_rate, sdsl::cache_config &t_config);

template<uint8_t t_width, typename TBvMark, typename TBvSampleIdx, typename TBvValidMark>
void constructSRIValidMark(const std::string &t_data_path, std::size_t t_subsample_rate, sri::Config &t_config) {
  constructSRI<t_width, TBvMark, TBvSampleIdx>(t_data_path, t_subsample_rate, t_config);

  auto prefix_key = std::to_string(t_subsample_rate) + "_";

  {
    // Construct subsampling validity marks and areas
    auto event = sdsl::memory_monitor::event("Subsampling Validity");
    auto key = prefix_key + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_MARK;
    if (!sdsl::cache_file_exists(key, t_config)) {
      constructSubsamplingForwardMarksValidity(t_subsample_rate, t_config);
    }

    if (!sdsl::cache_file_exists<TBvValidMark>(key, t_config)) {
      std::size_t r_prime;
      {
        sdsl::int_vector_buffer<> buf(sdsl::cache_file_name(prefix_key + conf::KEY_BWT_RUN_LAST_TEXT_POS, t_config));
        r_prime = buf.size();
      }
      constructBitVectorFromIntVector<TBvValidMark,
                                      typename TBvValidMark::rank_0_type,
                                      typename TBvValidMark::select_0_type>(key, t_config, r_prime, true);
    }
  }
}

void constructSubsamplingForwardSamplesForPhiBackward(std::size_t t_subsample_rate, sdsl::cache_config &t_config) {
  auto prefix = std::to_string(t_subsample_rate) + "_";

  // Samples
  sdsl::int_vector<> samples; // BWT-run end positions in text
  sdsl::load_from_cache(samples, conf::KEY_BWT_RUN_LAST_TEXT_POS, t_config);
  auto r = samples.size();
  auto log_r = sdsl::bits::hi(r) + 1;

  sdsl::int_vector<> sorted_samples_idx;
  sdsl::load_from_cache(sorted_samples_idx, conf::KEY_BWT_RUN_LAST_TEXT_POS_SORTED_IDX, t_config);

  std::array<std::size_t, 2> req_samples_idx{};
  {
    // We must sub-sample the samples associated to the first and last marks in the text
    sdsl::int_vector_buffer<> mark_to_sample(
        sdsl::cache_file_name(conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_TO_LAST_IDX, t_config));
    req_samples_idx[0] = mark_to_sample[0];
    req_samples_idx[1] = mark_to_sample[mark_to_sample.size() - 1];
  }

  // Compute sub-sampled indices for sampled values
  sdsl::int_vector<> subsamples_idx;
  {
    auto subsamples_idx_vec = computeSampling(t_subsample_rate,
                                              sorted_samples_idx.begin(),
                                              sorted_samples_idx.end(),
                                              samples,
                                              req_samples_idx);

    std::sort(subsamples_idx_vec.begin(), subsamples_idx_vec.end());

    subsamples_idx = sdsl::int_vector<>(subsamples_idx_vec.size(), 0, log_r);
    std::copy(subsamples_idx_vec.begin(), subsamples_idx_vec.end(), subsamples_idx.begin());

    // Store sub-sample indices sorted by BWT positions
    sdsl::store_to_cache(subsamples_idx, prefix + conf::KEY_BWT_RUN_LAST_IDX, t_config);
  }

  {
    // Compute sub-samples
    sdsl::int_vector<> subsamples(subsamples_idx.size(), 0, samples.width());
    std::transform(subsamples_idx.begin(), subsamples_idx.end(), subsamples.begin(),
                   [&samples](auto tt_i) { return samples[tt_i]; });

    sdsl::store_to_cache(subsamples, prefix + conf::KEY_BWT_RUN_LAST_TEXT_POS, t_config);
  }
}

auto computeSampleToMarkLinksForPhiBackward(const std::string &t_prefix, sdsl::cache_config &t_config) {
  // Sub-sampled indices of samples
  sdsl::int_vector<> subsamples_idx;
  sdsl::load_from_cache(subsamples_idx, t_prefix + conf::KEY_BWT_RUN_LAST_IDX, t_config);

  sdsl::int_vector<> subsample_to_mark_links(subsamples_idx.size(), 0, subsamples_idx.width());

  std::size_t r;
  {
    sdsl::int_vector_buffer<> buf(sdsl::cache_file_name(conf::KEY_BWT_RUN_FIRST, t_config));
    r = buf.size();
  }

  // Compute links from samples to marks
  for (size_t i = 0; i < subsamples_idx.size(); ++i) {
    subsample_to_mark_links[i] = (subsamples_idx[i] + 1) % r;
  }

  return subsample_to_mark_links;
}

void constructSubsamplingForwardMarksForPhiBackward(std::size_t t_subsample_rate, sdsl::cache_config &t_config) {
  auto prefix = std::to_string(t_subsample_rate) + "_";

  // Text positions of marks indices associated to sub-samples, i.e., text positions of sub-sampled marks
  sdsl::int_vector<> subsampled_mark_text_pos;
  std::size_t r_prime;

  {
    // Compute sub-sample to mark links

    // Links from sub-samples to their corresponding mark (actually, to rank of mark in BWT)
    auto subsample_to_mark_links = computeSampleToMarkLinksForPhiBackward(prefix, t_config);
    r_prime = subsample_to_mark_links.size();

    sdsl::int_vector<> marks;
    sdsl::load_from_cache(marks, conf::KEY_BWT_RUN_FIRST_TEXT_POS, t_config);

    subsampled_mark_text_pos = sdsl::int_vector(r_prime, 0, marks.width());
    std::transform(subsample_to_mark_links.begin(),
                   subsample_to_mark_links.end(),
                   subsampled_mark_text_pos.begin(),
                   [&marks](auto tt_i) { return marks[tt_i]; });
  }

  auto log_r_prime = sdsl::bits::hi(r_prime) + 1;

  // Links from sub-sampled marks (sorted by text position) to sub-samples indices. Note that, initially, these are the indices of sub-sample in BWT.
  sdsl::int_vector<> subsampled_mark_to_subsample_links(r_prime, 0, log_r_prime);
  std::iota(subsampled_mark_to_subsample_links.begin(), subsampled_mark_to_subsample_links.end(), 0);

  // Sort indexes by text positions of its marks, becoming in the links from the sub-sampled marks to sub-sampled samples.
  std::sort(subsampled_mark_to_subsample_links.begin(),
            subsampled_mark_to_subsample_links.end(),
            [&subsampled_mark_text_pos](const auto &tt_a, const auto &tt_b) {
              return subsampled_mark_text_pos[tt_a] < subsampled_mark_text_pos[tt_b];
            });

  sdsl::store_to_cache(subsampled_mark_text_pos, prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_BY_LAST, t_config);

  sdsl::store_to_cache(subsampled_mark_to_subsample_links,
                       prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_TO_LAST_IDX,
                       t_config);
}

void constructSubsamplingForwardMarksValidity(std::size_t t_subsample_rate, sdsl::cache_config &t_config) {
  auto prefix = std::to_string(t_subsample_rate) + "_";

  sdsl::int_vector<> marks;
  sdsl::load_from_cache(marks, conf::KEY_BWT_RUN_FIRST_TEXT_POS, t_config);
  sdsl::int_vector<> sorted_marks_idx;
  sdsl::load_from_cache(sorted_marks_idx, conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_IDX, t_config);
  auto it_marks_idx = sorted_marks_idx.begin();
  auto get_next_mark = [&marks, &it_marks_idx]() { return marks[*(it_marks_idx++)]; };

  sdsl::int_vector<> submarks;
  sdsl::load_from_cache(submarks, prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_BY_LAST, t_config);
  std::sort(submarks.begin(), submarks.end());
  auto it_submarks = submarks.begin();
  auto get_next_submark = [&it_submarks]() { return *(it_submarks++); };

  auto r_prime = submarks.size();
  std::vector<std::pair<std::size_t, std::size_t>> validity;
  validity.reserve(r_prime / 4);
  std::size_t max_valid_area = 0;
  auto report = [r_prime, &validity, &max_valid_area](auto tt_i, auto tt_submark, auto tt_next_mark) {
    auto valid_area = tt_next_mark - tt_submark;
    validity.emplace_back(tt_i, valid_area);

    if (max_valid_area < valid_area) max_valid_area = valid_area;
  };

  computeSubmarksValidity(r_prime, get_next_mark, get_next_submark, report);

  const std::size_t buffer_size = 1 << 20;
  sdsl::int_vector_buffer<> valid_submarks(
      sdsl::cache_file_name(prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_MARK, t_config),
      std::ios::out,
      buffer_size,
      sdsl::bits::hi(r_prime) + 1);

  sdsl::int_vector_buffer<> valid_areas(
      sdsl::cache_file_name(prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_AREA, t_config),
      std::ios::out,
      buffer_size,
      sdsl::bits::hi(max_valid_area) + 1);

  for (auto it = validity.begin(); it != validity.end(); ++it) {
    auto [idx, area] = *it;
    valid_submarks.push_back(it->first);
    valid_areas.push_back(it->second);
  }

  valid_submarks.close();
  sdsl::register_cache_file(prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_MARK, t_config);
  valid_areas.close();
  sdsl::register_cache_file(prefix + conf::KEY_BWT_RUN_FIRST_TEXT_POS_SORTED_VALID_AREA, t_config);
}

}

#endif //SRI_SR_INDEX_H_
