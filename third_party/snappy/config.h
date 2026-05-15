// Hand-written config.h for SlothDB's vendored snappy build.
// Replaces what snappy's CMake would normally generate via configure-time
// feature detection. Targets: MSVC (Windows) + GCC/Clang (Linux/macOS).
//
// We only need decompression to be fast (Parquet read path); compression
// is unused but the same .cc file is compiled in.

#ifndef SLOTHDB_VENDORED_SNAPPY_CONFIG_H_
#define SLOTHDB_VENDORED_SNAPPY_CONFIG_H_

// Builtins available on GCC/Clang. MSVC handled via the _MSC_VER branches in
// snappy-stubs-internal.h.
#if defined(__GNUC__) || defined(__clang__)
#  define HAVE_BUILTIN_EXPECT 1
#  define HAVE_BUILTIN_CTZ 1
#  define HAVE_BUILTIN_PREFETCH 1
#  define HAVE_ATTRIBUTE_ALWAYS_INLINE 1
#endif

// SSSE3 is required for the vector-byte-shuffle fast path in literal copy.
// We compile with /arch:AVX2 (MSVC) or -mavx2 (gcc/clang) elsewhere; AVX2
// implies SSSE3, so enable it for snappy.cc.
#if defined(_MSC_VER)
#  if defined(__AVX2__) && !defined(SNAPPY_HAVE_SSSE3)
#    define SNAPPY_HAVE_SSSE3 1
#  endif
#elif defined(__SSSE3__) && !defined(SNAPPY_HAVE_SSSE3)
#  define SNAPPY_HAVE_SSSE3 1
#endif

// Endianness: SlothDB targets only little-endian (x86_64 + ARM64 LE).
#ifndef SNAPPY_IS_BIG_ENDIAN
#  define SNAPPY_IS_BIG_ENDIAN 0
#endif

// POSIX headers: present on Linux/macOS, absent on MSVC.
#if !defined(_MSC_VER)
#  define HAVE_UNISTD_H 1
#  define HAVE_SYS_MMAN_H 1
#endif

#endif  // SLOTHDB_VENDORED_SNAPPY_CONFIG_H_
