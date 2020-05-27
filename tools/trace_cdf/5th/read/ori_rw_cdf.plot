set terminal png size 950,600 enhanced font 'Helvetica,16' linewidth 1
set key at 450,0.99
set key box width 2 height 1.5
set key horizontal
set title "READ"
set xlabel "Time (msec)"
set ylabel "CDF"
#set yrange [90:100]
#set xrange [0:80]
set yrange [0.95:1]
set color

set style line 1 lt 1 lw 7 lc rgb "blue"
set style line 2 lt 1 lw 7 lc rgb "coral"

set output "read_CDF.png"
plot "swan_read.dat" using 1:2 ti "SWAN0" w l ls 1, \
	"md0_read.dat" using 1:2 ti "RAID0" w l ls 2
