//
// Created by Dustin Cobas <dustin.cobas@gmail.com> on 8/5/21.
//

#ifndef SRI_PSI_H_
#define SRI_PSI_H_

#include <cstddef>
#include <algorithm>

#include <sdsl/bits.hpp>
#include <sdsl/vectors.hpp>
#include <sdsl/bit_vectors.hpp>
#include <sdsl/io.hpp>
#include <sdsl/coder.hpp>

#include "enc_vector.hpp"
#include "coder.h"
#include "io.h"

namespace sri {

//! Constructs the Psi function using BWT for text.
template<typename TGetBwtSymbol, typename TCumulativeC>
auto constructPsi(const TGetBwtSymbol &t_get_bwt_symbol, const TCumulativeC &t_cumulative_c) {
  const auto sigma = t_cumulative_c.size() - 1;
  const auto n = t_cumulative_c[sigma];
  auto sa_position = t_cumulative_c;

  // Calculate psi
  sdsl::int_vector<> psi(n, 0, sdsl::bits::hi(n) + 1);
  for (std::size_t i = 0; i < n; ++i) {
    psi[sa_position[t_get_bwt_symbol(i)]++] = i;
  }

  return psi;
}

//! Constructs and stores the Psi function using BWT for text.
template<typename TGetBwtSymbol, typename TCumulativeC>
void constructPsi(TGetBwtSymbol &t_get_bwt_symbol, const TCumulativeC &t_cumulative_c, sdsl::cache_config &t_config) {
  // Store psi
  sdsl::store_to_cache(constructPsi(t_get_bwt_symbol, t_cumulative_c), sdsl::conf::KEY_PSI, t_config);
}

//! Psi function core based on partial psi per symbol using run-length encoded representation.
//! \tparam TEncVector Encoded vector to store each partial psi function
//! \tparam TIntVector Integer vector to store rank per sampled value in each RLE partial psi
//! \tparam TChar Character or symbol in the compact alphabet
template<typename TEncVector = enc_vector<sdsl::coder::elias_delta, 64>,
    typename TIntVector = sdsl::int_vector<>,
    typename TChar = uint8_t>
class PsiCoreRLE {
 public:
  PsiCoreRLE() = default;

  //! Constructor
  /**
   * @tparam TCumulativeC Random access container
   * @tparam TPsi Random access container
   * @param t_cumulative_c Cumulative count for the alphabet [0..sigma]
   * @param t_psi Full psi function as a container
   */
  template<typename TCumulativeC, typename TPsi>
  PsiCoreRLE(const TCumulativeC &t_cumulative_c, const TPsi &t_psi) {
    auto sigma = t_cumulative_c.size() - 1;
    n_ = t_cumulative_c[sigma];

    partial_psi_.reserve(sigma);

    for (std::size_t i = 0; i < sigma; ++i) {
      const auto sa_sp = t_cumulative_c[i],
          sa_ep = t_cumulative_c[i + 1];
      auto n_c = sa_ep - sa_sp; // Number of symbol i

      // Compute first symbol in BWT (BWT[0])
      if (t_psi[sa_sp] == 0) { first_bwt_symbol_ = i; }

      std::vector<std::size_t> psi_c; // Partial psi for symbol i
      psi_c.reserve(2 * n_c);

      std::vector<std::size_t> psi_c_sample_rank; // Rank per sampled value in run-length encoded partial psi
      psi_c_sample_rank.reserve(n_c / sample_dens_ + 1);

      // Psi values in range of SA corresponding to symbol i. Store only start and end for each run of symbol i
      for (auto j = sa_sp, value = t_psi[j]; j < sa_ep;) {
        psi_c.emplace_back(value); // Save value of the run start
        auto run_end_value = value + 1;

        // Find the run end
        for (; ++j < sa_ep && run_end_value == (value = t_psi[j]); ++run_end_value) {}

        psi_c.emplace_back(run_end_value); // Save value of the run end (RLE will store only run length)

        if (psi_c.size() % sample_dens_ == 0 && j < sa_ep) {
          // If next run start value must be sample, its rank is saved
          psi_c_sample_rank.emplace_back(j - sa_sp);
        }
      }

      auto psi_c_sample_rank_iv = sdsl::int_vector<>(
          psi_c_sample_rank.size(), 0, psi_c_sample_rank.empty() ? 0 : sdsl::bits::hi(psi_c_sample_rank.back()) + 1);
      std::copy(psi_c_sample_rank.begin(), psi_c_sample_rank.end(), psi_c_sample_rank_iv.begin());

      partial_psi_.emplace_back(std::move(psi_c), std::move(psi_c_sample_rank_iv));
    }
  }

