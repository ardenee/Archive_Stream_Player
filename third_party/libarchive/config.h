#pragma once

#ifndef LIBARCHIVE_CUSTOM_CONFIG_H
#define LIBARCHIVE_CUSTOM_CONFIG_H

#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS 1
#endif

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif

#ifndef WINVER
#define WINVER _WIN32_WINNT
#endif

#include <stdint.h>
#include <stddef.h>
#include <inttypes.h>
#include <sys/types.h>

#ifndef PRIu32
#define PRIu32 "u"
#endif

#ifndef PRIx32
#define PRIx32 "x"
#endif

#ifndef PRIu64
#define PRIu64 "llu"
#endif

#ifndef PRIx64
#define PRIx64 "llx"
#endif

#ifndef PRId64
#define PRId64 "lld"
#endif

typedef intptr_t ssize_t;
typedef int pid_t;
typedef int uid_t;
typedef int gid_t;

#ifndef _MODE_T_DEFINED
typedef unsigned short mode_t;
#define _MODE_T_DEFINED
#endif

#define HAVE_CONFIG_H 1
#define __LIBARCHIVE_CONFIG_H_INCLUDED 1

/*
    Package/version info.
*/
#define VERSION "3.8.7"
#define PACKAGE "libarchive"
#define PACKAGE_NAME "libarchive"
#define PACKAGE_VERSION "3.8.7"
#define PACKAGE_STRING "libarchive 3.8.7"

/*
    Basic type sizes for Win32.
*/
#define SIZEOF_SHORT 2
#define SIZEOF_INT 4
#define SIZEOF_LONG 4
#define SIZEOF_LONG_LONG 8
#define SIZEOF_WCHAR_T 2

#define STDC_HEADERS 1

/*
    Declaration availability.
*/
#define HAVE_DECL_INT64_MAX 1
#define HAVE_DECL_INT64_MIN 1
#define HAVE_DECL_SIZE_MAX 1
#define HAVE_DECL_UINT32_MAX 1
#define HAVE_DECL_UINT64_MAX 1
#define HAVE_DECL_UINTMAX_MAX 1
#define HAVE_DECL_SSIZE_MAX 0
#define HAVE_DECL_STRERROR_R 0
#define HAVE_DECL_EXTATTR_NAMESPACE_USER 0
#define HAVE_DECL_EXTATTR_NAMESPACE_SYSTEM 0

/*
    Headers available on MSVC/Windows.
*/
#define HAVE_ERRNO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_IO_H 1
#define HAVE_LIMITS_H 1
#define HAVE_LOCALE_H 1
#define HAVE_MEMORY_H 1
#define HAVE_STDARG_H 1
#define HAVE_STDDEF_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UTIME_H 1
#define HAVE_TIME_H 1
#define HAVE_UTIME_H 1
#define HAVE_WCHAR_H 1
#define HAVE_WINDOWS_H 1
#define HAVE_WINCRYPT_H 1

/*
    Headers not available on normal MSVC/Windows builds.

    These must be undefined, not defined as 0, because parts of libarchive use
    #ifdef / #if defined(...) checks for headers.
*/
#undef HAVE_DIRENT_H
#undef HAVE_DLFCN_H
#undef HAVE_GRP_H
#undef HAVE_LANGINFO_H
#undef HAVE_LINUX_FS_H
#undef HAVE_LINUX_MAGIC_H
#undef HAVE_PATHS_H
#undef HAVE_POLL_H
#undef HAVE_PWD_H
#undef HAVE_REGEX_H
#undef HAVE_STRINGS_H
#undef HAVE_SYS_ACL_H
#undef HAVE_SYS_CDEFS_H
#undef HAVE_SYS_EXTATTR_H
#undef HAVE_SYS_IOCTL_H
#undef HAVE_SYS_MKDEV_H
#undef HAVE_SYS_MOUNT_H
#undef HAVE_SYS_PARAM_H
#undef HAVE_SYS_POLL_H
#undef HAVE_SYS_RICHACL_H
#undef HAVE_SYS_SELECT_H
#undef HAVE_SYS_STATFS_H
#undef HAVE_SYS_STATVFS_H
#undef HAVE_SYS_TIME_H
#undef HAVE_SYS_WAIT_H
#undef HAVE_UNISTD_H

/*
    MSVC/Windows functions available.
*/
#define HAVE__CHSIZE_S 1
#define HAVE__FSEEKI64 1
#define HAVE__FTELLI64 1
#define HAVE__GET_TIMEZONE 1
#define HAVE__LOCALTIME64_S 1
#define HAVE__MKGMTIME64 1
#define HAVE__STAT64 1
#define HAVE__STRDUP 1
#define HAVE__TIME64 1
#define HAVE__UTIME64 1
#define HAVE__WSTAT64 1

#define HAVE_MEMMOVE 1
#define HAVE_MKDIR 1
#define HAVE_SETLOCALE 1
#define HAVE_STRCHR 1
#define HAVE_STRDUP 1
#define HAVE_STRERROR 1
#define HAVE_STRFTIME 1
#define HAVE_WCRTOMB 1
#define HAVE_WCSCMP 1
#define HAVE_WCSCPY 1
#define HAVE_WCSLEN 1
#define HAVE_WCTOMB 1

