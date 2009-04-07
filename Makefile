all: doenerfs mkdoenerfs undoenerfs

CFLAGS=-g3 -W -Wall
CPPFLAGS=-D_LARGEFILE_SOURCE=1  -D_FILE_OFFSET_BITS=64

doenerfs: doenerfs.c doenerfs_common.c doenerfs.h
	gcc $(CFLAGS) $(CPPFLAGS) doenerfs.c doenerfs_common.c -llzma -lfuse -o doenerfs

undoenerfs: undoenerfs.c doenerfs_common.c doenerfs.h
	gcc $(CFLAGS) $(CPPFLAGS) undoenerfs.c doenerfs_common.c -llzma -lfuse -o undoenerfs

mkdoenerfs: mkdoenerfs.cpp
	g++ $(CFLAGS) $(CPPFLAGS) mkdoenerfs.cpp -llzma -lfuse -o mkdoenerfs -lcrypto

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

clean:
	rm -f doenerfs mkdoenerfs undoenerfs
