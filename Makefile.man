all: clicfs mkclicfs unclicfs

CFLAGS=-g3 -W -Wall
CPPFLAGS=-D_LARGEFILE_SOURCE=1  -D_FILE_OFFSET_BITS=64

clicfs: clicfs.c clicfs_common.c clicfs.h
	gcc $(CFLAGS) $(CPPFLAGS) clicfs.c clicfs_common.c -llzma -lfuse -o clicfs

unclicfs: unclicfs.c clicfs_common.c clicfs.h
	gcc $(CFLAGS) $(CPPFLAGS) unclicfs.c clicfs_common.c -llzma -lfuse -o unclicfs

mkclicfs: mkclicfs.cpp
	g++ $(CFLAGS) $(CPPFLAGS) mkclicfs.cpp -llzma -lfuse -o mkclicfs -lcrypto

update:
	rm -rf openSUSE:Factory:Live
	osc co openSUSE:Factory:Live/clicfs
	git archive --format tar HEAD | bzip2 > openSUSE:Factory:Live/clicfs/clicfs.tar.bz2
	commit=`git log -n 1 HEAD | head -n 1` ;\
	cd openSUSE:Factory:Live/clicfs/ ;\
	osc commit -m "$$commit"  .
	rm -rf openSUSE:Factory:Live

install:
	install -m 755 clicfs /usr/bin
	install -m 755 unclicfs /usr/bin
	install -m 755 mkclicfs /usr/bin

clean:
	rm -f clicfs mkclicfs unclicfs
