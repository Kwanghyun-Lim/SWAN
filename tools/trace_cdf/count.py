import sys
check = 0
tmp = '0'
count = 0
traces = 1
read = 0
write = 0
    
filename = sys.argv[1]
file = open(filename, 'r')
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
        write+=int(filed[2])
    else:
        read+=int(filed[2])
#            count += int(filed[9])
    traces+=1
#            if write >= 180363216:
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
