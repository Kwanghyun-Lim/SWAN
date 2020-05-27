#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define R	1
#define W	0
#define TIME_SCALE	20000.0
#define LEN_STR		30

int main(int argc, char* argv[])
{
	FILE* rfp,* wfp;
	char buffer[1024]={0};
	char rfname[256],wfname[256];
	int bw, seq;
	char str_dev[LEN_STR];
	char str_app[LEN_STR];
	char str_event[LEN_STR];
	char type, dummy;
	double tmp, prev_time=0;
	int blkno, len, read, meta, other_pid=0, pid, read_cnt=0, trace_cnt=0;
	unsigned long time_invert=0, pid0_len=0, pid1_len=0, other_len=0;
	
	if(argc!=2)
	{
		printf("Input a data file!!\n");
		return 0;
	}
	strcpy(rfname, argv[1]);
	strcpy(wfname, argv[1]);
	strcat(wfname,".bw");
	
	if(rfp = fopen(rfname, "rt"))
	{
		if(wfp = fopen(wfname,"w+"))
		{
			trace_cnt = 0;
			while(!feof(rfp))
			{
				fgets(buffer, 1024, rfp);
				sscanf(buffer,"%s %f %f %d %f %f", 
					str_dev, &tmp, &tmp, &bw, &tmp, &tmp);
			//	printf("%s %f %f %d %f %f\n", 
			//		str_dev, tmp, tmp, bw, tmp, tmp);
				
				trace_cnt++;
				fprintf(wfp,"%d %d\n",trace_cnt, bw);
				printf("%d %d\n",trace_cnt, bw);

			}
		}
//		printf("other_pid:%d other_len:%ld pid0_len:%ld pid1_len:%ld trace_cnt:%d read_cnt:%d time_invert:%ld\n", other_pid, other_len, pid0_len, pid1_len, trace_cnt, read_cnt, time_invert);
	}
	fclose(rfp);
	fclose(wfp);
}
