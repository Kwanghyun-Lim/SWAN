#echo "20" > /proc/sys/vm/pagecache
#sysctl -w vm.pagecache="40"


./disable_randomize.sh
free -h
sync; echo 3 > /proc/sys/vm/drop_caches
free -h
./filebench/filebench -f ./fileserver.f > raid4_fileserver89_fixed2.filebench
