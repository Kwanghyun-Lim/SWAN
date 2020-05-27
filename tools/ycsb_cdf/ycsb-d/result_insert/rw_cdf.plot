#set terminal postscript eps enhanced font 'Helvetica,16' linewidth 1
set terminal png size 950,600 enhanced font 'Helvetica,25' linewidth 1

set key at 10000000,0.994
set key box width 0 height 0.7
#set key horizontal
set title "YCSB-D Insert"
set xlabel "Time (µsec)"
set ylabel "CDF"
#set yrange [90:100]
set xrange [0:10000000]
set yrange [0.99:1]
set color

set xtics rotate by 45 right   

set style line 1 lt 1 lw 7 lc rgb "blue"
set style line 2 lt 1 lw 7 lc rgb "coral"
set style line 3 lt 1 lw 7 lc rgb "black"

set output "D_insert_CDF.png"
plot "swan.dat" using 1:2 ti "SWAN" w l ls 1, \
	 "lograid.dat" using 1:2 ti "Log-RAID4" w l ls 2, \
     "md4.dat" using 1:2 ti "RAID4" w l ls 3
