set terminal postscript eps enhanced font 'Helvetica,16' linewidth 1
set key at 3000,90,2
set key box width 2 height 1.5
set key horizontal
set xlabel "Time (nsec)"
set ylabel "CDF(%)"
set yrange [0:100]
set xrange [0:3500]
set color

set style line 1 lt 1 lw 7 lc rgb "blue"
set style line 2 lt 1 lw 7 lc rgb "coral"

set output "CDF.pdf"
plot "ori_cal_read" using 1:2 ti "Read" w l ls 1, \
	"ori_cal_write" using 1:2 ti "Write" w l ls 2
