import sys
check = 0
tmp = '0'
count = 0
traces = 1
read = 0
write = 0
if len(sys.argv) > 1:
    filename = sys.argv[1]
    outname = sys.argv[2]
    file = open(filename, 'r')
    rf = open(outname,'w')
#    wf = open("write_result", 'w')
    line = file.readline().strip()
#    test = line.split()
#    tmp = test[7]
    while(line):
        filed = line.split();
#        print(filed[0])
#        print(filed[1])
#        print(filed[2])
#        print(filed[3])
#        print(filed[4])
#        print(filed[5])
#        print(filed[6])
#        print(filed[7])
#        print(filed[8])
#        print(filed[9])
#        print(filed[10])
        if filed[6] != 'N':
#            if tmp > filed[7]:
#                tmp = filed[7]
#	    print("hello")
            if filed[6] == 'WS':
                rf.write('W')
                write+=int(filed[9])
                rf.write(" ")
                rf.write(filed[7]) #offset
                rf.write(" ")
                rf.write(filed[9]) #count
                rf.write('\n')
            else:
                rf.write(filed[6]) #read write
                read+=int(filed[9])
#            count += int(filed[9])
                rf.write(" ")
                rf.write(filed[7]) #offset
	        rf.write(" ")
	        rf.write(filed[9]) #count
	        rf.write('\n')
            traces+=1
#            if read+write >= 1000:
#                break
#            if write >= 180363216:
#                break
#        else:
#            wf.write(line)
#	    wf.write('\n')
	line = file.readline().strip()
    print("all count ",traces)
    print("read count", read)
    print("write count", write)
    print("read+write", read+write)
    file.close()
    rf.close()
else:
    print("input error")
