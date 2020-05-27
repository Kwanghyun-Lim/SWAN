#echo "20" > /proc/sys/vm/pagecache
#sysctl -w vm.pagecache="40"
/root/Prj/SWAN/swan_write_read_FB/my_compile-3R3C.sh

free -m
sync; echo 3 > /proc/sys/vm/drop_caches
free -m
filebench -f fileserver.f
