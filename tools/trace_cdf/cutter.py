import sys
check = 0
tmp = '0'
count = 0
traces = 1
read = 0
write = 0
    
filename = sys.argv[1]
outname = sys.argv[2]
file = open(filename, 'r')
rf = open(outname, 'w')

line = file.readline().strip()

while(line):
    filed = line.split();
#    print(filed[0])
#    print(filed[1])
#    print(filed[2])
#        print(filed[3])
#        print(filed[4])
#        print(filed[5])
#        print(filed[6])
#        print(filed[7])
#        print(filed[8])
#        print(filed[9])
#        print(filed[10])
#            if tmp > filed[7]:
#                tmp = filed[7]
#	    print("hello")
    if filed[0] == 'W':
        if write <= 25000000:
            write+=int(filed[2])
            rf.write('W')
            rf.write(" ")
            rf.write(filed[1]) #offset
            rf.write(" ")
            rf.write(filed[2]) #count
            rf.write('\n')

    else:
        if read <= 25000000:
            read+=int(filed[2])
            rf.write('R')
            rf.write(" ")
            rf.write(filed[1]) #offset
            rf.write(" ")
            rf.write(filed[2]) #count
            rf.write('\n')

#            count += int(filed[9])
#    traces+=1
    if write+read >= 50000000:
        break
#   if write >= 180363216:
#                break
#        else:
#            wf.write(line)
#	    wf.write('\n')
    line = file.readline().strip()

print(traces)
print("read count", read)
print("write count", write)
print("total", read + write)
file.close()
