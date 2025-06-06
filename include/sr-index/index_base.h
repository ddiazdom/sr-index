//
// Created by Dustin Cobas <dustin.cobas@gmail.com> on 8/30/21.
//

#ifndef SRI_INDEX_BASE_H_
#define SRI_INDEX_BASE_H_

#include <map>
#include <string>
#include <any>
#include <functional>
#include <variant>

#include <sdsl/io.hpp>

#include "config.h"

namespace sri {

class LocateIndex {
 public:
  virtual std::vector<std::size_t> Locate(const std::string &_pattern) const = 0;

  virtual std::pair<std::size_t, std::size_t> Count(const std::string &_pattern) const = 0;
};

using GenericStorage = std::map<std::string, std::any>;

template<typename TItem>
const TItem *get(const GenericStorage &t_storage, const std::string &t_key) {
  auto it = t_storage.find(t_key);
  return (it != t_storage.end()) ? std::any_cast<TItem>(&it->second) : nullptr;
}

template<typename TItem>
const TItem *set(GenericStorage &t_storage, const std::string &t_key, TItem &&t_item) {
  auto [it, inserted] = t_storage.emplace(t_key, t_item);
  return std::any_cast<TItem>(&it->second);
}


template<typename TStorage = GenericStorage>
class IndexBaseWithExternalStorage : public LocateIndex {
 public:
  explicit IndexBaseWithExternalStorage(const TStorage &t_storage) : storage_{t_storage} {}

  IndexBaseWithExternalStorage() = default;

  std::vector<std::size_t> Locate(const std::string &t_pattern) const override {
    return index_->Locate(t_pattern);
  }

  std::pair<std::size_t, std::size_t> Count(const std::string &t_pattern) const override {
    return index_->Count(t_pattern);
  }

  auto sizeSequence() const { return n_; }

  virtual void load(Config t_config) = 0;

  typedef std::size_t size_type;
  virtual void load(std::istream &in) = 0;

  virtual size_type serialize(std::ostream &out) const {
    return serialize(out, nullptr, "");
  }

  virtual size_type serialize(std::ostream &out, sdsl::structure_tree_node *v, const std::string &name) const = 0;
  virtual std::vector<std::pair<std::string, size_t>> breakdown() const = 0;

protected:
  enum class ItemKey : unsigned char {
    ALPHABET = 0,
    NAVIGATE,
    MARKS,
    SAMPLES,
    MARK_TO_SAMPLE,
    SAMPLES_IDX,
    RUN_CUMULATIVE_COUNT,
    VALID_MARKS,
    VALID_AREAS,
    NUM_ITEMS
  };

  auto &key(const ItemKey &t_key_enum) {
    return keys_[static_cast<unsigned char>(t_key_enum)];
  }

  const auto &key(const ItemKey &t_key_enum) const {
    return keys_[static_cast<unsigned char>(t_key_enum)];
  }

  using TSource = std::variant<std::reference_wrapper<Config>, std::reference_wrapper<std::istream>>;

  template<typename TItem>
  auto loadRawItem(const std::string &t_key, TSource &t_source, bool t_add_type_hash = false) {
    auto item = get<TItem>(storage_, t_key);
    if (!item) {
      TItem data;
      load(data, t_source, t_key, t_add_type_hash);

      item = set(storage_, t_key, std::move(data));
    }
    return item;
  }

  template<typename TItem>
  auto load(TItem &t_item, TSource t_source, const std::string &t_key, bool t_add_type_hash) {
    std::visit([&t_item, &t_key, &t_add_type_hash, this](auto &&tt_source) {
                 return this->load(t_item, tt_source.get(), t_key, t_add_type_hash);
               },
               t_source);
  }

  template<typename TItem>
  auto load(TItem &t_item, const sdsl::cache_config &t_config, const std::string &t_key, bool t_add_type_hash) {
    if (!sdsl::load_from_cache(t_item, t_key, t_config, t_add_type_hash))
      throw std::invalid_argument("File not found (Key: '" + t_key + "')");
  }