  [[nodiscard]] inline std::size_t size() const { return n_; }

  //! Select operation over partial psi function for symbol c
  //! \param t_c Symbol c
  //! \param t_rnk Rank (or number of symbols c) query. It must be less or equal than the number of symbol c
  //! \return Psi value for t_rnk-th symbol c
  auto select(TChar t_c, std::size_t t_rnk) const {
    const auto &[values, ranks] = partial_psi_[t_c];

    auto [run, _] = selectCore(t_rnk, values, ranks);

    return run.end - 1 - (run.rank_end - (t_rnk));
  }

  //! Select operation over partial psi function for symbol c
  //! \param t_c Symbol c
  //! \param t_rnk Rank (or number of symbols c) query. It must be less or equal than the number of symbol c
  //! \param t_report Report psi value for t_rnk-th symbol c and data of run containing that value
  template<typename TReport>
  void select(TChar t_c, std::size_t t_rnk, TReport t_report) const {
    const auto &[values, ranks] = partial_psi_[t_c];

    auto [run, run_ops] = selectCore(t_rnk, values, ranks);

    auto run_rank = run_ops.runSoftRank();
    t_report(run.end - 1 - (run.rank_end - (t_rnk)), run.start, run.end, run_rank);
  }

  //! Rank operation over partial psi function for symbol c
  //! \param t_c Symbol c
  //! \param t_value Psi value (or SA position) query
  //! \return Rank for symbol c before the position given, i.e., number of symbols c with psi value less than t_value
  auto rank(TChar t_c, std::size_t t_value) const {
    assert(("Value must be less or equal than the size of sequence", t_value <= n_));
    const auto &[values, ranks] = partial_psi_[t_c];

    auto upper_bound = computeUpperBound(values, t_value);
    if (upper_bound == 0) return 0ul;

    auto idx = upper_bound - 1; // Index of previous sample to the greater one
    auto rank = idx ? ranks[idx - 1] : 0; // Rank
    RunOps run_ops(values, idx, n_);

    // Sequential search of psi value equal to given t_value and its rank
    auto next_run_start = run_ops.nextStart(0);
    decltype(next_run_start) run_start, run_end;
    do {
      run_start = next_run_start;
      run_end = run_ops.nextEnd(run_start);
      rank += run_end - run_start;
    } while (run_end < t_value && (next_run_start = run_ops.nextStart(run_end)) < t_value);

    if (t_value < run_end) rank -= run_end - t_value;  // rank - 1 - (value - (t_value + 1))
    return rank;
  }

  //! Rank operation over partial psi function for symbol c
  //! \tparam TReport
  //! \param t_c Symbol c
  //! \param t_value Psi value (or SA position) query
  //! \param t_report Report rank for symbol c before the position given, i.e., number of symbols c with psi value less than t_value
  //! and data of run containing the value or the next (upper) run if value does not belong to any run
  template<typename TReport>
  void rank(TChar t_c, std::size_t t_value, TReport t_report) const {
    assert(("Value must be less or equal than the size of sequence", t_value <= n_));
    const auto &[values, ranks] = partial_psi_[t_c];

    bool is_last_value = t_value == n_;
    if (is_last_value) --t_value;

    auto upper_bound = computeUpperBound(values, t_value);

    auto idx = upper_bound ? upper_bound - 1 : 0; // Index of previous sample to the greater one
    auto rank = idx ? ranks[idx - 1] : 0; // Rank
    RunOps run_ops(values, idx, n_);

    // Sequential search of psi value equal to given t_value and its rank
    auto run_start = run_ops.nextStart(0);
    auto run_end = run_ops.nextEnd(run_start);
    while (run_end <= t_value) {
      rank += run_end - run_start;
      run_start = run_ops.nextStart(run_end);
      run_end = run_ops.nextEnd(run_start);
    }

    if (is_last_value) ++t_value;
    if (run_start < t_value) rank += t_value - run_start;

    auto run_rank = run_ops.runSoftRank();

    t_report(rank, run_start, run_end, run_rank);
  }

