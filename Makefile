all: doenerfs mkdoenerfs undoenerfs

doenerfs: doenerfs.c doenerfs_common.c doenerfs.h
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables doenerfs.c doenerfs_common.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o doenerfs

undoenerfs: undoenerfs.c doenerfs_common.c doenerfs.h
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables undoenerfs.c doenerfs_common.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o undoenerfs

mkdoenerfs: mkdoenerfs.c
	gcc -g3 -W -Wall -O0 -D_FORTIFY_SOURCE=2 -fstack-protector -funwind-tables -fasynchronous-unwind-tables mkdoenerfs.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o mkdoenerfs

update:
	rm -rf openSUSE:Factory:Live
	osc co openSUSE:Factory:Live/doenerfs
	git archive --format tar HEAD | bzip2 > openSUSE:Factory:Live/doenerfs/doenerfs.tar.bz2
	commit=`git log -n 1 HEAD | head -n 1` ;\
	cd openSUSE:Factory:Live/doenerfs/ ;\
	osc commit -m "$$commit"  .
	rm -rf openSUSE:Factory:Live
