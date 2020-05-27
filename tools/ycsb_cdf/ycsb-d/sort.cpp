#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>

using namespace std;

struct cdf{
    int latency;
    int count;
    float cal;
};

int main(int argc, char** argv){
    vector<int> number;
    vector<cdf> all;
    cdf tmp;
    int temp = 0, loop = 0, size = 0, cd = 0, a = 1;

    ifstream infile;

    infile.open(argv[1]);

    while(!infile.eof()){
        infile >> temp;
        number.push_back(temp);
    }

//    sort(number.begin(), number.end(), greater<int>());
     sort(number.begin(), number.end());

//    for(loop = 0; loop < (number.size()-1); loop++){
//        cout << number[loop] <<endl;
//    }
    for(loop = 1; loop < (number.size()-1); loop++){
        if(number[loop] == number[loop-1]){
            a += 1;
        }else{
            tmp.latency = number[loop-1];
            tmp.count = a;
            tmp.cal = (float)cd / (float)(number.size()-1);
            all.push_back(tmp);
            cd += a;
            a = 1;
        }
        if(loop == number.size()-2){
            tmp.latency = number[loop-1];
            tmp.count = a;
            tmp.cal = (float)cd / (float)(number.size()-1);
            all.push_back(tmp);
            cd += a;
            a = 1;
        }
    }

    for(loop = 0; loop < (all.size()-1); loop++){
        cout << all[loop].latency<<" "<<all[loop].count<<" "<<all[loop].cal <<endl;
//        cout << all[loop].latency<<" "<<all[loop].cal <<endl;

    }


    cout <<"all size : "<< number.size()-1 <<endl;
    cout <<"count : "<<cd<<endl;
    return 0;
}
