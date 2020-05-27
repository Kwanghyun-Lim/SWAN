set terminal postscript eps enhanced font 'Helvetica,16' linewidth 1
set key at 1300,99.55,2
set key box width 2 height 1.5
set key horizontal
set xlabel "Time (nsec)"
set ylabel "CDF(%)"
set yrange [98:100]
set xrange [600:1500]
set color

set style line 2 lt 1 lw 7 lc rgb "coral"

set output "w_CDF.pdf"
plot "cal_write" using 1:2 ti "Write" w l ls 2
