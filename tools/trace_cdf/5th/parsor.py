import sys

count = [0] * 12
check = 0
tmp = 0.0
if len(sys.argv) > 1:
    filename = sys.argv[1]
    outname = sys.argv[2]
    file = open(filename, 'r')
    out = open(outname, 'w')
    line = file.readline().strip()
    while(line):
        filed = line.split();
        check = check + 1
        tmp = float(filed[0]) / float(1000)
        out.write(str(tmp))
#        out.write(filed[0])
        out.write(' ')
	out.write(filed[2])
	out.write('\n')
	line = file.readline().strip()

#    for i in count: 
#	print(i)
#    for i in range(len(count)):
#	print((i+1)*100,float(count[i])/check*100)
#        tmp += float(count[i])/check*100
#	print((i+1)*100,tmp)
    print("Count: ", check)
#    print(tmp)
    file.close()
    out.close()
else:
    print("input error")
