HDD=/dev/nvme0n1p1

SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd

cd ./util
source umount_dmsrc
cd ..
cd driver
make
modprobe raid456
insmod dm-src.ko
cd ../util
make

./dmsrc_create -n 1 -N 3 -e none $SSD0 $SSD1 $SSD2 $HDD

source mount_dmsrc $HDD $SSD0 $SSD1 $SSD2