/*
    POSIX/Unix functions not available in this build.
*/
#define HAVE_CREATEFILE2 0
#define HAVE_CYGWIN_CONV_PATH 0
#define HAVE_EFTYPE 0
#define HAVE_FCHDIR 0
#define HAVE_FCHMOD 0
#define HAVE_FCHOWN 0
#define HAVE_FCNTL 0
#define HAVE_FDOPENDIR 0
#define HAVE_FSEEKO 0
#define HAVE_FSTAT 0
#define HAVE_FSTATAT 0
#define HAVE_FSTATFS 0
#define HAVE_FSTATVFS 0
#define HAVE_FTRUNCATE 0
#define HAVE_FUTIMES 0
#define HAVE_GETEUID 0
#define HAVE_GETGRGID_R 0
#define HAVE_GETGRNAM_R 0
#define HAVE_GETPID 0
#define HAVE_GETPWNAM_R 0
#define HAVE_GETPWUID_R 0
#define HAVE_GETVFSBYNAME 0
#undef HAVE_GMTIME_R
#define HAVE_LCHFLAGS 0
#define HAVE_LCHMOD 0
#define HAVE_LCHOWN 0
#undef HAVE_LOCALTIME_R
#define HAVE_LSTAT 0
#define HAVE_LUTIMES 0
#define HAVE_MBR_GID_TO_UUID 0
#define HAVE_MBR_UID_TO_UUID 0
#define HAVE_MKFIFO 0
#define HAVE_MKNOD 0
#define HAVE_NL_LANGINFO 0
#define HAVE_POLL 0
#define HAVE_READLINK 0
#define HAVE_READPASSPHRASE 0
#define HAVE_SELECT 0
#define HAVE_SETENV 0
#define HAVE_SIGACTION 0
#define HAVE_STATFS 0
#define HAVE_STATVFS 0
#define HAVE_SYMLINK 0
#define HAVE_TIMEGM 0
#define HAVE_UNSETENV 0
#define HAVE_UTIMES 0
#define HAVE_VFORK 0

/*
    Optional compression/filter libraries disabled for the first minimal read-only
    build. Add zlib/other libraries later after libarchive_static builds cleanly.
*/
#undef HAVE_BZLIB_H
#undef HAVE_LZ4_H
#undef HAVE_LZMA_H
#define HAVE_ZLIB_H 1
#undef HAVE_ZSTD_H

#define HAVE_LIBB2 0
#define HAVE_LIBBZIP2 0
#define HAVE_LIBCHARSET 0
#undef HAVE_LIBCRYPTO


#define HAVE_BCRYPT_H 1
//#undef HAVE_BCRYPT_H

/*
    OpenSSL/libcrypto is disabled for the Winamp plugin build.

    These must be undefined, not defined as 0, because libarchive uses
    #ifdef / defined(...) checks in several private headers. Defining them as 0
    still enables the OpenSSL path and causes libcrypto-3.dll to be imported.
*/
#undef HAVE_OPENSSL_EVP_H
#undef HAVE_OPENSSL_HMAC_H
#undef HAVE_OPENSSL_MD5_H
#undef HAVE_OPENSSL_RIPEMD_H
#undef HAVE_OPENSSL_SHA_H
#undef HAVE_OPENSSL_SHA256_INIT
#undef HAVE_OPENSSL_SHA384_INIT
#undef HAVE_OPENSSL_SHA512_INIT
#undef HAVE_OPENSSL_OPENSSLV_H

#define HAVE_LIBEXPAT 0
#define HAVE_LIBLZ4 0
#define HAVE_LIBLZMA 0
#define HAVE_LIBMD 0
#define HAVE_LIBXML2 0
#define HAVE_LIBZ 1
#define HAVE_LIBZSTD 0

/*
    Windows crypto/hash support.
*/
#define ARCHIVE_CRYPTO_MD5_WIN 1
#define ARCHIVE_CRYPTO_RMD160_WIN 1
#define ARCHIVE_CRYPTO_SHA1_WIN 1
#define ARCHIVE_CRYPTO_SHA256_WIN 1
#define ARCHIVE_CRYPTO_SHA384_WIN 1
#define ARCHIVE_CRYPTO_SHA512_WIN 1

#define ARCHIVE_HASH_MD5_WIN 1
#define ARCHIVE_HASH_RMD160_WIN 1
#define ARCHIVE_HASH_SHA1_WIN 1
#define ARCHIVE_HASH_SHA256_WIN 1
#define ARCHIVE_HASH_SHA384_WIN 1
#define ARCHIVE_HASH_SHA512_WIN 1

