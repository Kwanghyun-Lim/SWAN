#echo "20" > /proc/sys/vm/pagecache
#sysctl -w vm.pagecache="40"


./disable_randomize.sh
free -h
sync; echo 3 > /proc/sys/vm/drop_caches
free -h
./filebench/filebench -f ./webproxy.f > raid4_webproxy89_fixed.filebench
