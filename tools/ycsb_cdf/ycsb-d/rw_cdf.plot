set terminal postscript eps enhanced font 'Helvetica,16' linewidth 1
set key at 600,100,2
set key box width 2 height 1.5
set key horizontal
set xlabel "Time (nsec)"
set ylabel "CDF(%)"
#set yrange [90:100]
#set xrange [0:1200]
set yrange [0.99:1]
set color

set style line 1 lt 1 lw 7 lc rgb "blue"
set style line 2 lt 1 lw 7 lc rgb "coral"

set output "CDF.pdf"
plot "test1" using 1:2 ti "lograid" w l ls 1
#	"swan_ms_read_latency" using 1:2 ti "swan" w l ls 2
