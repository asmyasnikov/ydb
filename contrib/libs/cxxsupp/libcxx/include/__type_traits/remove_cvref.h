//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef _LIBCPP___TYPE_TRAITS_REMOVE_CVREF_H
#define _LIBCPP___TYPE_TRAITS_REMOVE_CVREF_H

#include <__config>
#include <__type_traits/is_same.h>
#include <__type_traits/remove_cv.h>
#include <__type_traits/remove_reference.h>

#if !defined(_LIBCPP_HAS_NO_PRAGMA_SYSTEM_HEADER)
#  pragma GCC system_header
#endif

_LIBCPP_BEGIN_NAMESPACE_STD

#if defined(_LIBCPP_COMPILER_GCC)
template <class _Tp>
struct __remove_cvref_gcc {
  using type = __remove_cvref(_Tp);
};

template <class _Tp>
using __remove_cvref_t _LIBCPP_NODEBUG = typename __remove_cvref_gcc<_Tp>::type;
// compatibility with cuda && clang14
#elif defined(__CUDACC__) || !__has_builtin(__remove_cvref)
template <class _Tp>
using __remove_cvref_t _LIBCPP_NODEBUG = __remove_cv_t<__libcpp_remove_reference_t<_Tp> >;
#else
template <class _Tp>
using __remove_cvref_t _LIBCPP_NODEBUG = __remove_cvref(_Tp);
#endif // __has_builtin(__remove_cvref)

template <class _Tp, class _Up>
using __is_same_uncvref = _IsSame<__remove_cvref_t<_Tp>, __remove_cvref_t<_Up> >;

#if _LIBCPP_STD_VER >= 20
template <class _Tp>
struct remove_cvref {
#if defined(__CUDACC__) || !__has_builtin(__remove_cvref)
  using type _LIBCPP_NODEBUG = __remove_cvref_t<_Tp>;
#else
  using type _LIBCPP_NODEBUG = __remove_cvref(_Tp);
#endif
};

template <class _Tp>
using remove_cvref_t = __remove_cvref_t<_Tp>;
#endif

_LIBCPP_END_NAMESPACE_STD

#endif // _LIBCPP___TYPE_TRAITS_REMOVE_CVREF_H
