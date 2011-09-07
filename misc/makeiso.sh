set -e
make 
cp src/clicfs initrd/usr/bin/clicfs 
(cd initrd ; find . | cpio --create --format=newc --quiet | gzip -9 -f > ../CD1/boot/x86_64/loader/initrd)
genisoimage -R -J -f -pad -joliet-long -no-emul-boot -boot-load-size 4 -boot-info-table -b boot/x86_64/loader/isolinux.bin -o kde2.iso CD1/
#isohybrid -id $(cat CD1/boot/grub/mbrid) kde2.iso  && dd if=/dev/zero seek=1000 count=1 bs=1M of=kde2.iso
