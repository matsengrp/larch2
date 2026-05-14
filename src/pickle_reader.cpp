#include <larch/pickle_reader.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <unordered_map>

namespace larch {
namespace {

constexpr std::size_t k_max_stack_entries = 10'000;
constexpr std::size_t k_max_memo_entries = 1'000'000;
constexpr std::size_t k_max_tensors = 10'000;

// Pickle protocol 2 opcodes used by torch.save state dicts.
enum : uint8_t {
  OP_PROTO = 0x80,
  OP_FRAME = 0x95,
  OP_EMPTY_DICT = 0x7d,
  OP_EMPTY_LIST = 0x5d,
  OP_EMPTY_TUPLE = 0x29,
  OP_MARK = 0x28,
  OP_TUPLE = 0x74,
  OP_TUPLE1 = 0x85,
  OP_TUPLE2 = 0x86,
  OP_TUPLE3 = 0x87,
  OP_SETITEMS = 0x75,
  OP_SETITEM = 0x73,
  OP_BINUNICODE = 0x58,
  OP_SHORT_BINUNICODE = 0x8c,
  OP_GLOBAL = 0x63,
  OP_STACK_GLOBAL = 0x93,
  OP_REDUCE = 0x52,
  OP_NEWOBJ = 0x81,
  OP_BUILD = 0x62,
  OP_BINPERSID = 0x51,
  OP_BINPUT = 0x71,
  OP_LONG_BINPUT = 0x72,
  OP_BINGET = 0x68,
  OP_LONG_BINGET = 0x6a,
  OP_MEMOIZE = 0x94,
  OP_BININT1 = 0x4b,
  OP_BININT2 = 0x4d,
  OP_BININT = 0x4a,
  OP_LONG1 = 0x8a,
  OP_NONE = 0x4e,
  OP_NEWTRUE = 0x88,
  OP_NEWFALSE = 0x89,
  OP_STOP = 0x2e,
  OP_APPEND = 0x61,
  OP_APPENDS = 0x65,
  OP_POP = 0x30,
  OP_DUP = 0x32,
  OP_BINFLOAT = 0x47,
  OP_SHORT_BINBYTES = 0x43,
  OP_BINBYTES = 0x42,
};

template <typename UInt>
UInt native_from_little_endian(UInt v) {
  if constexpr (std::endian::native == std::endian::little) {
    return v;
  } else if constexpr (std::endian::native == std::endian::big) {
    return std::byteswap(v);
  } else {
    throw std::runtime_error{"pickle_reader: unsupported mixed-endian host"};
  }
}

template <typename UInt>
UInt native_from_big_endian(UInt v) {
  if constexpr (std::endian::native == std::endian::big) {
    return v;
  } else if constexpr (std::endian::native == std::endian::little) {
    return std::byteswap(v);
  } else {
    throw std::runtime_error{"pickle_reader: unsupported mixed-endian host"};
  }
}

// Lightweight tagged value for the pickle VM stack.
struct pval {
  enum tag_t {
    t_none,
    t_bool,
    t_int,
    t_float,
    t_string,
    t_tuple,
    t_list,
    t_dict,
    t_global,
    t_persid,
    t_mark,
    t_reduced,
  } tag = t_none;

  bool bool_val = false;
  int64_t int_val = 0;
  double float_val = 0.0;
  std::string str_val;
  std::vector<pval> items;                        // tuple, list
  std::vector<std::pair<pval, pval>> dict_items;  // dict
  std::string module, name;                       // global

  static pval make_none() { return {.tag = t_none}; }
  static pval make_bool(bool v) { return {.tag = t_bool, .bool_val = v}; }
  static pval make_int(int64_t v) { return {.tag = t_int, .int_val = v}; }
  static pval make_float(double v) { return {.tag = t_float, .float_val = v}; }
  static pval make_string(std::string s) {
    return {.tag = t_string, .str_val = std::move(s)};
  }
  static pval make_tuple(std::vector<pval> items) {
    return {.tag = t_tuple, .items = std::move(items)};
  }
  static pval make_list() { return {.tag = t_list}; }
  static pval make_dict() { return {.tag = t_dict}; }
  static pval make_global(std::string mod, std::string n) {
    return {.tag = t_global, .module = std::move(mod), .name = std::move(n)};
  }
  static pval make_persid(std::vector<pval> id) {
    return {.tag = t_persid, .items = std::move(id)};
  }
  static pval make_mark() { return {.tag = t_mark}; }
  static pval make_reduced() { return {.tag = t_reduced}; }
};

class pickle_vm {
  const uint8_t* pos_;
  const uint8_t* end_;
  std::vector<pval> stack_;
  std::unordered_map<uint32_t, pval> memo_;
  uint32_t next_memo_ = 0;

