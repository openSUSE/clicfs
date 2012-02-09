set -e 
make
cp src/clicfs initrd/usr/bin/clicfs
loaderdir=$(cd CD1 ; ls -1 boot/*/loader/initrd)
cd initrd 
find . | cpio --create --format=newc --quiet | gzip -9 -f > ../CD1/$loaderdir
cd ..
loaderdir=$(cd CD1 ; ls -1 boot/*/loader/isolinux.bin)
genisoimage -V 0x8efb7557 -R -J -f -pad -joliet-long -no-emul-boot -boot-load-size 4 -boot-info-table -b $loaderdir -o kde2.iso CD1/
isohybrid -id 0x8efb7557 kde2.iso 
dd if=/dev/zero seek=1000 count=1 bs=1M of=kde2.iso

