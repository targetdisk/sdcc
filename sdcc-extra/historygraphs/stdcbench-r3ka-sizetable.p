set output "stdcbench-r3ka-size.svg"
set terminal svg size 640,480
set style data lines
set xlabel "revision"
set ylabel "size [B]"
set arrow from 12085, 25431 to 12085, 25331
set label "4.1.0" at 12085, 25431
set arrow from 13131, 24610 to 13131, 24510
set label "4.2.0" at 13131, 24610
plot "stdcbench-r3ka-sizetable" using 1:4 title "default", "stdcbench-r3ka-sizetable" using 1:2 title "size", "stdcbench-r3ka-sizetable" using 1:3 title "speed"

