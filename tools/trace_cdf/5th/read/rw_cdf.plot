#set terminal png size 950,600 enhanced font 'Helvetica,16' linewidth 1
set terminal postscript eps enhanced font 'Helvetica,28' linewidth 1
#set key at 450,0.99
#set key box width 2 height 1.5
set output "read_CDF.eps"
set color
set multiplot
set key horizontal outside top center
set key font 'Helveltica,22'
#set title "READ"
set xlabel "Time (msec)"
set ylabel "CDF"
#set yrange [90:100]
set xrange [0:200]
set yrange [0.95:1]

set style line 1 lt 1 lw 7 lc rgb "blue"
set style line 2 lt 1 lw 7 dt 2 lc rgb "red"

plot "swan_read.dat" using 1:2 ti "SWAN0" w l ls 1, \
	"md0_read.dat" using 1:2 ti "RAID0" w l ls 2

unset key
unset xlabel
unset ylabel
unset title
set tics font 'Helvetica,25'
set size 0.70,0.55
set origin 0.23,0.23
set xrange [0:50]
set yrange [0.99:1]

plot "swan_read.dat" using 1:2 ti "SWAN0" w l ls 1, \
	"md0_read.dat" using 1:2 ti "RAID0" w l ls 2