/*
    Non-Windows crypto/hash backends disabled.

    These must be undefined, not defined as 0. Several libarchive private headers
    check these with #if defined(...), so defining them as 0 still enables their
    include paths, such as md5.h or sha1.h.
*/
#undef ARCHIVE_CRYPTO_MD5_LIBC
#undef ARCHIVE_CRYPTO_MD5_LIBSYSTEM
#undef ARCHIVE_CRYPTO_MD5_MBEDTLS
#undef ARCHIVE_CRYPTO_MD5_NETTLE
#undef ARCHIVE_CRYPTO_MD5_OPENSSL

#undef ARCHIVE_CRYPTO_RMD160_LIBC
#undef ARCHIVE_CRYPTO_RMD160_MBEDTLS
#undef ARCHIVE_CRYPTO_RMD160_NETTLE
#undef ARCHIVE_CRYPTO_RMD160_OPENSSL

#undef ARCHIVE_CRYPTO_SHA1_LIBC
#undef ARCHIVE_CRYPTO_SHA1_LIBSYSTEM
#undef ARCHIVE_CRYPTO_SHA1_MBEDTLS
#undef ARCHIVE_CRYPTO_SHA1_NETTLE
#undef ARCHIVE_CRYPTO_SHA1_OPENSSL

#undef ARCHIVE_CRYPTO_SHA256_LIBC
#undef ARCHIVE_CRYPTO_SHA256_LIBC2
#undef ARCHIVE_CRYPTO_SHA256_LIBC3
#undef ARCHIVE_CRYPTO_SHA256_LIBSYSTEM
#undef ARCHIVE_CRYPTO_SHA256_MBEDTLS
#undef ARCHIVE_CRYPTO_SHA256_NETTLE
#undef ARCHIVE_CRYPTO_SHA256_OPENSSL

#undef ARCHIVE_CRYPTO_SHA384_LIBC
#undef ARCHIVE_CRYPTO_SHA384_LIBC2
#undef ARCHIVE_CRYPTO_SHA384_LIBC3
#undef ARCHIVE_CRYPTO_SHA384_LIBSYSTEM
#undef ARCHIVE_CRYPTO_SHA384_MBEDTLS
#undef ARCHIVE_CRYPTO_SHA384_NETTLE
#undef ARCHIVE_CRYPTO_SHA384_OPENSSL

#undef ARCHIVE_CRYPTO_SHA512_LIBC
#undef ARCHIVE_CRYPTO_SHA512_LIBC2
#undef ARCHIVE_CRYPTO_SHA512_LIBC3
#undef ARCHIVE_CRYPTO_SHA512_LIBSYSTEM
#undef ARCHIVE_CRYPTO_SHA512_MBEDTLS
#undef ARCHIVE_CRYPTO_SHA512_NETTLE
#undef ARCHIVE_CRYPTO_SHA512_OPENSSL

#undef ARCHIVE_HASH_MD5_LIBC
#undef ARCHIVE_HASH_MD5_LIBSYSTEM
#undef ARCHIVE_HASH_MD5_MBEDTLS
#undef ARCHIVE_HASH_MD5_NETTLE
#undef ARCHIVE_HASH_MD5_OPENSSL

#undef ARCHIVE_HASH_RMD160_LIBC
#undef ARCHIVE_HASH_RMD160_MBEDTLS
#undef ARCHIVE_HASH_RMD160_NETTLE
#undef ARCHIVE_HASH_RMD160_OPENSSL

#undef ARCHIVE_HASH_SHA1_LIBC
#undef ARCHIVE_HASH_SHA1_LIBSYSTEM
#undef ARCHIVE_HASH_SHA1_MBEDTLS
#undef ARCHIVE_HASH_SHA1_NETTLE
#undef ARCHIVE_HASH_SHA1_OPENSSL

#undef ARCHIVE_HASH_SHA256_LIBC
#undef ARCHIVE_HASH_SHA256_LIBC2
#undef ARCHIVE_HASH_SHA256_LIBC3
#undef ARCHIVE_HASH_SHA256_LIBSYSTEM
#undef ARCHIVE_HASH_SHA256_MBEDTLS
#undef ARCHIVE_HASH_SHA256_NETTLE
#undef ARCHIVE_HASH_SHA256_OPENSSL

#undef ARCHIVE_HASH_SHA384_LIBC
#undef ARCHIVE_HASH_SHA384_LIBC2
#undef ARCHIVE_HASH_SHA384_LIBC3
#undef ARCHIVE_HASH_SHA384_LIBSYSTEM
#undef ARCHIVE_HASH_SHA384_MBEDTLS
#undef ARCHIVE_HASH_SHA384_NETTLE
#undef ARCHIVE_HASH_SHA384_OPENSSL

#undef ARCHIVE_HASH_SHA512_LIBC
#undef ARCHIVE_HASH_SHA512_LIBC2
#undef ARCHIVE_HASH_SHA512_LIBC3
#undef ARCHIVE_HASH_SHA512_LIBSYSTEM
#undef ARCHIVE_HASH_SHA512_MBEDTLS
#undef ARCHIVE_HASH_SHA512_NETTLE
#undef ARCHIVE_HASH_SHA512_OPENSSL

#endif