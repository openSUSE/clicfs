rm -rf filesystems
osc co filesystems/clicfs
git archive --format tar HEAD | bzip2 > filesystems/clicfs/clicfs.tar.bz2
commit=`git log -n 1 HEAD | head -n 1` ;\
( cd filesystems/clicfs/ ;\
osc commit -m "$$commit" . )
rm -rf filesystems
