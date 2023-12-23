#!/bin/sh
#
# Kitserv installation script.
#
# Change these values to reflect your desired install location.
# They may also be overridden by same-named environment variables.
# KITSERV_INCDIR - include directory (kitserv.h)
# KITSERV_LIBDIR - library directory (libkitserv.a)
# KITSERV_BINDIR - standalone server binary directory
# KITSERV_MANDIR - man pages directory
#
# If PREFIX is defined, then Kitserv will be installed as follows:
# PREFIX/include - kitserv.h
# PREFIX/lib     - libkitserv.a
# PREFIX/bin     - standalone server binary
# PREFIX/man     - man pages
# The prior environment variables may still be used as overrides.
# Additionally, PREFIX will suppress the confirmation prompt.

if [ -z "${KITSERV_INCDIR}" ]; then
    if [ -z "${PREFIX}" ]; then
        KITSERV_INCDIR=~/.local/lib/
    else
        KITSERV_INCDIR=${PREFIX}/include
    fi
fi
if [ -z "${KITSERV_LIBDIR}" ]; then
    if [ -z "${PREFIX}" ]; then
        KITSERV_LIBDIR=~/.local/lib/
    else
        KITSERV_LIBDIR=${PREFIX}/lib
    fi
fi
if [ -z "${KITSERV_BINDIR}" ]; then
    if [ -z "${PREFIX}" ]; then
        KITSERV_BINDIR=~/.local/bin/
    else
        KITSERV_BINDIR=${PREFIX}/bin
    fi
fi
if [ -z "${KITSERV_MANDIR}" ]; then
    if [ -z "${PREFIX}" ]; then
        KITSERV_MANDIR=~/.local/share/man/
    else
        KITSERV_MANDIR=${PREFIX}/man
    fi
fi

echo Using include directory: ${KITSERV_INCDIR}
echo Using library directory: ${KITSERV_LIBDIR}
echo Using binary directory: ${KITSERV_BINDIR}
echo Using man page directory: ${KITSERV_MANDIR}

export KITSERV_INCDIR
export KITSERV_LIBDIR
export KITSERV_BINDIR
export KITSERV_MANDIR

if [ -z "${PREFIX}" ]; then
    echo
    while true; do
        read -p "Continue with install? [Y/N] " yn
        case $yn in
            [Yy]* ) make install; break;;
            [Nn]* ) exit;;
            * ) ;;
        esac
    done
else
    make install
fi