  template<typename TItem>
  auto load(TItem &t_item, std::istream &t_in, [[maybe_unused]] const std::string &t_key, bool) {
    sdsl::load(t_item, t_in);
  }

  template<typename TItem>
  auto loadItem(const std::string &t_key, TSource &t_source, bool t_add_type_hash = false) {
    auto item = loadRawItem<TItem>(t_key, t_source, t_add_type_hash);
    return std::cref(*item);
  }

  template<typename TBv, typename TBvRank = typename TBv::rank_1_type>
  auto loadBVRank(const std::string &t_key, TSource &t_source, bool t_add_type_hash = false) {
    auto key_rank = t_key + "_rank";
    auto item_rank = get<TBvRank>(storage_, key_rank);
    if (!item_rank) {
      auto item_bv = loadRawItem<TBv>(t_key, t_source, t_add_type_hash);

      TBvRank rank;
      load(rank, t_source, t_key, t_add_type_hash);
      rank.set_vector(item_bv);

      item_rank = set(storage_, key_rank, std::move(rank));
    }

    return std::cref(*item_rank);
  }

  template<typename TBv, typename TBvSelect = typename TBv::select_1_type>
  auto loadBVSelect(const std::string &t_key, TSource &t_source, bool t_add_type_hash = false) {
    auto key_select = t_key + "_select";
    auto item_select = get<TBvSelect>(storage_, key_select);
    if (!item_select) {
      auto item_bv = loadRawItem<TBv>(t_key, t_source, t_add_type_hash);

      TBvSelect select;
      load(select, t_source, t_key, t_add_type_hash);
      select.set_vector(item_bv);

      item_select = set(storage_, key_select, std::move(select));
    }

    return std::cref(*item_select);
  }

  template<typename TItem>
  std::size_t serializeItem(const std::string &t_key,
                            std::ostream &out,
                            sdsl::structure_tree_node *v,
                            const std::string &name) const {
    auto item = get<TItem>(storage_, t_key);
    if (item) {
      return sdsl::serialize(*item, out, v, name);
    }
    return sdsl::serialize_empty_object<TItem>(out, v, name);
  }

  template<typename TItem, typename TItemRank = typename TItem::rank_1_type>
  std::size_t serializeRank(const std::string &t_key,
                            std::ostream &out,
                            sdsl::structure_tree_node *v,
                            const std::string &name) const {
    return serializeItem<TItemRank>(t_key + "_rank", out, v, name);
  }

  template<typename TItem, typename TItemSelect = typename TItem::select_1_type>
  std::size_t serializeSelect(const std::string &t_key,
                              std::ostream &out,
                              sdsl::structure_tree_node *v,
                              const std::string &name) const {
    return serializeItem<TItemSelect>(t_key + "_select", out, v, name);
  }

  //********************
  //********************
  //********************

  std::size_t n_ = 0;
  TStorage storage_;
  std::array<std::string, static_cast<u_int8_t>(ItemKey::NUM_ITEMS)> keys_;

  std::shared_ptr<LocateIndex> index_ = nullptr;
};

template<typename TBackwardNav, typename TUpdateToeholdData, typename TComputeAllValues, typename TGetInitialToeholdData, typename TGetSymbol, typename TCreateFullRange, typename TIsRangeEmpty>
class RIndexBase : public LocateIndex {
 public:
  RIndexBase(const TBackwardNav &t_lf,
             const TUpdateToeholdData &t_update_toehold_data,
             const TComputeAllValues &t_compute_all_values,
             std::size_t t_bwt_size,
             const TGetInitialToeholdData &t_get_initial_toehold_data,
             const TGetSymbol &t_get_symbol,
             const TCreateFullRange &t_create_full_range,
             const TIsRangeEmpty &t_is_range_empty)
      : lf_{t_lf},
        update_toehold_data_{t_update_toehold_data},
        compute_all_values_{t_compute_all_values},
        bwt_size_{t_bwt_size},
        get_initial_toehold_data_{t_get_initial_toehold_data},
        get_symbol_{t_get_symbol},
        create_full_range_{t_create_full_range},
        is_range_empty_{t_is_range_empty} {
  }

