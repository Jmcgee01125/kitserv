#!/bin/sh
#
# Install Kitserv locally.
# Change these values to reflect your desired install location.
# They may also be overridden by same-named environment variables
#
# KITSERV_INCDIR - include directory (kitserv.h)
# KITSERV_LIBDIR - library directory (kitserv.a)
# KITSERV_BINDIR - standalone server biary directory
# KITSERV_MANDIR - man pages directory

if [ -z "${KITSERV_INCDIR}" ]; then
    KITSERV_INCDIR=~/.local/lib/
    echo Using default include directory: ${KITSERV_INCDIR}
else
    echo Using include directory: ${KITSERV_INCDIR}
fi
if [ -z "${KITSERV_LIBDIR}" ]; then
    KITSERV_LIBDIR=~/.local/lib/
    echo Using default library directory: ${KITSERV_LIBDIR}
else
    echo Using library directory: ${KITSERV_LIBDIR}
fi
if [ -z "${KITSERV_BINDIR}" ]; then
    KITSERV_BINDIR=~/.local/bin/
    echo Using default binary directory: ${KITSERV_BINDIR}
else
    echo Using binary directory: ${KITSERV_BINDIR}
fi
if [ -z "${KITSERV_MANDIR}" ]; then
    KITSERV_MANDIR=~/.local/share/man/
    echo Using default man page directory: ${KITSERV_MANDIR}
else
    echo Using man page directory: ${KITSERV_MANDIR}
fi

export KITSERV_INCDIR
export KITSERV_LIBDIR
export KITSERV_BINDIR
export KITSERV_MANDIR

echo
while true; do
    read -p "Continue with install? [Y/N] " yn
    case $yn in
        [Yy]* ) make install; break;;
        [Nn]* ) exit;;
        * ) ;;
    esac
done

