#-*- coding: utf-8 -*-
import argparse
import numpy as np

START = 1
LENGTH = 9999999


# dmesg 로그파일 파싱 후, Dictionary 리스트로 반환
def parse_dmesg(filename, key, start=START, length=LENGTH):
	f = open(filename, "r")
	res = []
	cnt = 0
	for line in f.readlines():
		if cnt < start:
			cnt += 1
			continue
		if cnt >= start+length:
			return res
                
		log = {}
		s = line.split()
		try:
                        if len(s) < 6:
                                continue
			if key == "normal_io":
                                if s[1] == "###" and s[2] == "normal":
				        log["user_read"] = int(s[5])
			                log["user_write"] = int(s[9])
                                        res.append(log)

                        elif key == "gc_io":
                                if s[1] == "###" and s[2] == "gc":
				        log["gc_read"] = int(s[5])
			                log["gc_write"] = int(s[9])
                                        res.append(log)
			
                                
			elif key == "victim_u":
				if s[3] == "Util":
				        log["timestamp"] = (s[0])
				        log["victim_u"] = int(s[5])
                                        res.append(log)

		except ValueError:
			print str(cnt) , " line Error: "
			print s
		cnt += 1
	f.close()
	return res

# 메인함수
if __name__ == "__main__":

	# 매개변수 설정
	parser = argparse.ArgumentParser()
	parser.add_argument("log_file", help="dmesg log file (str)")
	parser.add_argument("key", help="victim_u, normal_io, or gc_io (str)")
	parser.add_argument("-s", "--start", type=int, help="Start second (int), default="+str(START), default=START)
	parser.add_argument("-l", "--length", type=int, help="Length second (int), default="+str(LENGTH), default=LENGTH)	
	args = parser.parse_args()
	START = args.start
	LENGTH = args.length

	# 파싱
	dict_dmesg = parse_dmesg(args.log_file, args.key)

	# normal_io/gc_io/victim_u 리스트 추출
        if args.key == "normal_io":
	        list_value = [x["user_write"] for x in dict_dmesg]
        elif args.key == "gc_io":
	        list_value = [x["gc_write"] for x in dict_dmesg]
        elif args.key == "victim_u":
	        list_value = [x["victim_u"] for x in dict_dmesg]

	# 평균 
	#print "Avg Bandwidth : ", np.mean(list_bandwidth)

	# Bandwidth 시간별 Gnuplot 포멧 출력
	for i, x in enumerate(list_value):
		print i, x



