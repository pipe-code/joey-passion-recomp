#!/bin/sh
# Link the Vita ELF, making sure the gap between the code segment and the data segment is large enough
# for the SCE metadata vita-elf-create appends to segment 0 (it aborts with "segment 1 overlaps" when the
# code happens to end just below a page boundary). If the gap is too small, relink with a .rodata pad
# that pushes the code end just past the boundary.
#   usage: vita_link.sh CC OUT.elf PADDIR -- objects... -- libs...
set -e
CC=$1; OUT=$2; PADDIR=$3; shift 3
[ "$1" = "--" ] && shift
OBJS=""; while [ "$1" != "--" ]; do OBJS="$OBJS $1"; shift; done; shift
LIBS="$*"
READELF="$(dirname "$CC")/arm-vita-eabi-readelf"
NEED=4096   # comfortably above what vita-elf-create needs (~2.8 KB)
$CC -Wl,-q -o "$OUT" $OBJS $LIBS
gap() {
  set -- $("$READELF" -lW "$OUT" | awk '$1=="LOAD"{printf "%s %s ", $3, $5}')
  echo $(( $3 - ($1 + $2) ))
}
G=$(gap)
if [ "$G" -lt "$NEED" ]; then
  PAD=$((G + 64))
  mkdir -p "$PADDIR"
  printf '__attribute__((used)) const unsigned char vita_segment_pad[%d] = {1};\n' "$PAD" > "$PADDIR/pad.c"
  $CC -c -o "$PADDIR/pad.o" "$PADDIR/pad.c"
  $CC -Wl,-q -o "$OUT" $OBJS "$PADDIR/pad.o" $LIBS
  echo "vita_link: segment gap was $G bytes, padded by $PAD -> gap $(gap)"
fi
