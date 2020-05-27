#include <stdio.h>
#include <iostream>
#include <math.h>
#include <vector>
#include <fstream>

using namespace std;

double Sum(vector<double>& data, int count);         // 총합

double Mean(vector<double>& data, int count);        // 평균

double Var(vector<double>& data, int count);         // 분산

double Stdev(vector<double>& data, int count);       // 표준편차



int main(int argc, char** argv)

{

    vector<double> data;
    double tmp = 0;
    int n = 0;
    ifstream infile;
    int i;



    // n개의수를읽어들인다
    infile.open(argv[1]);
    while(!infile.eof()){
        infile >> tmp;
        data.push_back(tmp);
        n++;
    }


    // n개의수를출력

//    for ( i = 0; i < n; ++i )
//
//        printf("%10.5lf", data[i]);


    // 계산결과출력

    printf("자료수: %d\n", n-1);

    printf("총   합: %.5lf\n", Sum(data, n-1));

    printf("평   균: %.5lf\n", Mean(data, n-1));

    printf("분   산: %.5lf\n", Var(data, n-1));

    printf("표준편차: %.5lf\n", Stdev(data, n-1));

    return 0;

}



double Sum(vector<double>& data, int count)

{

    double sum = 0.0;

    int i;



    for ( i = 0; i < count; ++i )

        sum += data[i];



    return sum;

}



double Mean(vector<double>& data, int count)

{

    return ( Sum(data, count) / count );

}



double Var(vector<double>& data, int count)

{

    int i;

    double sum = 0.0;

    double mean = Mean(data, count);



    for ( i = 0; i < count; ++i )

        sum += ( data[i] - mean ) * ( data[i] - mean );



    sum /= (double)(count - 1);



    return sum;

}



double Stdev(vector<double>& data, int count)

{

    return ( sqrt(Var(data, count)) );

}
