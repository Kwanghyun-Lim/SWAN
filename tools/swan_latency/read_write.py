import sys
check = 0
if len(sys.argv) > 1:
    filename = sys.argv[1]
    file = open(filename, 'r')
    rf = open("read_log",'w')
    wf = open("write_log", 'w')
    line = file.readline().strip()
    while(line):
        filed = line.split(', ');
        check = check + 1
#        print(filed[0])
#        print(filed[1])
#        print(filed[2])
#        print(filed[3])
        if filed[2] == '0':
#	    print("hello)"
            rf.write(line)
	    rf.write('\n')
        else:
            wf.write(line)
	    wf.write('\n')
	line = file.readline().strip()

    file.close()
