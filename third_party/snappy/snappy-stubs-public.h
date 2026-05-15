// Generated from snappy-stubs-public.h.in for SlothDB vendored build.
// HAVE_SYS_UIO_H = 0 (Windows + MSVC). On POSIX we still don't need iovec for
// our raw-buffer decompress path, so the stub iovec is defined unconditionally
// inside namespace snappy.
//
// Version: 1.2.1

#ifndef THIRD_PARTY_SNAPPY_OPENSOURCE_SNAPPY_STUBS_PUBLIC_H_
#define THIRD_PARTY_SNAPPY_OPENSOURCE_SNAPPY_STUBS_PUBLIC_H_

#include <cstddef>

#if defined(__has_include)
#  if __has_include(<sys/uio.h>)
#    include <sys/uio.h>
#    define SLOTHDB_SNAPPY_HAVE_SYS_UIO_H 1
#  endif
#endif

#define SNAPPY_MAJOR 1
#define SNAPPY_MINOR 2
#define SNAPPY_PATCHLEVEL 1
#define SNAPPY_VERSION \
    ((SNAPPY_MAJOR << 16) | (SNAPPY_MINOR << 8) | SNAPPY_PATCHLEVEL)

namespace snappy {

#ifndef SLOTHDB_SNAPPY_HAVE_SYS_UIO_H
// Windows does not have an iovec type, yet the concept is universally useful.
// It is simple to define it ourselves, so we put it inside our own namespace.
struct iovec {
  void* iov_base;
  size_t iov_len;
};
#endif  // !HAVE_SYS_UIO_H

}  // namespace snappy

#endif  // THIRD_PARTY_SNAPPY_OPENSOURCE_SNAPPY_STUBS_PUBLIC_H_
