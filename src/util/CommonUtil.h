#ifndef _UTIL_H
#define _UTIL_H

#include <cstdlib>
#include <iostream>
#include <string.h>
#include <stdio.h>
#include <functional>
#include <vector>
#include <list>
#include "ql/time/date.hpp"
#include <unordered_map>

#ifdef _WIN32
#include <Windows.h>
#else
#include <unistd.h>
#endif

#ifdef _WIN32
#include <time.h>
#else
#include <utime.h>
#include <sys/time.h>
#endif

#include <sstream>
#include <locale>
#include <codecvt>
#include <random>

#define APP_SERIAL_NO "5e995b81dfb69be0a130c8e1"

#define ORDER_STATUS_REPORTING '0'
#define ORDER_STATUS_REPORTED '2'
#define ORDER_STATUS_CANCELLING '3'
#define ORDER_STATUS_PARTCANCELLED '5'
#define ORDER_STATUS_CANCELLED '6'
#define ORDER_STATUS_PARTTRADED '7'
#define ORDER_STATUS_FULLTRADED '8'
#define ORDER_STATUS_FAILED '9'
#define ORDER_STATUS_UNKOWN '\255'


class chs_codecvt : public std::codecvt_byname<wchar_t, char, std::mbstate_t> {
public:
    chs_codecvt(std::string s) : codecvt_byname(s) {

    }
};


template<typename Type>
void SetZero(Type &inst) {
    memset(&inst, 0, sizeof(inst));
}

template<typename T>
int GetArraySize(T &array) {
    return sizeof(array) / sizeof(array[0]);
}

void vSplitString(std::string strSrc, std::vector<std::string> &vecDest, char cSeparator);

void fastSplitStr(char *lineBuf, std::vector<char *> &vecDest, char cSeparator);

std::string trim1(const std::string &str);

char **CreateInstArray(const std::vector<std::string> &vctInstrument);

void DestroyInstArray(char **pArray, size_t uiArray);

//char* ConvertLPWSTRToLPSTR(LPWSTR lpwszStrIn);

std::string getTimeOfDayMs(int64_t t);

const char *getExchangeID(const char *id);

bool isValidInstID(std::string instID, std::string exchangeID);

std::string getLocalTimeStr(std::string formatStr);

std::string getLocalTimeValStr(time_t time_seconds, std::string formatStr);

void USecDelay(int delayTime);

std::string bin2hex(std::string &_in);

std::string parseHsTimeToStr(long int time,char* format);
std::string parseHsDateToStr(long int date,char* format);
uint64_t parseHsTimeToPosixTime(long int t);
std::string parseHsDateTimeToStr(long int time,char* format);

std::string StringToUTF8Encoding(std::string src,std::string srcEncoding);
std::string GetLocalTimeStr();
std::string GetLocalDateStr();
int GetLocalTimeInt();
int GetLocalDateInt();
double GetLocalDateTimeMsec();
std::string getTimeString();
int GenRandom(int start,int end);
unsigned long long GetCurrentTimeMsec();

std::string IntToString(int value);
std::string DoubleToString(double value, int floor = 4);

std::string getFullStockCode(std::string stock_code, int exchange_type);

#ifdef _WIN32
LPCWSTR stringToLPCWSTR(std::string orig);
#endif

long long getLocalTimestamp();
std::string getLocalTimesStr(std::string format);

std::string UTF8String2GBK(std::string content);

bool isToday(std::string date);

bool isTodayTimeStamp(long timestamp);

void StrcopyTrim(char* dst,const char* src,size_t len=32);

std::string replaceString(std::string strSrc,std::string strPat,std::string strReplace);

std::string GetUUID();

std::string joinVec(std::vector<std::string>* vec, std::string start, std::string sep, std::string end, std::string delimiter="'");
std::string joinDateList(std::list<QuantLib::Date>* list, std::string start, std::string sep, std::string end, std::string = "%Y%m%d");
std::string joinDateVec(std::vector<QuantLib::Date>* vec, std::string start, std::string sep, std::string end, std::string fmt = "%Y%m%d");

std::string time2String(long stamp);


time_t convertTimeStr2TimeStamp(std::string& timeStr);

time_t getTimeStamp(std::string& date);

time_t getTimestampBeforeOpen(std::string& date);

time_t getTimestampAfterClose(std::string& date);

void mapValueToArray(std::unordered_map<std::string, double> *map, double *arr);

int compareDoubleValue(double value1,double value2);

std::string toUpper(std::string str);
std::string toLower(std::string str);
std::string getFirstNumberSubstr(const std::string& str);
#endif //_UTIL_H