  //! Find if the given psi value corresponds to the given symbol (appears in the range of the symbol)
  //! \param t_c Symbol c
  //! \param t_value Psi value (or SA position) query
  //! \return If exists a symbol t_c with the psi value t_value
  auto exist(TChar t_c, std::size_t t_value) const {
    auto [_, run_start_value] = rankSoftRun(t_c, t_value);

    return run_start_value != n_;
  }

  //! Rank operation over runs (run length encoded) in psi (these runs match with BWT runs)
  //! \param t_value Psi value (or SA position) query
  //! \return Rank for run-starts before the given position, i.e., number of runs with start psi value less than t_value
  auto rankRun(std::size_t t_value) const {
    auto [n_runs, run_start] = rankSoftRun(t_value);
    if (t_value != n_ && run_start == t_value) --n_runs;

    return n_runs;
  }

  //! Compute the number of runs up to the value (including it) and the start value of the run containing the given value
  //! \param t_value Psi value (or SA position) query
  //! \return {Number of runs started up to the queried position; start value of run containing queried position or @p n if it does not belong to any run}
  auto rankSoftRun(std::size_t t_value) const {
    std::size_t n_runs = 0;
    std::size_t run_start = n_;
    for (int i = 0; i < partial_psi_.size(); ++i) {
      auto [n_runs_c, run_start_c] = rankSoftRun(i, t_value);
      if (run_start_c != n_) run_start = run_start_c;

      n_runs += n_runs_c;
    }

    return std::make_pair(n_runs, run_start);
  }

  //! Compute the number of runs up to the value (including it) and the start value of the run containing the given value
  //! \param t_c Symbol
  //! \param t_value Queried position
  //! \return {Number of runs started up to the queried position; start value of run containing queried position or n if it does not belong to any run}
  std::pair<std::size_t, std::size_t> rankSoftRun(TChar t_c, std::size_t t_value) const {
    const auto &[values, ranks] = partial_psi_[t_c];

    auto upper_bound = computeUpperBound(values, t_value);

    if (upper_bound == 0) { return {0, n_}; }

    auto idx = upper_bound - 1; // Index of previous sample to the greater one

    auto run_start = values.sample_and_pointer()[2 * idx]; // Psi value at run start
    auto n_runs = idx * sample_dens_ / 2 + 1; // Number of runs (runs in psi are equal to bwt run)

    if (run_start == t_value) { return {n_runs, run_start}; }

    DecodeUInt decode_uint(values, idx);
    auto &run_length = decode_uint; // Must be called once per run
    auto &run_gap = decode_uint; // Must be called once per run

    // Sequential search of psi run_start equal to given t_value and its rank
    auto run_end = run_start + run_length();
    decltype(run_start) next_run_start;
    for (auto n_runs_to_next_sample = std::min(sample_dens_, values.size() - idx * sample_dens_) / 2 - 1;
         run_end < t_value && n_runs_to_next_sample && (next_run_start = run_end + run_gap()) <= t_value;
         --n_runs_to_next_sample) {
      run_start = next_run_start;
      run_end = run_start + run_length();
      ++n_runs;
    }

    return {n_runs, t_value < run_end ? run_start : n_};
  }

