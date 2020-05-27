#set terminal postscript eps enhanced font 'Helvetica,16' linewidth 1
set terminal png size 950,600  enhanced font 'Helvetica,25' linewidth 1
set key at 1000,0.9995
#set key box width 0.8 height 0.8
#set key horizontal
set title "YCSB-D Insert"
set xlabel "Time (msec)"
set ylabel "CDF"
#set yrange [90:100]
set ytics 0.0001
set xrange [0:1000]
set yrange [0.999:1]
#set yrange [0.7:1]
#set yrange [0:1]
set color

set xtics rotate by 45 right

set style line 1 lt 1 lw 6 lc rgb "blue"
set style line 2 lt 1 lw 6 lc rgb "coral"
set style line 3 lt 1 lw 6 lc rgb "black"
set style line 4 lt 1 lw 6 lc rgb "dark-grey"
set style line 5 lt 1 lw 6 lc rgb "dark-green"

set output "D_Insert_CDF_ver1.png"
plot "swan.dat" using 1:2 ti "SWAN" w l ls 1, \
	"md5.dat" using 1:2 ti "RAID5" w l ls 2, \
    "md4.dat" using 1:2 ti "RAID4" w l ls 3, \
    "log_raid.dat" using 1:2 ti "LOGRAID" w l ls 5
