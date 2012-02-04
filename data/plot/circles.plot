#!/usr/bin/gnuplot
l = strlen(in)
out = sprintf("%s.%s",substr(in, 0, l-4), type);

if( type eq "png" ) set terminal png size 1600,1200; else set terminal type
   
set output out;

set title "Odometry Data"
set xlabel "X (meters)"
set ylabel "Y (meters)"
set style fill transparent solid 0.2 noborder
plot in using 1:2:(cos($4)/5):(sin($4)/5) with vectors title "Odometry", \
   in using 1:2:3 with circles title "Position", \
   in using 1:2 with lines title "Path";