  //! Compute the number of runs for symbols smaller than @p t_c
  //! \param t_c Symbol
  //! \return Number of runs for symbol c, with 0 \<= c \< @p t_c
  auto rankCharRun(TChar t_c) const {
    std::size_t n_runs = 0;
    for (int i = 0; i < t_c; ++i) {
      n_runs += partial_psi_[i].first.size() / 2;
    }

    return n_runs;
  }

  //! Split in runs (BWT runs) on the given range [t_first..t_last)
  //! \param t_first First position in queried range
  //! \param t_last Last position in queried range (not included)
  //! \return Runs (BWT) in the queried range
  auto splitInRuns(std::size_t t_first, std::size_t t_last) const {
    using Run = std::pair<std::size_t, std::size_t>;

    auto construct_run = [](auto tt_first, auto tt_last, auto, auto, auto) {
      return Run(tt_first, tt_last);
    };

    return splitInSortedRuns(t_first, t_last, construct_run);
  }

  //! Split in runs (BWT runs) on the given range [t_first..t_last)
  //! \tparam TCreateRun
  //! \param t_first First position in queried range
  //! \param t_last Last position in queried range (not included)
  //! \param t_create_run Create a run from <first, last, symbol, number of run, if first is the real first of run>
  //! \return Runs (BWT) in the queried range
  template<typename TCreateRun>
  auto splitInSortedRuns(std::size_t t_first, std::size_t t_last, TCreateRun t_create_run) const {
    std::vector<decltype(t_create_run(t_first, t_last, TChar(0u), 0u, true))> runs;

    auto report = [&runs, &t_create_run](auto tt_first, auto tt_last, auto tt_c, auto tt_n_run, auto tt_is_first) {
      runs.emplace_back(t_create_run(tt_first, tt_last, tt_c, tt_n_run, tt_is_first));
    };

    splitInRuns(t_first, t_last, report);
    std::sort(runs.begin(), runs.end());
    return runs;
  }

  //! Split in runs (BWT runs) on the given range [t_first..t_last)
  //! \tparam TReportRun
  //! \param t_first
  //! \param t_last
  //! \param t_report_run Report runs (BWT) in the queried range
  template<typename TReportRun>
  void splitInRuns(std::size_t t_first, std::size_t t_last, TReportRun t_report_run) const {
    for (int i = 0; i < partial_psi_.size(); ++i) {
      splitInRuns(i, t_first, t_last, t_report_run);
    }
  }

  //! Split in runs (BWT runs) of symbol c on the given range [@p t_first..@p t_last)
  //! \tparam TReportRun
  //! \param t_c Symbol
  //! \param t_first First position in queried range
  //! \param t_last Last position in queried range (not included)
  //! \param t_report_run Report runs (BWT) in the queried range for given symbol
  template<typename TReportRun>
  void splitInRuns(TChar t_c, std::size_t t_first, std::size_t t_last, TReportRun t_report_run) const {
    const auto &[values, ranks] = partial_psi_[t_c];

    auto upper_bound = computeUpperBound(values, t_first);

    auto idx = upper_bound ? upper_bound - 1 : 0; // Index of previous sample to the greater one

    RunOps run_ops(values, idx, n_);

    auto run_start = run_ops.nextStart(0);
    auto run_end = run_ops.nextEnd(run_start);
    auto next_run_start = run_ops.nextStart(run_end);

    // Compute first run that ends after t_first (this is the first run cover by given range)
    while (run_end <= t_first) {
      run_start = next_run_start;
      run_end = run_ops.nextEnd(run_start);

      next_run_start = run_ops.nextStart(run_end);
    }

    auto first = std::max(t_first, run_start), last = run_end;

    // Compute first run that starts after t_last (this is the first run not cover by given range)
    while (next_run_start < t_last) {
      last = run_end;
      t_report_run(first, last, t_c, run_ops.runSoftRank() - 1, first == run_start);

      first = run_start = next_run_start;
      run_end = run_ops.nextEnd(run_start);

      next_run_start = run_ops.nextStart(run_end);
    }

    last = std::min(t_last, run_end);
    if (first < last) {
      t_report_run(first, last, t_c, run_ops.runSoftRank() - 1, first == run_start);
    }
  }

