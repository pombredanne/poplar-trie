#ifndef POPLAR_TRIE_COMPACT_HASH_TRIE_HPP
#define POPLAR_TRIE_COMPACT_HASH_TRIE_HPP

#include <map>

#include "BitChunk.hpp"
#include "BitVector.hpp"
#include "CompactHashTable.hpp"
#include "IntVector.hpp"
#include "bijective_hash.hpp"

namespace poplar {

template <uint32_t Factor = 80, uint32_t DspBits = 3, typename Table = CompactHashTable<7>,
          typename Hash = bijective_hash::SplitMix>
class CompactHashTrie {
 private:
  static_assert(0 < Factor and Factor < 100);
  static_assert(0 < DspBits and DspBits < 64);

 public:
  static constexpr uint64_t NIL_ID = UINT64_MAX;
  static constexpr uint32_t MIN_CAPA_BITS = 16;

  static constexpr uint32_t DSP1_BITS = DspBits;
  static constexpr uint64_t DSP1_MASK = (1ULL << DSP1_BITS) - 1;
  static constexpr uint32_t DSP2_BITS = Table::VAL_BITS;
  static constexpr uint32_t DSP2_MASK = Table::VAL_MASK;

 public:
  CompactHashTrie() = default;

  CompactHashTrie(uint32_t capa_bits, uint32_t symb_bits, uint32_t cht_capa_bits = 0) {
    capa_size_ = size_p2_t{std::max(MIN_CAPA_BITS, capa_bits)};
    symb_size_ = size_p2_t{symb_bits};

    max_size_ = static_cast<uint64_t>(capa_size_.size() * Factor / 100.0);

    hash_ = Hash{capa_size_.bits() + symb_size_.bits()};
    table_ = IntVector{capa_size_.size(), symb_size_.bits() + DSP1_BITS};
    aux_cht_ = Table{capa_size_.bits(), cht_capa_bits};
  }

  ~CompactHashTrie() = default;

  uint64_t get_root() const {
    assert(size_ != 0);
    return 0;
  }

  void add_root() {
    assert(size_ == 0);
    size_ = 1;
  }

  uint64_t find_child(uint64_t node_id, uint64_t symb) const {
    assert(node_id < capa_size_.size());
    assert(symb < symb_size_.size());

    if (size_ == 0) {
      return NIL_ID;
    }

    decomp_val_t dec = decompose_(hash_.hash(make_key_(node_id, symb)));

    for (uint64_t i = dec.mod, cnt = 1;; i = right_(i), ++cnt) {
      if (i == get_root()) {
        // because the root's dsp value is zero though it is defined
        continue;
      }

      if (compare_dsp_(i, 0)) {
        // this slot is empty
        return NIL_ID;
      }

      if (compare_dsp_(i, cnt) and dec.quo == get_quo_(i)) {
        return i;
      }
    }
  }

  ac_res_type add_child(uint64_t& node_id, uint64_t symb) {
    assert(node_id < capa_size_.size());
    assert(symb < symb_size_.size());

    decomp_val_t dec = decompose_(hash_.hash(make_key_(node_id, symb)));

    for (uint64_t i = dec.mod, cnt = 1;; i = right_(i), ++cnt) {
      // because the root's dsp value is zero though it is defined
      if (i == get_root()) {
        continue;
      }

      if (compare_dsp_(i, 0)) {
        // this slot is empty
        if (size_ == max_size_) {
          return ac_res_type::NEEDS_TO_EXPAND;
        }

        update_slot_(i, dec.quo, cnt);

        ++size_;
        node_id = i;

        return ac_res_type::SUCCESS;
      }

      if (compare_dsp_(i, cnt) and dec.quo == get_quo_(i)) {
        node_id = i;
        return ac_res_type::ALREADY_STORED;
      }
    }
  }

  std::pair<uint64_t, uint64_t> get_parent_and_symb(uint64_t node_id) const {
    assert(node_id < capa_size_.size());

    if (compare_dsp_(node_id, 0)) {
      // root or not exist
      return {NIL_ID, 0};
    }

    uint64_t dist = get_dsp_(node_id) - 1;
    uint64_t init_id = dist <= node_id ? node_id - dist : table_.size() - (dist - node_id);
    uint64_t orig_key = hash_.hash_inv(get_quo_(node_id) << capa_size_.bits() | init_id);

    // Returns pair (parent, label)
    return std::make_pair(orig_key >> symb_size_.bits(), orig_key & symb_size_.mask());
  }

