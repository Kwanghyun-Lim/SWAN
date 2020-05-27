#echo "20" > /proc/sys/vm/pagecache
#sysctl -w vm.pagecache="40"


./disable_randomize.sh
free -h
sync; echo 3 > /proc/sys/vm/drop_caches
free -h
./filebench/filebench -f ./webserver.f > raid4_webserver_89%_fixed.filebench
