HDD=/dev/md0

SSD0=/dev/sdb
SSD1=/dev/sdc
SSD2=/dev/sdd

SSD3=/dev/sde
SSD4=/dev/sdf
SSD5=/dev/sdg

SSD6=/dev/sdh
SSD7=/dev/sdi
SSD8=/dev/sdj

cd ./util
source umount_dmsrc
cd ..
cd driver
make
modprobe raid456
insmod dm-src.ko
cd ../util
make

./dmsrc_create -n 1 -N 9 -e none $SSD0 $SSD1 $SSD2 $SSD3 $SSD4 $SSD5 $SSD6 $SSD7 $SSD8 $HDD

source mount_dmsrc $HDD $SSD0 $SSD1 $SSD2 $SSD3 $SSD4 $SSD5 $SSD6 $SSD7 $SSD8

