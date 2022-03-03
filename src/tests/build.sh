#!/bin/sh
CC=clang
CXX=clang++
LD=clang++
CFLAGS="-DHAVE_WCSLIB -Wall -g -I.. -fopenmp `pkg-config --cflags gtk+-3.0` `pkg-config --cflags cfitsio` `pkg-config --cflags gsl`"
LDFLAGS="-fopenmp `pkg-config --libs gtk+-3.0` `pkg-config --libs cfitsio` `pkg-config --libs gsl` -lm"

set -x
# compile the compare_fits tool
$CC $CFLAGS -c -o compare_fits.o compare_fits.c &&
$CC $CFLAGS -c -o dummy.o dummy.c &&
$LD -o compare_fits compare_fits.o dummy.o ../io/image_format_fits.o ../core/utils.o ../core/siril_log.o ../core/siril_date.o $LDFLAGS

# compile the sorting algorithm test (broken, use meson tests)
# $CC $CFLAGS -c -o sorting.o sorting.c &&
# $CC $CFLAGS -DUSE_ALL_SORTING_ALGOS -c -o ../algos/sorting.o ../algos/sorting.c &&
# $CXX $CFLAGS -c -o ../rt/rt_algo.o ../rt/rt_algo.cc
# $LD $LDFLAGS -o sorting sorting.o ../algos/sorting.o ../rt/rt_algo.o `pkg-config --libs criterion` `pkg-config --libs glib-2.0`

# compile the stacking tests
$CC $CFLAGS -DWITH_MAIN -c -o stacking_blocks_test.o stacking_blocks_test.c &&
$CC $CFLAGS -DDUMMY_LOG -c -o dummy.o dummy.c &&
$LD -o stacking_blocks_test stacking_blocks_test.o ../stacking/median_and_mean.o ../core/utils.o dummy.o -Wl,--unresolved-symbols=ignore-all $LDFLAGS

# compile the SER tests
$CC $CFLAGS -DWITH_MAIN -c -o ser_test.o ser_test.c &&
$CC $CFLAGS -DDUMMY_LOG -c -o dummy.o dummy.c &&
$LD -o ser_test ser_test.o ../io/ser.o ../core/siril_date.o ../algos/demosaicing.o ../io/seqwriter.o ../io/image_format_fits.o ../algos/demosaicing_rtp.o ../core/utils.o dummy.o -Wl,--unresolved-symbols=ignore-all $LDFLAGS

# compile the date tests
$CC $CFLAGS -DWITH_MAIN -c -o siril_date_test.o siril_date_test.c &&
#$CC $CFLAGS -DDUMMY_LOG -c -o dummy.o dummy.c &&
$LD -o date_test siril_date_test.o ../core/siril_date.o -Wl,--unresolved-symbols=ignore-all $LDFLAGS


# in mpp, a libsiril.a was created to not require the endless dummy list of functions:
# https://gitlab.com/free-astro/siril/-/blob/mpp/src/Makefile.am
