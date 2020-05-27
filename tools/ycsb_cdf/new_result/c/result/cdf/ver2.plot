set terminal postscript eps enhanced font 'Helvetica,28' linewidth 1
#set terminal pngcairo size 980,800 enhanced font 'Helvetica,28'
set output "C_read_CDF_ver2.eps"
set multiplot
set key horizontal outside top width 4
set key font 'Helvetica,22'
set color
#set key at 1800,0.998
#set key box width 1.5 height 0.8
#set key horizontal
#set title "YCSB-A Read"
set xlabel "Time (msec)"
#set ylabel "CDF"
#set yrange [90:100]
set xrange [0:1000]
set yrange [0.995:1]
#set yrange [0.7:1]
#set yrange [0:1]
#set color
set xtics rotate by 45 right
set style line 1 lt 1 lw 6 dt 1 lc rgb "blue"
set style line 2 lt 1 lw 6 dt '.' lc rgb "red"
set style line 3 lt 1 lw 6 dt '-' lc rgb "black"
set style line 4 lt 1 lw 6 lc rgb "dark-grey"
set style line 5 lt 1 lw 6 dt 4 lc rgb "dark-green"

plot "swan.dat" using 1:2 ti "SWAN" w l ls 1, \
	"md5.dat" using 1:2 ti "RAID5" w l ls 2, \
    "md4.dat" using 1:2 ti "RAID4" w l ls 3, \
    "log_raid.dat" using 1:2 ti "LOGRAID" w l ls 5
#    "swan_sp.dat" using 1:2 ti "SWAN-SP" w l ls 4, \

unset key
unset xlabel
unset ylabel
unset title
set tics font 'Helvetica,25'
set size 0.70,0.55
set origin 0.25,0.23
set ytics 0.0001
set xrange [0:600]
set yrange [0.9995:1]
plot "swan.dat" using 1:2 ti "SWAN" w l ls 1, \
	"md5.dat" using 1:2 ti "RAID5" w l ls 2, \
    "md4.dat" using 1:2 ti "RAID4" w l ls 3, \
    "log_raid.dat" using 1:2 ti "LOGRAID" w l ls 5

