rm -rf openSUSE:Factory:Live
osc co openSUSE:Factory:Live/clicfs
git archive --format tar HEAD | bzip2 > openSUSE:Factory:Live/clicfs/clicfs.tar.bz2
commit=`git log -n 1 HEAD | head -n 1` ;\
cd openSUSE:Factory:Live/clicfs/ ;\
osc commit -m "$$commit"  .
rm -rf openSUSE:Factory:Live
