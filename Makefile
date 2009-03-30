all: doenerfs mkdoenerfs

doenerfs: doenerfs.c
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables doenerfs.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o doenerfs

mkdoenerfs: mkdoenerfs.c
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables mkdoenerfs.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o mkdoenerfs
