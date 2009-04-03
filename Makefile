all: doenerfs mkdoenerfs undoenerfs

CFLAGS=-g3 -W -Wall

doenerfs: doenerfs.c doenerfs_common.c doenerfs.h
	gcc $(CFLAGS) doenerfs.c doenerfs_common.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o doenerfs

undoenerfs: undoenerfs.c doenerfs_common.c doenerfs.h
	gcc $(CFLAGS) undoenerfs.c doenerfs_common.c -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o undoenerfs

mkdoenerfs: mkdoenerfs.cpp
	g++ $(CFLAGS) mkdoenerfs.cpp -D_FILE_OFFSET_BITS=64  -llzma -lfuse -o mkdoenerfs -lcrypto

update:
	rm -rf openSUSE:Factory:Live
	osc co openSUSE:Factory:Live/doenerfs
	git archive --format tar HEAD | bzip2 > openSUSE:Factory:Live/doenerfs/doenerfs.tar.bz2
	commit=`git log -n 1 HEAD | head -n 1` ;\
	cd openSUSE:Factory:Live/doenerfs/ ;\
	osc commit -m "$$commit"  .
	rm -rf openSUSE:Factory:Live

install:
	install -m 755 doenerfs /usr/bin
	install -m 755 undoenerfs /usr/bin
	install -m 755 mkdoenerfs /usr/bin

