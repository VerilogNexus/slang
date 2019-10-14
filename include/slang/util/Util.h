//------------------------------------------------------------------------------
// Util.h
// Various utility functions and basic types used throughout the library.
//
// File is under the MIT license; see LICENSE for details.
//------------------------------------------------------------------------------
#pragma once

#include <climits>     // for type size macros
#include <cstddef>     // for std::byte
#include <cstdint>     // for sized integer types
#include <new>         // for placement new
#include <optional>    // for std::optional
#include <string_view> // for std::string_view
#include <utility>     // for many random utility functions

#include "slang/util/Assert.h"
#include "slang/util/NotNull.h"

using std::byte;
using std::int16_t;
using std::int32_t;
using std::int64_t;
using std::int8_t;
using std::intptr_t;
using std::nullptr_t;
using std::optional;
using std::ptrdiff_t;
using std::size_t;
using std::string_view;
using std::uint16_t;
using std::uint32_t;
using std::uint64_t;
using std::uint8_t;
using std::uintptr_t;

using namespace std::literals;

#define UTIL_ENUM_ELEMENT(x) x,
#define UTIL_ENUM_STRING(x) #x,
#define ENUM(name, elements)                                           \
    enum class name { elements(UTIL_ENUM_ELEMENT) };                   \
    inline string_view toString(name e) {                              \
        static const char* strings[] = { elements(UTIL_ENUM_STRING) }; \
        return strings[static_cast<std::underlying_type_t<name>>(e)];  \
    }                                                                  \
    inline std::ostream& operator<<(std::ostream& os, name e) { return os << toString(e); }

#define ENUM_MEMBER(name, elements)                                    \
    enum name { elements(UTIL_ENUM_ELEMENT) };                         \
    friend string_view toString(name e) {                              \
        static const char* strings[] = { elements(UTIL_ENUM_STRING) }; \
        return strings[static_cast<std::underlying_type_t<name>>(e)];  \
    }

#define span_CONFIG_INDEX_TYPE std::size_t
#define span_FEATURE_COMPARISON 0
#include <span.hpp>
using nonstd::span;

#include <bitmask.hpp>
using bitmask_lib::bitmask;

#include <nlohmann/json_fwd.hpp>
using json = nlohmann::json;

#define HAS_METHOD_TRAIT(name)                                                               \
    template<typename, typename T>                                                           \
    struct has_##name {                                                                      \
        static_assert(always_false<T>::value,                                                \
                      "Second template parameter needs to be of function type.");            \
    };                                                                                       \
    template<typename C, typename Ret, typename... Args>                                     \
    struct has_##name<C, Ret(Args...)> {                                                     \
    private:                                                                                 \
        template<typename T>                                                                 \
        static constexpr auto check(T*) ->                                                   \
            typename std::is_same<decltype(std::declval<T>().name(std::declval<Args>()...)), \
                                  Ret>::type;                                                \
        template<typename>                                                                   \
        static constexpr std::false_type check(...);                                         \
        typedef decltype(check<C>(nullptr)) type;                                            \
                                                                                             \
    public:                                                                                  \
        static constexpr bool value = type::value;                                           \
    };                                                                                       \
    template<typename C, typename Ret, typename... Args>                                     \
    static constexpr bool has_##name##_v = has_##name<C, Ret(Args...)>::value

namespace slang {

template<typename T>
struct always_false : std::false_type {};

/// Converts a span of characters into a string_view.
inline string_view to_string_view(span<char> text) {
    return string_view(text.data(), text.size());
}

/// Determines the number of edits to the left string that are required to
/// change it into the right string.
int editDistance(string_view left, string_view right, bool allowReplacements = true,
                 int maxDistance = 0);

inline void hash_combine(size_t&) {
}

/// Hash combining function, based on the function from Boost.
template<typename T, typename... Rest>
inline void hash_combine(size_t& seed, const T& v, Rest... rest) {
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    hash_combine(seed, rest...);
}

#if defined(_MSC_VER)

std::wstring widen(string_view str);
std::string narrow(std::wstring_view str);

#else

inline string_view widen(string_view str) {
    return str;
}

inline string_view narrow(string_view str) {
    return str;
}

#endif

} // namespace slang

namespace detail {

template<typename Tuple, size_t Index = std::tuple_size<Tuple>::value - 1>
struct HashValueImpl {
    static void apply(size_t& seed, const Tuple& tuple) {
        HashValueImpl<Tuple, Index - 1>::apply(seed, tuple);
        slang::hash_combine(seed, std::get<Index>(tuple));
    }
};

template<typename Tuple>
struct HashValueImpl<Tuple, 0> {
    static void apply(size_t& seed, const Tuple& tuple) {
        slang::hash_combine(seed, std::get<0>(tuple));
    }
};

} // namespace detail

namespace std {

template<typename... TT>
struct hash<std::tuple<TT...>> {
    size_t operator()(const std::tuple<TT...>& tt) const {
        size_t seed = 0;
        ::detail::HashValueImpl<std::tuple<TT...>>::apply(seed, tt);
        return seed;
    }
};

} // namespace std