  std::vector<std::size_t> Locate(const std::string &t_pattern) const override {
    std::vector<std::size_t> values;
    auto report = [&values](const auto &v) { values.emplace_back(v); };

    Locate(t_pattern, report);

    return values;
  }

  template<typename TPattern, typename TReport>
  void Locate(const TPattern &t_pattern, TReport &t_report) const {
    auto range = create_full_range_(bwt_size_);

    auto i = t_pattern.size() - 1;
    //TODO use default value (step == 0) instead of get_initial_toehold_data_
    auto toehold_data = get_initial_toehold_data_(i);

    for (auto it = rbegin(t_pattern); it != rend(t_pattern) && !is_range_empty_(range); ++it, --i) {
      auto c = get_symbol_(*it);

      auto next_range = lf_(range, c);
      update_toehold_data_(range, next_range, c, i, toehold_data);

      range = next_range;
    }

    if (!is_range_empty_(range)) {
      compute_all_values_(range, toehold_data, t_report);
    }
  }

  std::pair<std::size_t, std::size_t> Count(const std::string &t_pattern) const override {
    std::pair<std::size_t, std::size_t> range;
    auto report = [&range](const auto &tt_range) {
      const auto &[start, end] = tt_range;
      range = {start, end};
    };

    Count(t_pattern, report);

    return range;
  }

  template<typename TPattern, typename TReport>
  void Count(const TPattern &t_pattern, TReport &t_report) const {
    auto range = create_full_range_(bwt_size_);

    for (auto it = rbegin(t_pattern); it != rend(t_pattern) && !is_range_empty_(range); ++it) {
      auto c = get_symbol_(*it);
      range = lf_(range, c);
    }

    t_report(range);
  }

 private:

  TBackwardNav lf_;
  TUpdateToeholdData update_toehold_data_;
  TComputeAllValues compute_all_values_;

  std::size_t bwt_size_;
  TGetInitialToeholdData get_initial_toehold_data_;

  TGetSymbol get_symbol_;

  TCreateFullRange create_full_range_;
  TIsRangeEmpty is_range_empty_;
};

template<typename TBackwardNav, typename TGetLastValue, typename TComputeAllValues, typename TGetFinalValue, typename TGetSymbol>
auto buildSharedPtrRIndex(const TBackwardNav &t_lf,
                          const TGetLastValue &t_get_last_value,
                          const TComputeAllValues &t_compute_all_values,
                          std::size_t t_bwt_size,
                          const TGetFinalValue &t_get_final_sa_value,
                          const TGetSymbol &t_get_symbol) {
  using Range = std::pair<std::size_t, std::size_t>;
  using TFnCreateFullRange = std::function<Range(std::size_t)>;
  auto create_full_range = [](auto tt_seq_size) { return Range{0, tt_seq_size - 1}; };

  using TFnIsRangeEmpty = std::function<bool(const Range &)>;
  auto is_range_empty = [](const auto &tt_range) { return tt_range.second < tt_range.first; };

  return std::make_shared<RIndexBase<TBackwardNav,
                                     TGetLastValue,
                                     TComputeAllValues,
                                     TGetFinalValue,
                                     TGetSymbol,
                                     TFnCreateFullRange,
                                     TFnIsRangeEmpty>>(
      t_lf,
      t_get_last_value,
      t_compute_all_values,
      t_bwt_size,
      t_get_final_sa_value,
      t_get_symbol,
      create_full_range,
      is_range_empty);
}

}

#endif //SRI_INDEX_BASE_H_