  //! Compute forward runs (BWT runs) of symbol @p c on the given ranks range [@p t_first_rank..@p t_last_rank)
  //! \param t_c Symbol
  //! \param t_first_rank First rank in queried range
  //! \param t_last_rank Last rank in queried range
  //! \return Forward runs (BWT) in the queried range for given symbol, i.e., psi ranges from @p t_first_rank -th to @p t_last_rank -th symbol @p c
  auto computeForwardRuns(TChar t_c, std::size_t t_first_rank, std::size_t t_last_rank) const {
    using Run = std::pair<std::size_t, std::size_t>;
    std::vector<Run> runs;
    auto report = [&runs](auto tt_first, auto tt_last, auto, auto, auto) {
      runs.emplace_back(tt_first, tt_last);
    };

    computeForwardRuns(t_c, t_first_rank, t_last_rank, report);

    return runs;
  }

  //! Compute forward runs (BWT runs) of symbol @p c on the given ranks range [@p t_first_rank..@p t_last_rank)
  //! \tparam TReportRun
  //! \param t_c Symbol
  //! \param t_first_rank First rank in queried range
  //! \param t_last_rank Last rank in queried range
  //! \param t_report_run Report forward runs (BWT) in the queried range for given symbol, i.e., psi ranges from @p t_first_rank -th to @p t_last_rank -th symbol @p c
  template<typename TReportRun>
  void computeForwardRuns(TChar t_c, std::size_t t_first_rank, std::size_t t_last_rank, TReportRun t_report_run) const {
    const auto &[values, ranks] = partial_psi_[t_c];

    auto [run, run_ops] = selectCore(t_first_rank, values, ranks);

    auto first = run.end - 1 - (run.rank_end - (t_first_rank)), last = run.end;

    --t_last_rank;
    while (run.rank_end < t_last_rank) {
      last = run.end;
      t_report_run(first, last, t_c, run_ops.runSoftRank(), first == run.start);

      first = run.start = run_ops.nextStart(run.end);
      run.end = run_ops.nextEnd(run.start);
      run.rank_end += run.end - run.start;
    }

    last = run.end - 1 - (run.rank_end - (t_last_rank)) + 1;
    t_report_run(first, last, t_c, run_ops.runSoftRank(), first == run.start);
  }

  template<typename TReportRun>
  void traverse(TChar t_c, TReportRun t_report_run) const {
    const auto &[values, ranks] = partial_psi_[t_c];

    RunOps run_ops(values, 0, n_);

    auto run_start = run_ops.nextStart(0);
    auto run_end = run_ops.nextEnd(run_start);
    while (run_start < n_) {
      t_report_run(run_start, run_end);

      run_start = run_ops.nextStart(run_end);
      run_end = run_ops.nextEnd(run_start);
    }
  }

  inline auto getFirstBWTSymbol() const { return first_bwt_symbol_; }

  inline auto sigma() const { return partial_psi_.size(); }

  auto countRuns(TChar t_c) { return partial_psi_[t_c].first.size() / 2; }

  typedef std::size_t size_type;

  //! Serialize method
  size_type serialize(std::ostream &out, sdsl::structure_tree_node *v = nullptr, const std::string &name = "") const {
    auto child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));

    size_type written_bytes = 0;
    written_bytes += sdsl::write_member(n_, out, child, "m_n");
//    written_bytes += sdsl::write_member(sample_dens_, out, child, "m_sample_density");
    written_bytes += sdsl::write_member(first_bwt_symbol_, out, child, "m_first_bwt_symbol");

    written_bytes += sdsl::serialize(partial_psi_, out, child, "m_partial_psi");

    sdsl::structure_tree::add_size(child, written_bytes);

