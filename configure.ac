#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.68])
AC_INIT([obe], [1.5], [stoth@kernellabs.com])
AM_INIT_AUTOMAKE
AC_CONFIG_MACRO_DIR([m4])
AC_CANONICAL_HOST
AM_SILENT_RULES([yes])

# Checks for programs.
AC_CHECK_PROGS(YASM, yasm)
AC_PROG_CC
AC_PROG_CC_C99
AC_PROG_CXX
AM_PROG_AS
AX_PROG_YASM()
#CCAS=yasm
#CCASFLAGS=

#AC_SUBST(CCAS)
#AC_SUBST(CCASFLAGS)


# We always build with -fPIC in case our static libraries end up
# being linked into a consumer's shared library
AC_MSG_CHECKING(whether fPIC compiler option is accepted)
SAVED_CFLAGS="$CFLAGS -DKL_USERSPACE"
CFLAGS="$CFLAGS -fPIC -Werror"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([], [return 0;])],
    [AC_MSG_RESULT(yes)
     CFLAGS="$SAVED_CFLAGS -fPIC"],
    [AC_MSG_RESULT(no)
     CFLAGS="$SAVED_CFLAGS"])

AC_PROG_RANLIB
AC_PROG_LIBTOOL

# Checks for header files.
AC_CHECK_HEADERS([arpa/inet.h fcntl.h netinet/in.h stdint.h stdlib.h string.h sys/ioctl.h sys/socket.h sys/time.h unistd.h])
AC_CHECK_HEADERS([alsa/asoundlib.h],
                 [break],
                 [AC_MSG_ERROR([libasound (alsa) headers not found or not usable])])
AC_CHECK_HEADERS([fdk-aac/aacdecoder_lib.h],
                 [break],
                 [AC_MSG_ERROR([libfdk-aac headers not found or not usable])])
AC_CHECK_HEADERS([libavutil/avutil.h],
                 [break],
                 [AC_MSG_ERROR([libavutil headers not found or not usable])])
AC_CHECK_HEADERS([libyuv.h],
                 [break],
                 [AC_MSG_ERROR([libyuv headers not found or not usable])])
AC_CHECK_HEADERS([libklvanc/vanc.h],
                 [break],
                 [AC_MSG_ERROR([libklvanc headers not found or not usable])])
AC_CHECK_HEADERS([libklscte35/scte35.h],
                 [break],
                 [AC_MSG_ERROR([libklscte35 headers not found or not usable])])
AC_CHECK_HEADERS([x264.h],
                 [break],
                 [AC_MSG_ERROR([libx264 headers not found or not usable])])
AC_CHECK_HEADERS([readline/readline.h],
                 [break],
                 [AC_MSG_ERROR([libreadline headers not found or not usable])])
AC_CHECK_HEADERS([libmpegts.h],
                 [break],
                 [AC_MSG_ERROR([libmpegts headers not found or not usable])])
AC_CHECK_HEADERS([libzvbi.h],
                 [break],
                 [AC_MSG_ERROR([libzvbi headers not found or not usable])])
AC_CHECK_HEADERS([twolame.h],
                 [break],
                 [AC_MSG_ERROR([libtwolame headers not found or not usable])])

# Check for klmonitoring (optional)
#AC_CHECK_HEADERS([libklmonitoring/klmonitoring.h], HEADER_LIBKLMONITORING="yes")
#if test "x$HEADER_LIBKLMONITORING" == "xyes" ; then
#    LDFLAGS+=" -lklmonitoring "
#fi
#AM_CONDITIONAL(LIBKLMONITORING, test x"$HEADER_LIBKLMONITORING" == x"yes")

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_UINT16_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_CHECK_FUNCS([memset strrchr])

AC_CONFIG_FILES([Makefile obe/Makefile])
AC_OUTPUT

# Add debug support
AC_ARG_ENABLE(debug,
  AS_HELP_STRING(
    [--enable-debug],
    [enable debugging, default: no]),
    [case "${enableval}" in
      yes) debug=true ;;
      no)  debug=false ;;
      *)   AC_MSG_ERROR([bad value ${enableval} for --enable-debug]) ;;
    esac],
    [debug=false])
AM_CONDITIONAL(DEBUG, test x"$debug" = x"true")
AM_COND_IF(DEBUG,
    AC_DEFINE(DEBUG, 1, [Define to 0 if this is a release build]),
    AC_DEFINE(DEBUG, 0, [Define to 1 or higher if this is a debug build]))
