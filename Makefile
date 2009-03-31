all: doenerfs mkdoenerfs undoenerfs

doenerfs: doenerfs.c doenerfs_common.c doenerfs.h
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables doenerfs.c doenerfs_common.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o doenerfs

undoenerfs: undoenerfs.c doenerfs_common.c doenerfs.h
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables undoenerfs.c doenerfs_common.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o undoenerfs

mkdoenerfs: mkdoenerfs.c
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables mkdoenerfs.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o mkdoenerfs
