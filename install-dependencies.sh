#!/bin/sh

# requires `autoconf`, `libtool`, `pkg-config`, `openssl` (likely as `libssl-dev`) and `tcl`
# these must be installed manually (e.g. via package manager)

BASE=`pwd`

# clear existing installation to rebuild dependencies if being run multiple times
rm -rf ${BASE}/deps jansson libjwt sqlite
test -d ${BASE}/deps || mkdir ${BASE}/deps
test -d ${BASE}/deps/include || mkdir ${BASE}/deps/include
test -d ${BASE}/deps/obj || mkdir ${BASE}/deps/obj
test -d ${BASE}/deps/src || mkdir ${BASE}/deps/src

git clone https://github.com/akheron/jansson.git
(
    cd jansson;
    autoreconf -fi;
    ./configure --prefix=${BASE}/deps;
    make -j 4 install
)

git clone https://github.com/benmcollins/libjwt.git
(
    cd libjwt;
    autoreconf -fi;
    env PKG_CONFIG_PATH=../deps/lib/pkgconfig:${PKG_CONFIG_PATH} ./configure --prefix=${BASE}/deps;
    make -j 4 install
)

wget https://www.sqlite.org/src/tarball/sqlite.tar.gz?r=release -O sqlite.tar.gz
tar -xzvf sqlite.tar.gz
rm sqlite.tar.gz
(
    cd sqlite;
    autoreconf -fi;
    env PKG_CONFIG_PATH=../deps/lib/pkgconfig:${PKG_CONFIG_PATH} ./configure --prefix=${BASE}/deps;
    make -j 4 sqlite3.c;
    cp sqlite3.c ${BASE}/deps/src/sqlite3.c;
    cp sqlite3.h ${BASE}/deps/include/sqlite3.h;
    cd ${BASE}/deps/src;
    echo Building sqlite3...
)