    return written_bytes;
  }

  //! Load method
  void load(std::istream &in) {
    sdsl::read_member(n_, in);
//    sdsl::read_member(sample_dens_, in);
    sdsl::read_member(first_bwt_symbol_, in);

    sdsl::load(partial_psi_, in);
  }

 private:

  //! Find idx of first sample greater than t_value (binary search)
  //! \param t_values Searchable values
  //! \param t_value Queried value
  //! \return Index of first sample greater than t_value
  auto computeUpperBound(const TEncVector &t_values, std::size_t t_value) const {
    const auto n_samples = (t_values.size() - 1) / sample_dens_ + 1;
    auto fake_rac_samples = sdsl::random_access_container([](auto tt_i) { return tt_i; }, n_samples);

    auto upper_bound = std::upper_bound(fake_rac_samples.begin(), fake_rac_samples.end(), t_value,
                                        [&t_values](const auto &tt_value, const auto &tt_item) {
                                          return tt_value < t_values.sample(tt_item);
                                        });

    return *upper_bound;
  }

  class DecodeUInt {
   public:
    DecodeUInt(const TEncVector &t_values, std::size_t t_i) {
      data_ = t_values.delta().data(); // RLE data
      pointer_ = t_values.sample_and_pointer()[2 * t_i + 1]; // Pointer to next coded value
    }

    auto operator()() {
      return decode<typename TEncVector::coder>(data_, pointer_);
    }

   private:
    const uint64_t *data_ = nullptr;
    typename TEncVector::int_vector_type::value_type pointer_ = 0;
  };

  class RunOps {
   public:
    RunOps(const TEncVector &t_values, std::size_t t_idx, std::size_t t_n)
        : cr_values_{t_values}, idx_{t_idx}, n_{t_n}, decode_uint_{cr_values_, idx_} {
      sample_dens_ = cr_values_.get_sample_dens();
      n_samples_ = (cr_values_.size() - 1) / sample_dens_ + 1;
    }

    auto nextStart(std::size_t t_run_end) {
      if (n_runs_to_next_sample_) {
        // Still remaining runs until the next sampled value, so next run start value is in the current interval
        ++run_soft_rank_;
        --n_runs_to_next_sample_;
        return t_run_end + decode_uint_();
      }

      if (idx_ < n_samples_) {
        // No remaining runs until the next sampled value, so next run start value is the next sample
        run_soft_rank_ = idx_ * sample_dens_ / 2;
        n_runs_to_next_sample_ = std::min(sample_dens_, cr_values_.size() - idx_ * sample_dens_) / 2 - 1;
        return cr_values_.sample_and_pointer()[2 * idx_++]; // Psi value at run start
      }

      // No remaining runs
      run_soft_rank_ = cr_values_.size() / 2;
      return n_;
    }

    auto nextEnd(std::size_t t_run_start) {
      return (t_run_start != n_) ? t_run_start + decode_uint_() : n_;
    }

    //! Rank or index of current run starting at 0
    auto runSoftRank() const { return run_soft_rank_; }

   private:

    const TEncVector &cr_values_;
    std::size_t idx_;

    std::size_t n_;

    DecodeUInt decode_uint_;

    std::size_t sample_dens_;
    std::size_t n_samples_;
    std::size_t n_runs_to_next_sample_ = 0;
    std::size_t run_soft_rank_ = 0; // Number of current run (runs in psi are equal to bwt run)
  };

  struct RunSelect {
    std::size_t start = 0;
    std::size_t end = 0;

    std::size_t rank_end = 0;
  };

  //! Select (core) operation over partial psi function for symbol c
  //! \param t_rnk Rank (or number of symbols c) query. It must be less or equal than the number of symbol c
  //! \param t_values Encoded values
  //! \param t_ranks Ranks
  //! \return Psi run for t_rnk-th symbol c and rank run end (additional info is added)
  auto selectCore(std::size_t t_rnk, const TEncVector &t_values, const TIntVector &t_ranks) const {
    --t_rnk;

    // Find idx_ of first sample with rank_end greater than t_rnk (binary search)
    auto upper_bound = std::upper_bound(t_ranks.begin(), t_ranks.end(), t_rnk);
    auto idx = std::distance(t_ranks.begin(), upper_bound); // Index of previous sample to the one with greater rank

    auto rank_run_end = idx ? t_ranks[idx - 1] : 0; // Rank

    RunOps run_ops(t_values, idx, n_);

    std::size_t run_start, run_end = 0;

    // Sequential search of psi value with rank_end given, i.e., psi value for t_rnk-th symbol c
    do {
      run_start = run_ops.nextStart(run_end);
      run_end = run_ops.nextEnd(run_start);
      rank_run_end += run_end - run_start;

    } while (rank_run_end <= t_rnk);

    return std::make_pair(RunSelect{run_start, run_end, rank_run_end}, run_ops);
  }

  std::size_t n_ = 0; // Size of sequence
  std::size_t sample_dens_ = TEncVector::sample_dens; // Sample density, i.e., 1 sample per sample_density items

  TChar first_bwt_symbol_ = 0; // BWT[0]

  // For each symbol: {run-length encoded partial psi function; rank for each RLE sampled psi value}
  std::vector<std::pair<TEncVector, TIntVector>> partial_psi_;
};