  // Accumulated tensor info and current parameter name.
  std::vector<tensor_info> tensors_;

  void require(std::size_t n) const {
    if (static_cast<std::size_t>(end_ - pos_) < n)
      throw std::runtime_error{"pickle_reader: unexpected end of data"};
  }

  uint8_t read_u8() {
    require(1);
    return *pos_++;
  }
  uint16_t read_u16() {
    require(2);
    uint16_t v;
    std::memcpy(&v, pos_, 2);
    pos_ += 2;
    return native_from_little_endian(v);
  }
  uint32_t read_u32() {
    require(4);
    uint32_t v;
    std::memcpy(&v, pos_, 4);
    pos_ += 4;
    return native_from_little_endian(v);
  }
  int32_t read_i32() {
    uint32_t bits = read_u32();
    int32_t v;
    std::memcpy(&v, &bits, 4);
    return v;
  }
  uint64_t read_u64() {
    require(8);
    uint64_t v;
    std::memcpy(&v, pos_, 8);
    pos_ += 8;
    return native_from_little_endian(v);
  }
  double read_f64_be() {
    // BINFLOAT stores IEEE 754 big-endian double.
    require(8);
    uint64_t bits;
    std::memcpy(&bits, pos_, 8);
    pos_ += 8;
    bits = native_from_big_endian(bits);
    double v;
    std::memcpy(&v, &bits, 8);
    return v;
  }

  std::string read_line() {
    auto nl = static_cast<const uint8_t*>(
        std::memchr(pos_, '\n', static_cast<std::size_t>(end_ - pos_)));
    if (!nl) throw std::runtime_error{"pickle_reader: unterminated line"};
    std::string s{reinterpret_cast<const char*>(pos_),
                  static_cast<std::size_t>(nl - pos_)};
    pos_ = nl + 1;
    return s;
  }

  std::string read_string(std::size_t len) {
    require(len);
    std::string s{reinterpret_cast<const char*>(pos_), len};
    pos_ += len;
    return s;
  }

  void check_stack_cap() const {
    if (stack_.size() > k_max_stack_entries)
      throw std::runtime_error{
          "pickle_reader: stack entry limit exceeded (max 10000)"};
  }

  void check_memo_cap() const {
    if (memo_.size() > k_max_memo_entries)
      throw std::runtime_error{
          "pickle_reader: memo entry limit exceeded (max 1000000)"};
  }

  void check_tensor_cap() const {
    if (tensors_.size() > k_max_tensors)
      throw std::runtime_error{
          "pickle_reader: tensor limit exceeded (max 10000)"};
  }

  void push(pval v) {
    stack_.push_back(std::move(v));
    check_stack_cap();
  }

  void memoize(uint32_t idx) {
    if (!stack_.empty()) {
      if (!memo_.contains(idx) && memo_.size() >= k_max_memo_entries)
        throw std::runtime_error{
            "pickle_reader: memo entry limit exceeded (max 1000000)"};
      memo_[idx] = stack_.back();
      check_memo_cap();
    }
  }

  pval pop() {
    if (stack_.empty())
      throw std::runtime_error{"pickle_reader: stack underflow"};
    pval v = std::move(stack_.back());
    stack_.pop_back();
    return v;
  }

  pval& top() {
    if (stack_.empty())
      throw std::runtime_error{"pickle_reader: stack underflow"};
    return stack_.back();
  }

  // Pop items from the stack down to (and including) the MARK.
  // Returns items in order (bottom to top).
  std::vector<pval> pop_to_mark() {
    std::vector<pval> items;
    while (!stack_.empty()) {
      if (stack_.back().tag == pval::t_mark) {
        stack_.pop_back();
        std::reverse(items.begin(), items.end());
        return items;
      }
      items.push_back(std::move(stack_.back()));
      stack_.pop_back();
    }
    throw std::runtime_error{"pickle_reader: MARK not found on stack"};
  }

