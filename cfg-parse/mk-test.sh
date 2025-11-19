#!/bin/sh
#
#  SPDX-License-Identifier: MIT
#
#  Copyright (c) 2023-2025, Rice University.
#  See the file LICENSE for details.
#
#  Mark W. Krentel
#  Rice University
#  November 2025
#
#  Set SPACK, DYNINST, BOOST, etc and run:
#   ./mk-test.sh  [ file.cpp ]
#

CXX=g++
CXXFLAGS='-g -O -std=c++11 -fopenmp -Wall'

SPACK=/path/to/spack/install_tree/root/linux-arch

DYNINST=${SPACK}/dyninst-subdir
BOOST=${SPACK}/
ELF=${SPACK}/
TBB=${SPACK}/

prog=cfg-parse.cpp

#------------------------------------------------------------

die() {
    echo "error: $@"
    echo "usage: ./mk-test.sh [ file.cpp ]"
    exit 1
}

if test "x$1" != x ; then
    prog="$1"
fi

test "x$prog" != x || die "missing file name"
test -f "$prog" || die "missing input file: $prog"

case "$prog" in
    *?.?* ) out="${prog%.*}" ;;
    * ) die "missing file name extension" ;;
esac

if test -d "${DYNINST}/lib64" ; then
    libdir=lib64
elif test -d "${DYNINST}/lib" ; then
    libdir=lib
else
    die "unable to find dyninst lib dir in: $DYNINST"
fi

#------------------------------------------------------------

set --  \
    $CXXFLAGS  \
    $prog    \
    -o $out  \
    -I${DYNINST}/include  \
    -I${BOOST}/include    \
    -I${ELF}/include  \
    -I${TBB}/include  \
    -L${DYNINST}/$libdir  \
    -lparseAPI  -linstructionAPI  -lsymtabAPI  -lcommon  \
    -Wl,-rpath=${DYNINST}/$libdir

echo
echo $CXX $@
echo

$CXX "$@"