//! Psi function core based on partial psi per symbol using bit-vector representation.
//!
//! \tparam TBitVector Bit-vector to mark psi values for each symbol
//! \tparam TRank Rank operation over TBitVector
//! \tparam TSelect Select operation over TBitVector
//! \tparam TChar Character or symbol in the compact alphabet
template<typename TBitVector = sdsl::bit_vector,
    typename TRank = typename TBitVector::rank_1_type,
    typename TSelect = typename TBitVector::select_1_type,
    typename TChar = uint8_t>
class PsiCoreBV {
 public:
  PsiCoreBV() = default;

  //! Constructor
  /**
   * @tparam TCumulativeC Random access container
   * @tparam TPsi Random access container
   * @param t_cumulative_c Cumulative count for the alphabet [0..sigma]
   * @param t_psi Full psi function as a container
   */
  template<typename TCumulativeC, typename TPsi>
  PsiCoreBV(const TCumulativeC &t_cumulative_c, const TPsi &t_psi) {
    auto sigma = t_cumulative_c.size() - 1;
    n_ = t_cumulative_c[sigma];

    // Reserve space for psis is critical to avoid invalid pointers in rank/select data structures
    partial_psi_.reserve(sigma);
    rank_partial_psi_.reserve(sigma);
    select_partial_psi_.reserve(sigma);

    for (std::size_t i = 1; i <= sigma; ++i) {
      // Partial psi for character i
      sdsl::bit_vector psi_c(n_, 0);

      // Compute first symbol in BWT (BWT[0])
      if (t_psi[t_cumulative_c[i - 1]] == 0) { first_bwt_symbol_ = i - 1; }

      // Marks psi values in range of SA corresponding to character i
      for (auto j = t_cumulative_c[i - 1]; j < t_cumulative_c[i]; ++j) {
        psi_c[t_psi[j]] = 1;
      }

      partial_psi_.emplace_back(psi_c);
      rank_partial_psi_.emplace_back(&partial_psi_.back());
      select_partial_psi_.emplace_back(&partial_psi_.back());
    }
  }

  [[nodiscard]] inline std::size_t size() const { return n_; }

  //! Select operation over partial psi function for symbol c
  //! \param t_c Symbol c
  //! \param t_rnk Rank (or number of symbols c) query. It must be less or equal than the number of symbol c
  //! \return Psi value for t_rnk-th symbol c
  auto select(TChar t_c, std::size_t t_rnk) const { return select_partial_psi_[t_c](t_rnk); }

  //! Rank operation over partial psi function for symbol c
  //! \param t_c Symbol c
  //! \param t_i Psi value (or SA position) query
  //! \return Rank for symbol c before position given, i.e., number of symbols c with psi value less than t_value
  auto rank(TChar t_c, std::size_t t_i) const { return rank_partial_psi_[t_c](t_i); }

