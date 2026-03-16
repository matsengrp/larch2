#pragma once

#include <limits>
#include <meta>

namespace larch {

struct empty_type {};

template <typename T>
concept Structural = requires { []<T>() {}; };

template <auto V>
struct constant_wrapper {
  using type = decltype(V);
  static constexpr type value = V;
};

template <auto V>
using cw = constant_wrapper<V>;

inline constexpr std::size_t no_idx = std::numeric_limits<std::size_t>::max();
using no_idx_v = cw<no_idx>;

template <typename T>
concept TemplateInstantiation = has_template_arguments(^^T);

template <typename T, std::meta::info Tmp>
concept Specialization = TemplateInstantiation<T> and template_of(^^T) == Tmp;

template <typename T>
concept Enum = std::is_enum_v<T>;

template <auto V>
static constexpr bool is_enum = Enum<decltype(V)>;

template <auto... Vs>
struct nttp_list {
  using self = nttp_list;

  static consteval std::vector<std::meta::info> args() {
    return template_arguments_of(^^self);
  }

  static constexpr std::size_t size = sizeof...(Vs);

  static constexpr auto sequence = std::make_index_sequence<size>{};

  template <std::size_t I>
    requires(I < size)
  using type_at = [:std::meta::type_of(args()[I]):];

  template <std::size_t I>
    requires(I < size)
  static constexpr auto at = std::meta::extract<type_at<I>>(args()[I]);

  static constexpr bool has_common_type =
      requires { typename std::common_type<decltype(Vs)...>::type; };

  template <auto V>
  static constexpr bool contains = ((Vs == V) || ...);

  template <auto V>
  static constexpr std::size_t index_of = [] {
    std::size_t i = 0;
    ((Vs == V ? false : (++i, true)) && ...);
    return i;
  }();
};

template <Enum E>
using enumerators_list = [:substitute(^^nttp_list, enumerators_of(^^E)):];

}  // namespace larch
