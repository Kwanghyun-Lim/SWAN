
 ## How to make CDF

 - 1. Erase useless text
    - ex) Raw data measurement: will output to stdout.
    - ex) READ latency raw data: op, timestamp(ms), latency(us)

 - 2. Put the resulting file into the parser's input
    - python parsor.py "input file" "output file"

 - 3. Run a.out (sort.cpp)
    - ./a.out "python output file" > "output file.dat"

 - 4. Run plot file
    - check your dat file name in rw_cdf.plot
    - gnuplot rw_cdf.plot