  //! Find if the given psi value corresponds to the given symbol (appears in the range of the symbol)
  //! \param t_c Symbol c
  //! \param t_i Psi value (or SA position) query
  //! \return If exists a symbol t_c with the psi value t_value
  auto exist(TChar t_c, std::size_t t_i) const { return partial_psi_[t_c][t_i]; }

  inline auto getFirstBWTSymbol() const { return first_bwt_symbol_; }

  typedef std::size_t size_type;

  //! Serialize method
  size_type serialize(std::ostream &out, sdsl::structure_tree_node *v = nullptr, const std::string &name = "") const {
    auto child = sdsl::structure_tree::add_child(v, name, sdsl::util::class_name(*this));

    size_type written_bytes = 0;
    written_bytes += sdsl::write_member(n_, out, child, "m_n");
    written_bytes += sdsl::write_member(first_bwt_symbol_, out, child, "m_first_bwt_symbol");

    written_bytes += sdsl::serialize(partial_psi_, out, child, "m_partial_psi");
    written_bytes += sdsl::serialize(rank_partial_psi_, out, child, "m_rank_partial_psi");
    written_bytes += sdsl::serialize(select_partial_psi_, out, child, "m_select_partial_psi");

    sdsl::structure_tree::add_size(child, written_bytes);

    return written_bytes;
  }

  //! Load method
  void load(std::istream &in) {
    sdsl::read_member(n_, in);
    sdsl::read_member(first_bwt_symbol_, in);

    sdsl::load(partial_psi_, in);
    sdsl::load(rank_partial_psi_, in);
    sdsl::load(select_partial_psi_, in);

    for (int i = 0; i < partial_psi_.size(); ++i) {
      rank_partial_psi_[i].set_vector(&partial_psi_[i]);
      select_partial_psi_[i].set_vector(&partial_psi_[i]);
    }
  }

 private:
  std::size_t n_ = 0;

  TChar first_bwt_symbol_ = 0; // BWT[0]

  // Partial psi function per character. Each bit-vector marks the psi values in the range on SA for the corresponding character.
  std::vector<TBitVector> partial_psi_;
  std::vector<TRank> rank_partial_psi_; // Ranks for partial psis.
  std::vector<TSelect> select_partial_psi_; // Selects for partial psis.
};

//! Gets character c for index in SA using the cumulative counts.
/**
 *
 * @tparam TCumulativeC Random access container
 * @param t_cumulative_c Cumulative count for the alphabet [0..sigma]
 * @param t_index Index in the SA
 * @return Character corresponding to index
 */
template<typename TCumulativeC>
auto computeCForSAIndex(const TCumulativeC &t_cumulative_c, std::size_t t_index) {
  auto upper_bound = std::upper_bound(t_cumulative_c.begin(), t_cumulative_c.end(), t_index);
  return std::distance(t_cumulative_c.begin(), upper_bound) - 1;
}

//! Psi function.
/**
 * @tparam TSelectPartialPsi Select function for partial psis
 * @tparam TGetC Get character function
 * @tparam TCumulativeC Random access container
 */
template<typename TSelectPartialPsi, typename TGetC, typename TCumulativeC>
class Psi {
 public:
  Psi(const TSelectPartialPsi &t_select_partial_psi, const TGetC &t_get_c, const TCumulativeC &t_cumulative_c)
      : select_partial_psi_{t_select_partial_psi}, get_c_{t_get_c}, cumulative_c_(t_cumulative_c) {
  }

  auto operator()(std::size_t t_index) const {
    auto c = get_c_(t_index);
    return select_partial_psi_(c, t_index - cumulative_c_[c] + 1);
  }

 private:
  TSelectPartialPsi select_partial_psi_; // Select function for partial psis
  TGetC get_c_; // Get character function
  TCumulativeC cumulative_c_; // Cumulative count for alphabet [0..sigma]
};

}

#endif //SRI_PSI_H_
