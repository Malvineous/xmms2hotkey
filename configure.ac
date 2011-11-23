#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.63])
AC_INIT([xmms2hotkey], [0.4], [malvineous@shikadi.net])
AM_INIT_AUTOMAKE([xmms2hotkey], [0.4])
AC_CONFIG_SRCDIR([src/xmms2hotkey.cpp])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_LANG(C++)

# Checks for programs.
AC_PROG_CXX

# Checks for libraries.
AC_PROG_LIBTOOL

AC_PATH_X
if test "x$no_x" = "xyes";
then
	status_x="disabled"
else
	AC_DEFINE([USE_X11], [1], [Define to use X11 hotkeys])
	AC_SUBST([X_LIBS], ["$x_libraries -lX11"])
	status_x="enabled"
fi

AC_ARG_WITH(evdev, AC_HELP_STRING([--with-evdev],[enable evdev hotkeys]))
if test "x$with_evdev" = "xno";
then
	status_evdev="disabled"
else
	AC_CHECK_HEADERS([linux/input.h], [
		AC_DEFINE([USE_EVDEV], [1], [Define to use Linux evdev hotkeys])
		status_evdev="enabled"
	], [
		status_evdev="disabled"
	])
fi

BOOST_REQUIRE([1.42])
BOOST_PROGRAM_OPTIONS
BOOST_THREADS

PKG_CHECK_MODULES([xmms2client], [xmms2-client-cpp >= 0.6])

AC_OUTPUT(Makefile src/Makefile)

echo
echo "Hotkey availability summary:"
echo "  X-Windows:   $status_x"
echo "  Linux evdev: $status_evdev"
