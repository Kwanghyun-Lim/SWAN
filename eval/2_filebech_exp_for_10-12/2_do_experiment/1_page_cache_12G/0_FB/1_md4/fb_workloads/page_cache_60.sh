echo "60" > /proc/sys/vm/pagecache
free -m
sync; echo 3 > /proc/sys/vm/drop_caches
free -m
