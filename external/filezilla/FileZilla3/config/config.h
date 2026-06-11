/* config/config.h.  Generated from config.h.in by configure.  */
/* config/config.h.in.  Generated from configure.ac by autoheader.  */


#ifndef FILEZILLA_CONFIG_HEADER
#define FILEZILLA_CONFIG_HEADER


/* Buildtype, indicates official builds and nightly builds */
/* #undef BUILDTYPE */

/* Define if building with FTP(S) support. */
#define ENABLE_FTP 1

/* Define if building with SFTP support. */
#define ENABLE_SFTP 1

/* Define if building with Storj support. */
/* #undef ENABLE_STORJ */

/* Engine version number */
#define ENGINE_VERSION "1.2.2"

/* Shortened engine version number */
#define ENGINE_VERSION_SHORT "1.2.2"

/* Set to 1 to add support for automated update checks */
#define FZ_AUTOUPDATECHECK 0

/* Set to 1 to enable user initiated update checks */
#define FZ_MANUALUPDATECHECK 0

/* clock_gettime can be used */
#define HAVE_CLOCK_GETTIME 1

/* define if the compiler supports basic C++17 syntax */
#define HAVE_CXX17 1

/* Define to 1 if you have the declaration of 'CLOCK_MONOTONIC', and to 0 if
   you don't. */
#define HAVE_DECL_CLOCK_MONOTONIC 1

/* Define to 1 if you have the <dlfcn.h> header file. */
#define HAVE_DLFCN_H 1

/* Define to 1 if you have the 'ftime' function. */
#define HAVE_FTIME 1

/* Define to 1 if you have the 'getaddrinfo' function. */
#define HAVE_GETADDRINFO 1

/* Define to 1 if you have the 'gettimeofday' function. */
#define HAVE_GETTIMEOFDAY 1

/* Define to 1 if you have the 'gmtime_r' function. */
#define HAVE_GMTIME_R 1

/* gmtime_s can be used */
/* #undef HAVE_GMTIME_S */

/* Headers delare ICopyHookW */
/* #undef HAVE_ICOPYHOOKW */

/* Define to 1 if you have the 'in6addr_any' function. */
#define HAVE_IN6ADDR_ANY 1

/* Define to 1 if you have the 'in6addr_loopback' function. */
#define HAVE_IN6ADDR_LOOPBACK 1

/* Define to 1 if you have the <inttypes.h> header file. */
#define HAVE_INTTYPES_H 1

/* Define to 1 if your system has the `pugixml' library (-lpugixml). */
#define HAVE_LIBPUGIXML 1

/* Define to 1 if you have the 'localtime_r' function. */
#define HAVE_LOCALTIME_R 1

/* localtime_s can be used */
/* #undef HAVE_LOCALTIME_S */

/* Define to 1 if you have the 'posix_fadvise' function. */
#define HAVE_POSIX_FADVISE 1

/* Define to 1 if you have the 'ptsname' function. */
#define HAVE_PTSNAME 1

/* Define to 1 if putenv function is available. */
#define HAVE_PUTENV 1

/* Define to 1 if setenv function is available. */
#define HAVE_SETENV 1

/* Define to 1 if you have the 'setresuid' function. */
#define HAVE_SETRESUID 1

/* Define if SO_PEERCRED works in the Linux fashion. */
#define HAVE_SO_PEERCRED 1

/* Define to 1 if you have the <stdint.h> header file. */
#define HAVE_STDINT_H 1

/* Define to 1 if you have the <stdio.h> header file. */
#define HAVE_STDIO_H 1

/* Define to 1 if you have the <stdlib.h> header file. */
#define HAVE_STDLIB_H 1

/* Define to 1 if you have the <strings.h> header file. */
#define HAVE_STRINGS_H 1

/* Define to 1 if you have the <string.h> header file. */
#define HAVE_STRING_H 1

/* Define to 1 if you have the 'strsignal' function. */
#define HAVE_STRSIGNAL 1

/* Define to 1 if you have the <sys/select.h> header file. */
#define HAVE_SYS_SELECT_H 1

/* Define to 1 if you have the <sys/stat.h> header file. */
#define HAVE_SYS_STAT_H 1

/* Define to 1 if you have the <sys/types.h> header file. */
#define HAVE_SYS_TYPES_H 1

/* Define to 1 if you have the 'timegm' function. */
#define HAVE_TIMEGM 1

/* Define to 1 if you have the <unistd.h> header file. */
#define HAVE_UNISTD_H 1

/* Define to 1 if you have the 'updwtmpx' function. */
#define HAVE_UPDWTMPX 1

/* Define to 1 if you have the <utmpx.h> header file. */
#define HAVE_UTMPX_H 1

/* Define to the sub-directory where libtool stores uninstalled libraries. */
#define LT_OBJDIR ".libs/"

/* Name of package */
#define PACKAGE "parvion"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "tim.kosse@filezilla-project.org"

/* Define to the full name of this package. */
#define PACKAGE_NAME "Parvion"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "Parvion 1.2.2"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "parvion"

/* Define to the home page for this package. */
#define PACKAGE_URL "https://filezilla-project.org/"

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.2.2"

/* Shortened package version number */
#define PACKAGE_VERSION_SHORT "1.2.2"

/* Define to 1 if all of the C89 standard headers exist (not just the ones
   required in a freestanding environment). This macro is provided for
   backward compatibility; new code need not use it. */
#define STDC_HEADERS 1

/* Build system under which the program was compiled on. */
#define USED_BUILD "x86_64-pc-linux-gnu"

/* Define to name and version of used compiler */
#define USED_COMPILER "gcc (Debian 14.2.0-19) 14.2.0"

/* Define to the used CXXFLAGS to compile this package. */
#define USED_CXXFLAGS "-g -O2 -Wall"

/* Host system under which the program will run. */
#define USED_HOST "x86_64-pc-linux-gnu"

/* GCC bug 86164 requires use of Boost Regex insteadd */
#define USE_BOOST_REGEX 1

/* Define to 1 if the App Sandbox on OS X should be used. */
/* #undef USE_MAC_SANDBOX */

/* Version number of package */
#define VERSION "1.2.2"

/* Set to 2 to if libdbus >= 1.2 is available, set to 1 if an older version is
   available. */
/* #undef WITH_LIBDBUS */


#endif

