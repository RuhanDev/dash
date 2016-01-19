#ifndef DASH__ALGORITHM__COPY_H__
#define DASH__ALGORITHM__COPY_H__

#include <dash/GlobIter.h>
#include <dash/Future.h>
#include <dash/algorithm/LocalRange.h>
#include <dash/dart/if/dart_communication.h>

#include <algorithm>
#include <vector>
#include <memory>

//#define DASH__ALGORITHM__COPY__USE_FLUSH

namespace dash {

#ifdef DOXYGEN

/**
 * Copies the elements in the range, defined by \c [in_first, in_last), to
 * another range beginning at \c out_first.
 *
 * In terms of data distribution, ranges passed to \c dash::copy can be local
 * (\c *ValueType) or global (\c GlobIter<ValueType>).
 *
 * The operation performed is non-blocking if the output iterator is an
 * instance of a \c GlobAsyncIter type.
 *
 * Example:
 *
 * \code
 *     // Start asynchronous copying
 *     GlobAsyncIter<T> dest_last =
 *       dash::copy(array_a.lbegin(),
 *                  array_a.lend(),
 *                  array_b.async[200]);
 *     // Overlapping computation here
 *     // ...
 *     // Wait for completion of asynchronous copying:
 *     dest_last.fence();
 * \endcode
 *
 */
template <
  typename ValueType,
  class InputIt,
  class OutputIt >
OutputIt copy(
  InputIt  in_first,
  InputIt  in_last,
  OutputIt out_first);

#endif // DOXYGEN

namespace internal {

/**
 * Blocking implementation of \c dash::copy (global to local) without
 * optimization for local subrange.
 */
template <
  typename ValueType,
  class GlobInputIt >
dash::Future<ValueType *> copy_impl(
  GlobInputIt   in_first,
  GlobInputIt   in_last,
  ValueType   * out_first)
{
  DASH_LOG_TRACE("dash::copy_impl()",
                 "in_first:",  in_first.pos(),
                 "in_last:",   in_last.pos(),
                 "out_first:", out_first);
  auto num_elem_total  = dash::distance(in_first, in_last);
  if (num_elem_total <= 0) {
    DASH_LOG_TRACE("dash::copy_impl", "input range empty");
    return dash::Future<ValueType *>([=]() { return out_first; });
  }
  DASH_LOG_TRACE("dash::copy_impl",
                 "total elements:", num_elem_total,
                 "expected out_last:", out_first + num_elem_total);
  // Input iterators could be relative to a view. Map first input iterator to
  // global index range and use it to resolve last input iterator.
  // Do not use in_last.global() as this would span over the relative input
  // range.
  auto g_in_first      = in_first.global();
  auto g_in_last       = g_in_first + num_elem_total;
  DASH_LOG_TRACE("dash::copy_impl",
                 "g_in_first:", g_in_first.pos(),
                 "g_in_last:",  g_in_last.pos());
  auto pattern         = in_first.pattern();
  auto unit_first      = pattern.unit_at(g_in_first.pos());
  DASH_LOG_TRACE_VAR("dash::copy_impl", unit_first);
  auto unit_last       = pattern.unit_at(g_in_last.pos() - 1);
  DASH_LOG_TRACE_VAR("dash::copy_impl", unit_last);
  typedef typename decltype(pattern)::index_type index_type;
  typedef typename decltype(pattern)::size_type  size_type;

  // Accessed global pointers to be flushed:
#ifdef DASH__ALGORITHM__COPY__USE_FLUSH
  std::vector<dart_gptr_t>   flush_glob_ptrs;
#else
  std::vector<dart_handle_t> flush_glob_ptrs;
#endif

  size_type num_elem_copied = 0;
  if (unit_first == unit_last) {
    // Input range is located at a single remote unit:
    DASH_LOG_TRACE("dash::copy_impl", "input range at single unit");
    auto num_bytes_total = num_elem_total * sizeof(ValueType);
#ifdef DASH__ALGORITHM__COPY__USE_FLUSH
    DASH_ASSERT_RETURNS(
      dart_get(
        out_first,
        in_first.dart_gptr(),
        num_bytes_total),
      DART_OK);
    flush_glob_ptrs.push_back(in_first.dart_gptr());
#else
    dart_handle_t get_handle;
    DASH_ASSERT_RETURNS(
      dart_get_handle(
        out_first,
        in_first.dart_gptr(),
        num_bytes_total,
        &get_handle),
      DART_OK);
    flush_glob_ptrs.push_back(get_handle);
#endif
    num_elem_copied = num_elem_total;
  } else {
    // Input range is spread over several remote units:
    DASH_LOG_TRACE("dash::copy_impl", "input range spans multiple units");
    //
    // Copy elements from every unit:
    //
    // Number of elements located at a single unit:
    auto max_elem_per_unit    = pattern.local_capacity();
    // MPI uses offset type int, do not copy more than INT_MAX bytes:
    int  max_copy_elem        = (std::numeric_limits<int>::max() /
                                 sizeof(ValueType));
    DASH_LOG_TRACE_VAR("dash::copy_impl", max_elem_per_unit);
    DASH_LOG_TRACE_VAR("dash::copy_impl", max_copy_elem);
    while (num_elem_copied < num_elem_total) {
      // Global iterator pointing at begin of current unit's input range:
      auto cur_in_first    = g_in_first + num_elem_copied;
      // unit and local index of first element in current range segment:
      auto local_pos       = pattern.local(static_cast<index_type>(
                                             cur_in_first.pos()));
      // Unit id owning current segment:
      auto cur_unit        = local_pos.unit;
      // Local offset of first element in input range at current unit:
      auto l_in_first_idx  = local_pos.index;
      // Maximum number of elements to copy from current unit:
      auto num_unit_elem   = max_elem_per_unit - l_in_first_idx;
      // Number of elements left to copy:
      int  total_elem_left = num_elem_total - num_elem_copied;
      // Number of elements to copy in this iteration.
      int  num_copy_elem   = (num_unit_elem <
                                static_cast<size_type>(max_copy_elem))
                             ? num_unit_elem
                             : max_copy_elem;
      if (num_copy_elem > total_elem_left) {
        num_copy_elem = total_elem_left;
      }
      DASH_LOG_TRACE("dash::copy_impl",
                     "start g_idx:",    cur_in_first.pos(),
                     "->",
                     "unit:",           cur_unit,
                     "l_idx:",          l_in_first_idx,
                     "->",
                     "unit elements:",  num_unit_elem,
                     "get elements:",   num_copy_elem);
      DASH_LOG_TRACE("dash::copy_impl",
                     "total:",          num_elem_total,
                     "copied:",         num_elem_copied,
                     "left:",           total_elem_left);
      auto src_gptr = cur_in_first.dart_gptr();
      auto dest_ptr = out_first + num_elem_copied;
#ifdef DASH__ALGORITHM__COPY__USE_FLUSH
      if (dart_get(
            dest_ptr,
            src_gptr,
            num_copy_elem * sizeof(ValueType))
          != DART_OK) {
        DASH_LOG_ERROR("dash::copy_impl", "dart_get failed");
        DASH_THROW(
          dash::exception::RuntimeError, "dart_get failed");
      }
      flush_glob_ptrs.push_back(src_gptr);
#else
      dart_handle_t get_handle;
      if (dart_get_handle(
            dest_ptr,
            src_gptr,
            num_copy_elem * sizeof(ValueType),
            &get_handle)
          != DART_OK) {
        DASH_LOG_ERROR("dash::copy_impl", "dart_get_handle failed");
        DASH_THROW(
          dash::exception::RuntimeError, "dart_get_handle failed");
      }
      flush_glob_ptrs.push_back(get_handle);
#endif
      num_elem_copied += num_copy_elem;
    }
  }
  dash::Future<ValueType *> result([=]() mutable {
    // Wait for all get requests to complete:
    ValueType * _out = out_first + num_elem_copied;
    DASH_LOG_TRACE("dash::copy_impl [Future]",
                   "wait for", flush_glob_ptrs.size(), "async get request");
    DASH_LOG_TRACE("dash::copy_impl [Future]", "flush:", flush_glob_ptrs);
    DASH_LOG_TRACE("dash::copy_impl [Future]", "_out:", _out);
#ifdef DASH__ALGORITHM__COPY__USE_FLUSH
    for (auto gptr : flush_glob_ptrs) {
      dart_flush(gptr);
    }
#else
    dart_waitall(&flush_glob_ptrs[0], flush_glob_ptrs.size());
#endif
    DASH_LOG_TRACE("dash::copy_impl [Future]", "async requests completed");
    DASH_LOG_TRACE("dash::copy_impl [Future]", "> _out:", _out);
    return _out;
  });
  DASH_LOG_TRACE("dash::copy_impl >", "returning future");
  return result;
}

/**
 * Blocking implementation of \c dash::copy (local to global) without
 * optimization for local subrange.
 */
template <
  typename ValueType,
  class GlobOutputIt >
GlobOutputIt copy_impl(
  ValueType    * in_first,
  ValueType    * in_last,
  GlobOutputIt   out_first)
{
  auto num_elements = std::distance(in_first, in_last);
  auto num_bytes    = num_elements * sizeof(ValueType);
  DASH_ASSERT_RETURNS(
    dart_put_blocking(
      out_first.dart_gptr(),
      in_first,
      num_bytes),
    DART_OK);
  return out_first + num_elements;
}

} // namespace internal

/**
 * Specialization of \c dash::copy as global-to-local blocking copy operation.
 */
template <
  typename ValueType,
  class GlobInputIt >
dash::Future<ValueType *> copy_async(
  GlobInputIt   in_first,
  GlobInputIt   in_last,
  ValueType   * out_first)
{
  DASH_LOG_TRACE("dash::copy_async()", "async, global to local");
  if (in_first == in_last) {
    DASH_LOG_TRACE("dash::copy_async", "input range empty");
    return dash::Future<ValueType *>([=]() { return out_first; });
  }
  ValueType * dest_first = out_first;
  // Return value, initialize with begin of output range, indicating no values
  // have been copied:
  ValueType * out_last = out_first;
  // Check if part of the input range is local:
  DASH_LOG_TRACE_VAR("dash::copy_async()", in_first.dart_gptr());
  DASH_LOG_TRACE_VAR("dash::copy_async()", in_last.dart_gptr());
  DASH_LOG_TRACE_VAR("dash::copy_async()", out_first);
  auto li_range_in     = local_index_range(in_first, in_last);
  // Number of elements in the local subrange:
  auto num_local_elem  = li_range_in.end - li_range_in.begin;
  // Total number of elements to be copied:
  auto total_copy_elem = in_last - in_first;
  DASH_LOG_TRACE("dash::copy_async", "local range:",
                 li_range_in.begin,
                 li_range_in.end,
                 "in_first.is_local:", in_first.is_local());
  // Futures of asynchronous get requests:
  auto futures = std::vector< dash::Future<ValueType *> >();
  // Check if global input range is partially local:
  if (num_local_elem > 0) {
    // Part of the input range is local, copy local input subrange to local
    // output range directly.
    auto pattern          = in_first.pattern();
    // Map input iterators to global index domain:
    auto g_in_first       = in_first.global();
    auto g_in_last        = g_in_first + total_copy_elem;
    DASH_LOG_TRACE("dash::copy_async", "resolving local subrange");
    DASH_LOG_TRACE_VAR("dash::copy_async", num_local_elem);
    // Local index range to global input index range:
    // Global index of local range begin index:
    auto g_l_offset_begin = pattern.global(li_range_in.begin);
    // Global index of local range end index:
    auto g_l_offset_end   = pattern.global(li_range_in.end-1)
                            + 1; // pat.global(l_end) would be out of range
    DASH_LOG_TRACE("dash::copy_async",
                   "global index range of local subrange:",
                   "begin:", g_l_offset_begin, "end:", g_l_offset_end);
    // Global position of input start iterator:
    auto g_offset_begin   = g_in_first.pos();
    // Convert local subrange to global iterators:
    auto g_l_in_first     = g_in_first + (g_l_offset_begin - g_offset_begin);
    auto g_l_in_last      = g_in_first + (g_l_offset_end   - g_offset_begin);
    DASH_LOG_TRACE("dash::copy_async", "global it. range of local subrange:",
                   "begin:", g_l_in_first.pos(), "end:", g_l_in_last.pos());
    DASH_LOG_TRACE_VAR("dash::copy_async", g_l_in_last.pos());
    //
    // -----------------------------------------------------------------------
    // Copy remote elements preceding the local subrange:
    //
    auto num_prelocal_elem = g_l_in_first.pos() - g_in_first.pos();
    DASH_LOG_TRACE_VAR("dash::copy_async", num_prelocal_elem);
    if (num_prelocal_elem > 0) {
      DASH_LOG_TRACE("dash::copy_async",
                     "copy global range preceding local subrange",
                     "g_in_first:", g_in_first.pos(),
                     "g_in_last:",  g_l_in_first.pos());
      // ... [ --- copy --- | ... l ... | ........ ]
      //     ^              ^           ^          ^
      //     in_first       l_in_first  l_in_last  in_last
      auto fut_prelocal = dash::internal::copy_impl(g_in_first,
                                                    g_l_in_first,
                                                    dest_first);
      futures.push_back(fut_prelocal);
      // Advance output pointers:
      out_last   = dest_first + num_prelocal_elem;
      dest_first = out_last;
    }
    //
    // -----------------------------------------------------------------------
    // Copy local subrange:
    //
    // Convert local subrange of global input to native pointers:
    //
    // ... [ ........ | --- l --- | ........ ]
    //     ^          ^           ^          ^
    //     in_first   l_in_first  l_in_last  in_last
    //
    ValueType * l_in_first = g_l_in_first.local();
    ValueType * l_in_last  = l_in_first + num_local_elem;
    DASH_LOG_TRACE_VAR("dash::copy_async", l_in_first);
    DASH_LOG_TRACE_VAR("dash::copy_async", l_in_last);
    // Verify conversion of global input iterators to local pointers:
    DASH_ASSERT_MSG(l_in_first != nullptr,
                    "dash::copy_async: first index in global input (" <<
                    g_l_in_first.pos() << ") is not local");

    size_t num_copy_elem = l_in_last - l_in_first;
    DASH_LOG_TRACE("dash::copy_async", "copy local subrange",
                   "num_copy_elem:", num_copy_elem);
    out_last  = std::copy(l_in_first,
                          l_in_last,
                          dest_first);
    // Assert that all elements in local range have been copied:
    DASH_ASSERT_EQ(out_last, dest_first + num_local_elem,
                   "Expected to copy " << num_local_elem << " local elements "
                   "but copied " << (out_last - dest_first));
    DASH_LOG_TRACE("dash::copy_async", "finished local copy of",
                   num_copy_elem, "elements");
    // Advance output pointers:
    dest_first = out_last;
    //
    // -----------------------------------------------------------------------
    // Copy remote elements succeeding the local subrange:
    //
    auto num_postlocal_elem = in_last.pos() - g_l_offset_end;
    DASH_LOG_TRACE_VAR("dash::copy_async", num_postlocal_elem);
    if (num_postlocal_elem > 0) {
      DASH_LOG_TRACE("dash::copy_async",
                     "copy global range succeeding local subrange",
                     "in_first:", g_l_in_last.pos(),
                     "in_last:",  g_in_last.pos());
      // ... [ ........ | ... l ... | --- copy --- ]
      //     ^          ^           ^              ^
      //     in_first   l_in_first  l_in_last      in_last
      auto fut_postlocal = dash::internal::copy_impl(g_l_in_last,
                                                     g_in_last,
                                                     dest_first);
      futures.push_back(fut_postlocal);
      out_last = dest_first + num_postlocal_elem;
    }
  } else {
    DASH_LOG_TRACE("dash::copy_async", "no local subrange");
    // All elements in input range are remote
    auto fut_all = dash::internal::copy_impl(in_first,
                                             in_last,
                                             dest_first);
    futures.push_back(fut_all);
    out_last = out_first + total_copy_elem;
  }
  DASH_LOG_TRACE("dash::copy_async", "preparing future");
  dash::Future<ValueType *> fut_result([=]() mutable {
    ValueType * _out = out_last;
    DASH_LOG_TRACE("dash::copy_async [Future]",
                   "wait for", futures.size(), "async requests");
    DASH_LOG_TRACE("dash::copy_async [Future]", "futures:", futures);
    DASH_LOG_TRACE("dash::copy_async [Future]", "_out:", _out);
    for (auto f : futures) {
      f.wait();
    }
    DASH_LOG_TRACE("dash::copy_async [Future]", "async requests completed");
    DASH_LOG_TRACE("dash::copy_async [Future]", "> futures:", futures);
    DASH_LOG_TRACE("dash::copy_async [Future]", "> _out:", _out);
    return _out;
  });
  DASH_LOG_TRACE("dash::copy_async >", "finished,",
                 "expected out_last:", out_last);
  return fut_result;
}

/**
 * Specialization of \c dash::copy as global-to-local asynchronous copy
 * operation.
 */
template <
  typename ValueType,
  class GlobInputIt >
ValueType * copy(
  GlobInputIt   in_first,
  GlobInputIt   in_last,
  ValueType   * out_first)
{
  DASH_LOG_TRACE("dash::copy()", "blocking, global to local");
  auto future = dash::copy_async(in_first, in_last, out_first);
  DASH_LOG_TRACE("dash::copy()", "waiting for asynchronous requests");
  ValueType * out_last = future.get();
  DASH_LOG_TRACE("dash::copy >", "finished");
  return out_last;
}

/**
 * Specialization of \c dash::copy as local-to-global blocking copy operation.
 */
template <
  typename ValueType,
  class GlobOutputIt >
GlobOutputIt copy(
  ValueType    * in_first,
  ValueType    * in_last,
  GlobOutputIt   out_first)
{
  DASH_LOG_TRACE("dash::copy()", "blocking, local to global");
  // Return value, initialize with begin of output range, indicating no values
  // have been copied:
  GlobOutputIt out_last   = out_first;
  // Number of elements to copy in total:
  auto num_elements       = std::distance(in_first, in_last);
  DASH_LOG_TRACE_VAR("dash::copy", num_elements);
  // Global iterator pointing at hypothetical end of output range:
  GlobOutputIt out_h_last = out_first + num_elements;
  DASH_LOG_TRACE_VAR("dash::copy", out_first.pos());
  DASH_LOG_TRACE_VAR("dash::copy", out_h_last.pos());
  // Test if a subrange of global output range is local:
  auto li_range_out       = local_index_range(out_first, out_h_last);
  DASH_LOG_TRACE_VAR("dash::copy", li_range_out.begin);
  DASH_LOG_TRACE_VAR("dash::copy", li_range_out.end);
  // Number of elements in the local subrange:
  auto num_local_elem       = li_range_out.end - li_range_out.begin;
  // Check if part of the output range is local:
  if (num_local_elem > 0) {
    // Part of the output range is local
    // Copy local input subrange to local output range directly:
    auto pattern            = out_first.pattern();
    DASH_LOG_TRACE("dash::copy", "resolving local subrange");
    DASH_LOG_TRACE_VAR("dash::copy", num_local_elem);
    // Local index range to global output index range:
    auto g_l_offset_begin   = pattern.global(li_range_out.begin);
    DASH_LOG_TRACE_VAR("dash::copy", g_l_offset_begin);
    auto g_l_offset_end     = pattern.global(li_range_out.end-1)
                              + 1; // pat.global(l_end) would be out of range
    DASH_LOG_TRACE_VAR("dash::copy", g_l_offset_end);
    // Offset of local subrange in output range
    auto l_elem_offset      = g_l_offset_begin - out_first.pos();
    DASH_LOG_TRACE_VAR("dash::copy",l_elem_offset);
    // Convert local subrange of global output to native pointers:
    ValueType * l_out_first = (out_first + l_elem_offset).local();
    DASH_LOG_TRACE_VAR("dash::copy", l_out_first);
    ValueType * l_out_last  = l_out_first + num_local_elem;
    DASH_LOG_TRACE_VAR("dash::copy", l_out_last);
    // ... [ ........ | ---- l ---- | ......... ] ...
    //     ^          ^             ^           ^
    //     out_first  l_out_first   l_out_last  out_last
    out_last                = out_first + num_local_elem;
    // Assert that all elements in local range have been copied:
    DASH_LOG_TRACE("dash::copy", "copying local subrange");
    DASH_LOG_TRACE_VAR("dash::copy", in_first);
    DASH_ASSERT(
      std::copy(in_first + l_elem_offset,
                in_first + l_elem_offset + num_local_elem,
                l_out_first)
      == l_out_last);
    // Copy to remote elements preceding the local subrange:
    if (g_l_offset_begin > out_first.pos()) {
      DASH_LOG_TRACE("dash::copy", "copy to global preceding local subrange");
      out_last = dash::internal::copy_impl(
                   in_first,
                   in_first + l_elem_offset,
                   out_first);
    }
    // Copy to remote elements succeeding the local subrange:
    if (g_l_offset_end < out_h_last.pos()) {
      DASH_LOG_TRACE("dash::copy", "copy to global succeeding local subrange");
      out_last = dash::internal::copy_impl(
                   in_first + l_elem_offset + num_local_elem,
                   in_last,
                   out_first + num_local_elem);
    }
  } else {
    // All elements in output range are remote
    DASH_LOG_TRACE("dash::copy", "no local subrange");
    out_last = dash::internal::copy_impl(
                 in_first,
                 in_last,
                 out_first);
  }
  return out_last;
}

/**
 * Specialization of \c dash::copy as global-to-global blocking copy
 * operation.
 */
template <
  typename ValueType,
  class GlobInputIt,
  class GlobOutputIt >
ValueType * copy(
  GlobInputIt   in_first,
  GlobInputIt   in_last,
  GlobOutputIt  out_first)
{
  DASH_LOG_TRACE("dash::copy()", "blocking, global to global");

  // TODO:
  // - Implement adapter for local-to-global dash::copy here
  // - Return if global input range has no local sub-range
}

} // namespace dash

#endif // DASH__ALGORITHM__COPY_H__
