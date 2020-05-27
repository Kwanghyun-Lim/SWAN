P0 = "./tmp/tmp0"
#P1 = "./tmp/tmp1"
#P2 = "./tmp/tmp2"
#P3 = "./tmp/tmp3"


set terminal pngcairo size 800, 600 enhanced font 'Helveca, 25'
set output "swan-freesegs.png"
#set grid
#set key inside right bottom vertical Left reverse enhanced autotitles box linetype -1 linewidth 1.000
set pointsize 1
#set logscale y
#set format y "%2.2e"
set style data linespoints
#set style data points
set yrange [0:]
set xrange [0:]

set tics font ", 20"
set xlabel "Time (sec)"
#set xtics 0,50,180
set xtics rotate by 45 right
set ylabel "Num. free seg"

set key center top inside horizontal
plot P0 using 1:2 t "swan" lw 1 lc rgbcolor "#ffc000"
# , P2 using 1:2 t "VM-4" lw 1 lc rgbcolor "#a5a5a5" \
# , P1 using 1:2 t "VM-2" lw 1 lc rgbcolor "#dd7c31" \
# , P0 using 1:2 title 'VM-1' lw 1 lc rgbcolor "#5199b5" \