  class NodeMap {
   public:
    NodeMap() = default;

    NodeMap(IntVector&& map_high, IntVector&& map_low, BitVector&& done_flags)
        : map_high_(std::move(map_high)),
          map_low_(std::move(map_low)),
          done_flags_(std::move(done_flags)) {}

    ~NodeMap() = default;

    uint64_t operator[](uint64_t i) const {
      if (!done_flags_[i]) {
        return UINT64_MAX;
      }
      if (map_high_.size() == 0) {
        return map_low_[i];
      } else {
        return map_low_[i] | (map_high_[i] << map_low_.width());
      }
    }

    uint64_t size() const {
      return map_low_.size();
    }

    NodeMap(const NodeMap&) = delete;
    NodeMap& operator=(const NodeMap&) = delete;

    NodeMap(NodeMap&&) noexcept = default;
    NodeMap& operator=(NodeMap&&) noexcept = default;

   private:
    IntVector map_high_{};
    IntVector map_low_{};
    BitVector done_flags_{};
  };

  bool needs_to_expand() const {
    return max_size() <= size();
  }

  NodeMap expand() {
    // CompactHashTrie new_ht{capa_bits() + 1, symb_size_.bits(), aux_cht_.capa_bits()};
    CompactHashTrie new_ht{capa_bits() + 1, symb_size_.bits()};
    new_ht.add_root();

#ifdef POPLAR_ENABLE_EX_STATS
    new_ht.num_resize_ = num_resize_ + 1;
#endif

    BitVector done_flags(capa_size());
    done_flags.set(get_root());

    size_p2_t low_size{table_.width()};
    IntVector map_high;

    if (low_size.bits() < new_ht.capa_bits()) {
      map_high = IntVector(capa_size(), new_ht.capa_bits() - low_size.bits());
    }

    std::vector<std::pair<uint64_t, uint64_t>> path;
    path.reserve(256);

    auto get_mapping = [&](uint64_t i) -> uint64_t {
      if (map_high.size() == 0) {
        return table_[i];
      } else {
        return table_[i] | (map_high[i] << low_size.bits());
      }
    };

    auto set_mapping = [&](uint64_t i, uint64_t v) {
      if (map_high.size() == 0) {
        table_.set(i, v);
      } else {
        table_.set(i, v & low_size.mask());
        map_high.set(i, v >> low_size.bits());
      }
    };

    // 0 is root
    for (uint64_t i = 1; i < table_.size(); ++i) {
      if (done_flags[i] or compare_dsp_(i, 0)) {
        // skip already processed or empty elements
        continue;
      }

      path.clear();
      uint64_t node_id = i;

      do {
        auto [parent, label] = get_parent_and_symb(node_id);
        assert(parent != NIL_ID);
        path.emplace_back(std::make_pair(node_id, label));
        node_id = parent;
      } while (!done_flags[node_id]);

      uint64_t new_node_id = get_mapping(node_id);

      for (auto rit = std::rbegin(path); rit != std::rend(path); ++rit) {
        auto ret = new_ht.add_child(new_node_id, rit->second);
        assert(ret == ac_res_type::SUCCESS);
        set_mapping(rit->first, new_node_id);
        done_flags.set(rit->first);
      }
    }

    NodeMap node_map{std::move(map_high), std::move(table_), std::move(done_flags)};
    std::swap(*this, new_ht);

    return node_map;
  }

  uint64_t size() const {
    return size_;
  }

  uint64_t max_size() const {
    return max_size_;
  }

  uint64_t capa_size() const {
    return capa_size_.size();
  }

  uint32_t capa_bits() const {
    return capa_size_.bits();
  }

  uint64_t symb_size() const {
    return symb_size_.size();
  }

  uint32_t symb_bits() const {
    return symb_size_.bits();
  }

