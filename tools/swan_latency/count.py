import sys

count = [0] * 12
check = 0
tmp = 0
if len(sys.argv) > 1:
    filename = sys.argv[1]
    file = open(filename, 'r')
    line = file.readline().strip()
    while(line):
        filed = line.split(', ');
        check = check + 1
#        print(filed[0])

#        print(filed[1])
        if filed[1] <= '100':
            count[0] += 1
        elif filed[1] <= '200':
            count[1] += 1
        elif filed[1] <= '300':
            count[2] += 1
        elif filed[1] <= '400':
            count[3] += 1
        elif filed[1] <= '500':
	    count[4] += 1
        elif filed[1] <= '600':
            count[5] += 1
        elif filed[1] <= '700':
            count[6] += 1
        elif filed[1] <= '800':
            count[7] += 1
        elif filed[1] <= '900':
            count[8] += 1
        elif filed[1] <= '1000':
	    count[9] += 1
        elif filed[1] <= '1100':
            count[10] += 1
	else:
            count[11] += 1
#        print(filed[2])
#        print(filed[3])
	line = file.readline().strip()

#    for i in count: 
#	print(i)
    for i in range(len(count)):
#	print((i+1)*100,float(count[i])/check*100)
        tmp += float(count[i])/check*100
	print((i+1)*100,tmp)
    print("Count: ", check)
    print(tmp)
    file.close()
