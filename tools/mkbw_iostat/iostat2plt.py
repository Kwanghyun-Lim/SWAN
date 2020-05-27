#-*- coding: utf-8 -*-
import argparse
import numpy as np

START = 1
LENGTH = 9999999


# IOStat 로그파일 파싱 후, Dictionary 리스트로 반환
def parse_iostat(filename, dev, start=START, length=LENGTH):
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
			if len(s) != 6:
				continue
			if dev == "cpu":
				# s[0] is float?
				if not s[0].replace(".","",1).isdigit():
					continue
				log["user"] = float(s[0])
				log["nice"] = float(s[1])
				log["system"] = float(s[2])
				log["iowait"] = float(s[3])
				log["steal"] = float(s[4])
				log["idle"] = float(s[5])
			else:
				if dev != s[0]:
					continue
				log["tps"] = float(s[1])
				log["read_per_sec"] = float(s[2])
				log["write_per_sec"] = float(s[3])
				log["bandwidth"] = float(s[2]) + float(s[3])
				log["read"] = float(s[4])
				log["write"] = float(s[5])

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
	parser.add_argument("log_file", help="Iostat Log File (str)")
	parser.add_argument("dev", help="Dev (str)")
	parser.add_argument("-s", "--start", type=int, help="Start second (int), default="+str(START), default=START)
	parser.add_argument("-l", "--length", type=int, help="Length second (int), default="+str(LENGTH), default=LENGTH)	
	args = parser.parse_args()
	START = args.start
	LENGTH = args.length

	# 파싱
	dict_iostat = parse_iostat(args.log_file,args.dev)

	# Bandwidth 리스트 추출
	list_bandwidth = [x["bandwidth"] for x in dict_iostat]

	# 평균 
	#print "Avg Bandwidth : ", np.mean(list_bandwidth)

	# Bandwidth 시간별 Gnuplot 포멧 출력
	for i, x in enumerate(list_bandwidth):
		print i, x