  // Try to extract tensor_info from a REDUCE of _rebuild_tensor_v2.
  // Returns true if successfully extracted.
  bool try_extract_tensor(pval const& callable, pval const& args,
                          pval& result) {
    if (callable.tag != pval::t_global) return false;
    if (callable.name != "_rebuild_tensor_v2") return false;
    if (args.tag != pval::t_tuple || args.items.size() < 5) return false;

    // args: (persid, storage_offset, shape, stride, requires_grad, ...)
    auto const& persid = args.items[0];
    auto const& offset_v = args.items[1];
    auto const& shape_v = args.items[2];
    auto const& stride_v = args.items[3];

    tensor_info ti;

    // Extract storage key from persistent_id tuple.
    if (persid.tag == pval::t_persid && persid.items.size() >= 5) {
      // items: ("storage", FloatStorage_global, "key", "cpu", num_elements)
      if (persid.items[2].tag == pval::t_string)
        ti.storage_key = persid.items[2].str_val;
      if (persid.items[4].tag == pval::t_int)
        ti.num_elements = persid.items[4].int_val;
    }

    if (offset_v.tag == pval::t_int) ti.storage_offset = offset_v.int_val;

    if (shape_v.tag == pval::t_tuple) {
      for (auto& s : shape_v.items) {
        if (s.tag == pval::t_int) ti.shape.push_back(s.int_val);
      }
    }

    if (stride_v.tag == pval::t_tuple) {
      for (auto& s : stride_v.items) {
        if (s.tag == pval::t_int) ti.stride.push_back(s.int_val);
      }
    }

    // Store tensor_info temporarily; name will be assigned during SETITEMS.
    // Use a reduced value and stash the index.
    result = pval::make_reduced();
    result.int_val = static_cast<int64_t>(tensors_.size());
    tensors_.push_back(std::move(ti));
    check_tensor_cap();
    return true;
  }

 public:
  pickle_vm(std::span<const uint8_t> data)
      : pos_{data.data()}, end_{data.data() + data.size()} {}

