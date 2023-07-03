#!/bin/sh
#
#  Copyright (c) 2023, Rice University.
#  See the file LICENSE for details.
#
#  Mark W. Krentel
#  Rice University
#  June 2023
#
#  Set SPACK, DYNINST, BOOST, etc and run:
#   ./mk-test.sh  [ file.cpp ]
#

CXX=g++
CXXFLAGS='-g -O -std=c++11 -fopenmp'

SPACK=/path/to/spack/install/linux-distro-x86_64/gcc-x.y.z

DYNINST=${SPACK}/dyninst-subdir
BOOST=${SPACK}/boost-subdir
ELF=${SPACK}/
TBB=${SPACK}/
XED=${SPACK}/

prog=unknown-x86.cpp

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

#------------------------------------------------------------

set --  \
    $CXXFLAGS  \
    $prog    \
    -o $out  \
    -I${DYNINST}/include  \
    -I${BOOST}/include    \
    -I${ELF}/include  \
    -I${TBB}/include  \
    -I${XED}/include  \
    -L${DYNINST}/lib  \
    -lparseAPI  -linstructionAPI  -lsymtabAPI  \
    -ldynDwarf  -ldynElf  -lcommon  \
    -L${XED}/lib  -lxed  \
    -Wl,-rpath=${DYNINST}/lib  \
    -Wl,-rpath=${XED}/lib

echo
echo $CXX $@
echo

$CXX "$@"

test $? -eq 0 || die "compile failed"

