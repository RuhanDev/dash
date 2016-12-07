#ifndef DASH__TYPES_H_
#define DASH__TYPES_H_

#include <array>
#include <type_traits>
#include <dash/dart/if/dart_types.h>
#include <dash/internal/Unit.h>


namespace dash {

typedef enum MemArrange {
  MEM_ARRANGE_UNDEFINED = 0,
  ROW_MAJOR,
  COL_MAJOR
} MemArrange;

namespace internal {

typedef enum DistributionType {
  DIST_UNDEFINED = 0,
  DIST_NONE,
  DIST_BLOCKED,      // = BLOCKCYCLIC(ceil(nelem/nunits))
  DIST_CYCLIC,       // = BLOCKCYCLIC(1) Will be removed
  DIST_BLOCKCYCLIC,
  DIST_TILE
} DistributionType; // general blocked distribution

} // namespace internal

/**
 * Scalar type for a dimension value, with 0 indicating
 * the first dimension.
 */
typedef int dim_t;

namespace internal {

#if defined(DASH_ENABLE_DEFAULT_INDEX_TYPE_LONG)
  typedef          long default_signed_index;
  typedef unsigned long default_unsigned_index;
#elif defined(DASH_ENABLE_DEFAULT_INDEX_TYPE_INT)
  typedef          int  default_signed_index;
  typedef unsigned int  default_unsigned_index;
#else
  typedef ssize_t       default_signed_index;
  typedef size_t        default_unsigned_index;
#endif

}

/**
 * Signed integer type used as default for index values.
 */
typedef internal::default_signed_index    default_index_t;

/**
 * Unsigned integer type used as default for extent values.
 */
typedef internal::default_unsigned_index default_extent_t;

/**
 * Unsigned integer type used as default for size values.
 */
typedef internal::default_unsigned_index   default_size_t;

/**
 * Difference type for global pointers.
 */
typedef internal::default_signed_index         gptrdiff_t;

template<
  dash::dim_t NumDimensions,
  typename IndexType = dash::default_index_t>
struct Point {
  ::std::array<IndexType, NumDimensions> coords;
};

template<
  dash::dim_t NumDimensions,
  typename SizeType = dash::default_extent_t>
struct Extent {
  ::std::array<SizeType, NumDimensions> sizes;
};


/**
 * Type traits for mapping to DART data types.
 */
template<typename Type>
struct dart_datatype {
  static const dart_datatype_t value;
};

template<typename Type>
const dart_datatype_t dart_datatype<Type>::value = DART_TYPE_UNDEFINED;


template<>
struct dart_datatype<char> {
  static const dart_datatype_t value;
};

template<>
struct dart_datatype<int> {
  static const dart_datatype_t value;
};

template<>
struct dart_datatype<unsigned int> {
  static const dart_datatype_t value;
};

template<>
struct dart_datatype<float> {
  static const dart_datatype_t value;
};

template<>
struct dart_datatype<long> {
  static const dart_datatype_t value;
};

template<>
struct dart_datatype<unsigned long> {
  static const dart_datatype_t value;
};

template<>
struct dart_datatype<double> {
  static const dart_datatype_t value;
};

template <typename T>
inline dart_storage_t dart_storage(int nvalues) {
  dart_storage_t ds;
  ds.dtype = dart_datatype<T>::value;
  ds.nelem = nvalues;
  if (DART_TYPE_UNDEFINED == ds.dtype) {
    ds.dtype = DART_TYPE_BYTE;
    ds.nelem = nvalues * sizeof(T);
  }
  return ds;
}

/**
 * Unit ID to use for team-local IDs.
 *
 * Note that this is returned by calls to dash::Team::myid(),
 * including \c dash::Team::All().myid() as it is handled as
 * a team as well.
 *
 * \see unit_id
 * \see global_unit_t
 */
typedef struct
dash::unit_id<dash::local_unit>
local_unit_t;

/**
 * Unit ID to use for global IDs.
 *
 * Note that this typed is returned by \c dash::myid()
 * and \c dash::Team::GlobalUnitID().
 *
 * \see unit_id
 * \see local_unit_t
 */
typedef struct
dash::unit_id<dash::global_unit>
global_unit_t;

/**
 * Invalid local unit ID.
 *
 * This is a typed version of \ref DART_UNDEFINED_UNIT_ID.
 */
constexpr local_unit_t   UNDEFINED_LOCAL_UNIT_ID{DART_UNDEFINED_UNIT_ID};

/**
 * Invalid global unit ID.
 *
 * This is a typed version of \ref DART_UNDEFINED_UNIT_ID.
 */
constexpr global_unit_t UNDEFINED_GLOBAL_UNIT_ID{DART_UNDEFINED_UNIT_ID};

} // namespace dash

#endif // DASH__TYPES_H_