  state_dict_info run() {
    while (pos_ < end_) {
      uint8_t op = read_u8();

      switch (op) {
        case OP_PROTO: {
          [[maybe_unused]] uint8_t version = read_u8();
          break;
        }
        case OP_FRAME: {
          read_u64();  // frame length, ignore
          break;
        }
        case OP_EMPTY_DICT:
          push(pval::make_dict());
          break;
        case OP_EMPTY_LIST:
          push(pval::make_list());
          break;
        case OP_EMPTY_TUPLE:
          push(pval::make_tuple({}));
          break;
        case OP_MARK:
          push(pval::make_mark());
          break;

        case OP_TUPLE: {
          auto items = pop_to_mark();
          push(pval::make_tuple(std::move(items)));
          break;
        }
        case OP_TUPLE1: {
          auto a = pop();
          push(pval::make_tuple({std::move(a)}));
          break;
        }
        case OP_TUPLE2: {
          auto b = pop();
          auto a = pop();
          push(pval::make_tuple({std::move(a), std::move(b)}));
          break;
        }
        case OP_TUPLE3: {
          auto c = pop();
          auto b = pop();
          auto a = pop();
          push(pval::make_tuple({std::move(a), std::move(b), std::move(c)}));
          break;
        }

        case OP_SETITEMS: {
          auto items = pop_to_mark();
          auto& dict = top();
          if (dict.tag != pval::t_dict)
            throw std::runtime_error{"pickle_reader: SETITEMS on non-dict"};
          for (std::size_t i = 0; i + 1 < items.size(); i += 2) {
            auto& key = items[i];
            auto& val = items[i + 1];
            // If the value is a reduced tensor, assign its name now.
            if (val.tag == pval::t_reduced && key.tag == pval::t_string) {
              auto idx = static_cast<std::size_t>(val.int_val);
              if (idx < tensors_.size()) {
                tensors_[idx].name = key.str_val;
              }
            }
            dict.dict_items.emplace_back(std::move(key), std::move(val));
          }
          break;
        }
        case OP_SETITEM: {
          auto val = pop();
          auto key = pop();
          auto& dict = top();
          if (dict.tag == pval::t_dict) {
            if (val.tag == pval::t_reduced && key.tag == pval::t_string) {
              auto idx = static_cast<std::size_t>(val.int_val);
              if (idx < tensors_.size()) {
                tensors_[idx].name = key.str_val;
              }
            }
            dict.dict_items.emplace_back(std::move(key), std::move(val));
          }
          break;
        }

        case OP_BINUNICODE: {
          uint32_t len = read_u32();
          push(pval::make_string(read_string(len)));
          break;
        }
        case OP_SHORT_BINUNICODE: {
          uint8_t len = read_u8();
          push(pval::make_string(read_string(len)));
          break;
        }

        case OP_GLOBAL: {
          auto mod = read_line();
          auto name = read_line();
          push(pval::make_global(std::move(mod), std::move(name)));
          break;
        }
        case OP_STACK_GLOBAL: {
          auto name = pop();
          auto mod = pop();
          push(pval::make_global(
              mod.tag == pval::t_string ? std::move(mod.str_val) : "",
              name.tag == pval::t_string ? std::move(name.str_val) : ""));
          break;
        }

        case OP_REDUCE: {
          auto args = pop();
          auto callable = pop();
          pval result = pval::make_reduced();
          if (!try_extract_tensor(callable, args, result)) {
            // For OrderedDict() and other callables, just push a reduced value.
            // If callable is collections.OrderedDict and args is empty tuple,
            // produce a dict.
            if (callable.tag == pval::t_global &&
                callable.name == "OrderedDict") {
              result = pval::make_dict();
            }
          }
          push(std::move(result));
          break;
        }
        case OP_NEWOBJ: {
          auto args = pop();
          auto cls = pop();
          // Same as REDUCE for our purposes.
          pval result = pval::make_reduced();
          if (cls.tag == pval::t_global && cls.name == "OrderedDict") {
            result = pval::make_dict();
          }
          push(std::move(result));
          break;
        }
        case OP_BUILD: {
          pop();  // state -- ignore
          break;
        }

        case OP_BINPERSID: {
          auto pid = pop();
          // Wrap in a persid value, preserving the tuple items.
          if (pid.tag == pval::t_tuple) {
            push(pval::make_persid(std::move(pid.items)));
          } else {
            push(pval::make_persid({}));
          }
          break;
        }

        case OP_BINPUT: {
          uint8_t idx = read_u8();
          memoize(idx);
          break;
        }
        case OP_LONG_BINPUT: {
          uint32_t idx = read_u32();
          memoize(idx);
          break;
        }
        case OP_BINGET: {
          uint8_t idx = read_u8();
          auto it = memo_.find(idx);
          if (it == memo_.end())
            throw std::runtime_error{"pickle_reader: memo key not found"};
          push(it->second);
          break;
        }
        case OP_LONG_BINGET: {
          uint32_t idx = read_u32();
          auto it = memo_.find(idx);
          if (it == memo_.end())
            throw std::runtime_error{"pickle_reader: memo key not found"};
          push(it->second);
          break;
        }
        case OP_MEMOIZE: {
          if (!stack_.empty()) memoize(next_memo_++);
          break;
        }

        case OP_BININT1:
          push(pval::make_int(read_u8()));
          break;
        case OP_BININT2:
          push(pval::make_int(read_u16()));
          break;
        case OP_BININT:
          push(pval::make_int(read_i32()));
          break;
        case OP_LONG1: {
          uint8_t nbytes = read_u8();
          require(nbytes);
          int64_t val = 0;
          // Little-endian signed integer.
          for (int i = nbytes - 1; i >= 0; --i) {
            val = (val << 8) | pos_[i];
          }
          // Sign extend if high bit set.
          if (nbytes > 0 && (pos_[nbytes - 1] & 0x80)) {
            for (int i = nbytes; i < 8; ++i) val |= int64_t{0xff} << (i * 8);
          }
          pos_ += nbytes;
          push(pval::make_int(val));
          break;
        }

        case OP_NONE:
          push(pval::make_none());
          break;
        case OP_NEWTRUE:
          push(pval::make_bool(true));
          break;
        case OP_NEWFALSE:
          push(pval::make_bool(false));
          break;

        case OP_BINFLOAT:
          push(pval::make_float(read_f64_be()));
          break;

        case OP_SHORT_BINBYTES: {
          uint8_t len = read_u8();
          push(pval::make_string(read_string(len)));
          break;
        }
        case OP_BINBYTES: {
          uint32_t len = read_u32();
          push(pval::make_string(read_string(len)));
          break;
        }

        case OP_APPEND: {
          auto val = pop();
          auto& lst = top();
          if (lst.tag == pval::t_list) lst.items.push_back(std::move(val));
          break;
        }
        case OP_APPENDS: {
          auto items = pop_to_mark();
          auto& lst = top();
          if (lst.tag == pval::t_list) {
            for (auto& item : items) lst.items.push_back(std::move(item));
          }
          break;
        }

        case OP_POP:
          pop();
          break;
        case OP_DUP:
          push(top());
          break;

        case OP_STOP:
          goto done;

        default:
          throw std::runtime_error{"pickle_reader: unsupported opcode 0x" +
                                   std::string{
                                       "0123456789abcdef"[op >> 4],
                                       "0123456789abcdef"[op & 0xf],
                                   }};
      }
    }
  done:

    return state_dict_info{.tensors = std::move(tensors_)};
  }
};

}  // namespace

state_dict_info parse_state_dict(std::span<const uint8_t> pickle_data) {
  pickle_vm vm{pickle_data};
  return vm.run();
}

}  // namespace larch