  void show_stat(std::ostream& os, int level = 0) const {
    std::string indent(level, '\t');
    os << indent << "stat:CompactHashTrie\n";
    os << indent << "\tfactor:" << Factor << "\n";
    os << indent << "\tdsp1st_bits:" << DSP1_BITS << "\n";
    os << indent << "\tdsp2nd_bits:" << DSP2_BITS << "\n";
    os << indent << "\tsize:" << size() << "\n";
    os << indent << "\tcapa_size:" << capa_size() << "\n";
    os << indent << "\tcapa_bits:" << capa_bits() << "\n";
    os << indent << "\tsymb_size:" << symb_size() << "\n";
    os << indent << "\tsymb_bits:" << symb_bits() << "\n";
#ifdef POPLAR_ENABLE_EX_STATS
    os << indent << "\tnum_resize:" << num_resize_ << "\n";
    os << indent << "\trate_dsp1st:" << double(num_dsps_[0]) / size() << "\n";
    os << indent << "\trate_dsp2nd:" << double(num_dsps_[1]) / size() << "\n";
    os << indent << "\trate_dsp3rd:" << double(num_dsps_[2]) / size() << "\n";
#endif
    hash_.show_stat(os, level + 1);
    aux_cht_.show_stat(os, level + 1);
  }

  CompactHashTrie(const CompactHashTrie&) = delete;
  CompactHashTrie& operator=(const CompactHashTrie&) = delete;

  CompactHashTrie(CompactHashTrie&& rhs) noexcept = default;
  CompactHashTrie& operator=(CompactHashTrie&& rhs) noexcept = default;

 private:
  Hash hash_{};
  IntVector table_{};
  Table aux_cht_{};  // 2nd dsp
  std::map<uint64_t, uint64_t> aux_map_{};  // 3rd dsp
  uint64_t size_{};  // # of registered nodes
  uint64_t max_size_{};  // Factor% of the capacity
  size_p2_t capa_size_{};
  size_p2_t symb_size_{};

#ifdef POPLAR_ENABLE_EX_STATS
  uint64_t num_resize_{};
  uint64_t num_dsps_[3]{};
#endif

  uint64_t make_key_(uint64_t node_id, uint64_t symb) const {
    return (node_id << symb_size_.bits()) | symb;
  }
  decomp_val_t decompose_(uint64_t x) const {
    return {x >> capa_size_.bits(), x & capa_size_.mask()};
  }
  uint64_t right_(uint64_t slot_id) const {
    return (slot_id + 1) & capa_size_.mask();
  }

  uint64_t get_quo_(uint64_t slot_id) const {
    return table_[slot_id] >> DSP1_BITS;
  }
  uint64_t get_dsp_(uint64_t slot_id) const {
    uint64_t dsp = table_[slot_id] & DSP1_MASK;
    if (dsp < DSP1_MASK) {
      return dsp;
    }

    dsp = aux_cht_.get(slot_id);
    if (dsp != UINT64_MAX) {
      return dsp + DSP1_MASK;
    }

    auto it = aux_map_.find(slot_id);
    assert(it != aux_map_.end());
    return it->second;
  }

  bool compare_dsp_(uint64_t slot_id, uint64_t rhs) const {
    uint64_t lhs = table_[slot_id] & DSP1_MASK;
    if (lhs < DSP1_MASK) {
      return lhs == rhs;
    }
    if (rhs < DSP1_MASK) {
      return false;
    }

    lhs = aux_cht_.get(slot_id);
    if (lhs != UINT64_MAX) {
      return lhs + DSP1_MASK == rhs;
    }
    if (rhs < DSP1_MASK + DSP2_MASK) {
      return false;
    }

    auto it = aux_map_.find(slot_id);
    assert(it != aux_map_.end());
    return it->second == rhs;
  }

  void update_slot_(uint64_t slot_id, uint64_t quo, uint64_t dsp) {
    assert(table_[slot_id] == 0);
    assert(quo < symb_size_.size());

    uint64_t v = quo << DSP1_BITS;

    if (dsp < DSP1_MASK) {
      v |= dsp;
    } else {
      v |= DSP1_MASK;
      uint64_t _dsp = dsp - DSP1_MASK;
      if (_dsp < DSP2_MASK) {
        aux_cht_.set(slot_id, _dsp);
      } else {
        aux_map_.insert(std::make_pair(slot_id, dsp));
      }
    }

#ifdef POPLAR_ENABLE_EX_STATS
    if (dsp < DSP1_MASK) {
      ++num_dsps_[0];
    } else if (dsp < DSP1_MASK + DSP2_MASK) {
      ++num_dsps_[1];
    } else {
      ++num_dsps_[2];
    }
#endif

    table_.set(slot_id, v);
  }
};

}  // namespace poplar

#endif  // POPLAR_TRIE_COMPACT_HASH_TRIE_HPP