#include "util/CommonUtil.h"
#include <cctype>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>

std::string IntToString(int value) {
    std::stringstream ss;
    ss << value;
    return ss.str();
}
std::string DoubleToString(double value, int floor) {
    std::stringstream ss;
    ss << std::setiosflags(std::ios::fixed) << std::setprecision(floor)<< value;
    return ss.str();
}

void USecDelay(int delayTime) {
    usleep((useconds_t)delayTime);
}


unsigned long long GetCurrentTimeMsec() {
	std::time_t result = std::time(NULL);
	return result;
}

void vSplitString(std::string strSrc, std::vector<std::string>& vecDest, char cSeparator) {
    vecDest.clear();
    if (strSrc.empty())
        return;

    std::string::size_type size_pos = 0;
    std::string::size_type size_prev_pos = 0;

    while ((size_pos = strSrc.find_first_of(cSeparator, size_pos)) != std::string::npos) {
        std::string strTemp = strSrc.substr(size_prev_pos, size_pos - size_prev_pos);

        vecDest.push_back(strTemp);
        size_prev_pos = ++size_pos;
    }

    vecDest.push_back(&strSrc[size_prev_pos]);
}

void fastSplitStr(char* lineBuf, std::vector<char*>& vecDest, char cSeparator) {
    char* ptr = lineBuf;
    char* fieldHead = lineBuf;

    while (*ptr != '\0') {

        if (*ptr == cSeparator) {
            *ptr = '\0';
            vecDest.push_back(fieldHead);
            fieldHead = ptr + 1;
        }

        ++ptr;
    }

    vecDest.push_back(fieldHead);
}

std::string trim1(const std::string& str) {
    std::string::size_type pos = str.find_first_not_of(' ');
    if (pos == std::string::npos) {
        return str;
    }
    std::string::size_type pos2 = str.find_last_not_of(' ');
    if (pos2 != std::string::npos) {
        return str.substr(pos, pos2 - pos + 1);
    }
    return str.substr(pos);
}

std::string getTimeOfDayMs(int64_t t) {
    int h, m, s;
    h = t / (60 * 60 * 1000);
    m = t % (60 * 60 * 1000) / (60 * 1000);
    s = t % (60 * 1000) / 1000;

    char timeBuffer[32];
#ifdef _WIN32
    sprintf_s(timeBuffer, "%02d:%02d:%02d", h, m, s);
#else
    sprintf(timeBuffer, "%02d:%02d:%02d", h, m, s);
#endif
    return std::string(timeBuffer);
}

const char* getExchangeID(const char* id) {
    if (strlen(id) > 0 && *id == '6') {
        return "SSE";
    }
    else {
        return "SZE";
    }
}

bool isValidInstID(std::string instID, std::string exchangeID) {

    if ((instID.length() <= 0) || (exchangeID.length() <= 0))
        return false;

    char c = instID[0];
    if ((exchangeID.compare("SSE") == 0) && (c != '6')) {
        return false;
    }
    else if ((exchangeID.compare("SZE") == 0) && (c != '0') && (c != '3')) {
        return false;
    }

    return true;
}

std::string getLocalTimeStr(std::string formatStr) {
    char tmp[64];

    time_t time_seconds = time(0);

    struct tm now_time;
#ifdef _WIN32
    localtime_s(&now_time, &time_seconds);
#else
    localtime_r(&time_seconds, &now_time);    // the function in Util.cpp has to be thread safe
#endif
    strftime(tmp, sizeof(tmp), formatStr.c_str(), &now_time);

    return std::string(tmp);
}

std::string getLocalTimeValStr(time_t time_seconds, std::string formatStr) {
    char tmp[64];
    struct tm now_time;
    time_t nt = time(NULL);
#ifdef _WIN32

    localtime_s(&now_time, &nt);
#else
    localtime_r(&time_seconds, &now_time);    // the function in Util.cpp has to be thread safe

#endif // 
    strftime(tmp, sizeof(tmp), formatStr.c_str(), &now_time);
    return std::string(tmp);
}

char** CreateInstArray(const std::vector<std::string>& vctInstrument) {
    char** pArray = NULL;

    if (vctInstrument.empty())
        return pArray;

    size_t iCount = vctInstrument.size();
    pArray = new char* [iCount];
    for (size_t i = 0; i < iCount; i++) {
        const char* pInstrument = vctInstrument[i].c_str();
        pArray[i] = new char[strlen(pInstrument) + 1];

        strcpy(pArray[i], pInstrument);
    }
    return pArray;
}

void DestroyInstArray(char** pArray, size_t uiArray) {
    if (pArray) {
        for (size_t ui = 0; ui < uiArray; ui++)
            delete[] pArray[ui];
        delete[] pArray;
    }
}

std::string bin2hex(std::string& _in) {
    std::string result;
    const char hexdig[] = "0123456789abcdef";

    if (_in.empty()) {
        return result;
    }

    result.clear();
    for (std::string::iterator i = _in.begin(); i != _in.end(); i++) {
        result.append(1, hexdig[(*i >> 4) & 0xf]);
        result.append(1, hexdig[(*i & 0xf)]);
    }
    return result;
}

std::string parseHsDateTimeToStr(long int time, char* format)
{
    return "";
}

std::string parseHsDateToStr(long int date, char* format)
{
    int year, mon, day;
    int rest = date;
    year = rest / 10000;
    rest = rest - (year * 10000);
    mon = rest / 100;
    rest = rest - (mon * 100);
    day = rest;

    std::stringstream strStream;
    strStream << year << "-";
    if (mon < 10)
        strStream << "0";
    strStream << mon << "-";
    if (day < 10)
        strStream << "0";
    strStream << day;
    return strStream.str();
}

uint64_t parseHsTimeToPosixTime(long int t)
{
    if (t >= 1000000)
    {
        int hour, min, sec, msec;
        int rest = t;
        hour = rest / 10000000;
        rest = rest - (hour * 10000000);
        min = rest / 100000;
        rest = rest - (min * 100000);
        sec = rest / 1000;
        rest = rest - (sec * 1000);
        msec = rest;

        struct tm _tm;
        time_t tm_t(time(NULL));
        memcpy(&_tm, localtime(&tm_t), sizeof(_tm));

        _tm.tm_hour = hour;
        _tm.tm_min = min;
        _tm.tm_sec = sec;

        return (mktime(&_tm) * 1000) + msec;
    }

    int hour, min, sec;
    int rest = t;
    hour = rest / 10000;
    rest = rest - (hour * 10000);
    min = rest / 100;
    rest = rest - (min * 100);
    sec = rest;

    struct tm _tm;
    time_t tm_t(time(NULL));
    memcpy(&_tm, localtime(&tm_t), sizeof(_tm));

    _tm.tm_hour = hour;
    _tm.tm_min = min;
    _tm.tm_sec = sec;

    return mktime(&_tm) * 1000;
}

std::string parseHsTimeToStr(long int time, char* format)
{
    if (time >= 1000000)
    {
        int hour, min, sec, msec;
        int rest = time;
        hour = rest / 10000000;
        rest = rest - (hour * 10000000);
        min = rest / 100000;
        rest = rest - (min * 100000);
        sec = rest / 1000;
        rest = rest - (sec * 1000);
        msec = rest;


        std::stringstream strStream;
        if (hour < 10)
            strStream << "0";
        strStream << hour << ":";
        if (min < 10)
            strStream << "0";
        strStream << min << ":";
        if (sec < 10)
            strStream << "0";
        strStream << sec << ".";
        if (msec < 100)
            strStream << "0";
        if (msec < 10)
            strStream << "0";
        strStream << msec;

        return strStream.str();
    }

    int hour, min, sec;
    int rest = time;
    hour = rest / 10000;
    rest = rest - (hour * 10000);
    min = rest / 100;
    rest = rest - (min * 100);
    sec = rest;

    std::stringstream strStream;
    if (hour < 10)
        strStream << "0";
    strStream << hour << ":";
    if (min < 10)
        strStream << "0";
    strStream << min << ":";
    if (sec < 10)
        strStream << "0";
    strStream << sec << ".000";

    return strStream.str();

}

std::string StringToUTF8Encoding(std::string src, std::string srcEncoding)
{
    std::wstring_convert<chs_codecvt> codec(new chs_codecvt(srcEncoding));
    std::wstring srcWstring = codec.from_bytes(src.c_str());

    std::wstring_convert<std::codecvt_utf8<wchar_t>>wcv;
    std::string ret = wcv.to_bytes(srcWstring);
    return ret;
}

std::string GetLocalDateStr()
{
    std::stringstream ss;
    ss << GetLocalDateInt();
    return ss.str();
}
std::string GetLocalTimeStr()
{
    std::stringstream ss;
    ss << GetLocalTimeInt();
    return ss.str();
}
int GetLocalDateInt()
{
    time_t tt;
    struct tm t;
    tt = time(NULL);
#ifdef _WIN32
    t = *localtime(&tt);
#else
    localtime_r(&tt, &t);
#endif
    return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday;

}
int GetLocalTimeInt()
{
    time_t tt;
    struct tm t;
    tt = time(NULL);
#ifdef _WIN32
    t = *localtime(&tt);
#else
    localtime_r(&tt, &t);
#endif
    return (t.tm_hour * 10000) + t.tm_min * 100 + t.tm_sec;
}

double GetLocalDateTimeMsec()
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    time_t tt;
    struct tm t;
    tt =time(NULL);
    localtime_r(&tt,&t);
    uint64_t res = (uint64_t )((t.tm_year+1900)*10000+(t.tm_mon+1)*100+t.tm_mday)*1000000;
    res += (t.tm_hour*10000+t.tm_min*100+t.tm_sec);
    double r = res*1.0 + tv.tv_usec*1.0/1000000.0;
    return  r;
}

std::string getTimeString(){
    std::stringstream ss;
    struct timeval tv;
    gettimeofday(&tv,NULL);
    time_t tt;
    struct tm t;
    tt =time(NULL);
    localtime_r(&tt,&t);

    ss<<(t.tm_year+1900)<<"-"<<(t.tm_mon+1)<<"-"<<t.tm_mday<<" "<<t.tm_hour<<":"<<t.tm_min<<":"<<t.tm_sec<<"."<<tv.tv_usec;

    return ss.str();
}

int GenRandom(int start, int end)
{
    int max = start > end ? start : end;
    int min = start <= end ? start : end;
    srand((unsigned)time(NULL));
    return rand() % (max - min + 1) + min;
}

#ifdef _WIN32
LPCWSTR stringToLPCWSTR(std::string orig)
{
	size_t origsize = orig.length() + 1;
	const size_t newsize = 100;
	size_t convertedChars = 0;
	wchar_t *wcstring = (wchar_t *)malloc(sizeof(wchar_t)*(orig.length() - 1));
	mbstowcs_s(&convertedChars, wcstring, origsize, orig.c_str(), _TRUNCATE);
	return wcstring;
}
#endif // _WIN32

std::string getFullStockCode(std::string stock_code, int exchange_type) {
    std::stringstream ss;
    ss << stock_code;
    if (exchange_type == 1) {
        ss << ".SH";
    }
    else if (exchange_type == 2) {
        ss << ".SZ";
    }

    return ss.str();
}

long long getLocalTimestamp() {
	auto timeNow = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
	return timeNow.count();

}

std::string getLocalTimesStr(std::string format)
{
    char tmp[64];

    time_t time_seconds = time(0);

    struct tm now_time;
#ifdef _WIN32
    now_time = *localtime(&time_seconds);
#else
    localtime_r(&time_seconds, &now_time);    // the function in Util.cpp has to be thread safe
#endif
    strftime(tmp, sizeof(tmp), format.c_str(), &now_time);
    std::string td =  std::string(tmp);
    std::stringstream ss;
    int ts = getLocalTimestamp();
    int ms = ts%1000;
    ss<<td<<"."<<ms;
    return ss.str();
}
std::string UTF8String2GBK(std::string content) {
    std::wstring_convert<std::codecvt_utf8<wchar_t>> wcv;
    std::wstring src = wcv.from_bytes(content);
    std::wstring_convert<chs_codecvt> gbk(new chs_codecvt("zh_CN.GBK"));    //GBK - whar
    std::string ret = gbk.to_bytes(src);

	return ret;
}

bool isToday(std::string date){
    std::stringstream ss;
    time_t tt;
    struct tm t;
    tt = time(NULL);
#ifdef _WIN32
    t = *localtime(&tt);
#else
    localtime_r(&tt, &t);
#endif
    char tmp[64];
    strftime(tmp,64,"%Y-%m-%d",&t);
    return strcmp(tmp,date.c_str()) == 0;
}

bool isTodayTimeStamp(long timestamp){
    time_t ts = timestamp;
    struct tm ts_tm;

    time_t tt;
    struct tm t;
    tt = time(NULL);
#ifdef _WIN32
    t = *localtime(&tt);
#else
    localtime_r(&tt, &t);
    localtime_r(&ts, &ts_tm);
#endif

    return t.tm_year == ts_tm.tm_year && t.tm_mon == ts_tm.tm_mon && t.tm_mday == ts_tm.tm_mday;
}

void StrcopyTrim(char* dst,const char* src,size_t len){
    size_t src_len = strlen(src);
    if(len < src_len){
        memcpy(dst,src,len);
    }else{
        strcpy(dst,src);
    }
}

std::string replaceString(std::string strSrc,std::string strPat,std::string strReplace){
    if (strSrc.empty())
        return strSrc;

    std::string::size_type size_pos = 0;
    while ((size_pos = strSrc.find_first_of(strPat, 0)) != std::string::npos) {
        strSrc = strSrc.replace(size_pos,strPat.size(),strReplace);
    }
    return strSrc;
}

std::string GetUUID() {
    static std::random_device rd;
    static std::uniform_int_distribution<uint64_t> dist(0ULL, 0xFFFFFFFFFFFFFFFFULL);
    uint64_t ab = dist(rd);
    uint64_t cd = dist(rd);
    uint32_t a, b, c, d;
    std::stringstream ss;
    ab = ( ab & 0xFFFFFFFFFFFF0FFFULL ) | 0x0000000000004000ULL;
    cd = ( cd & 0x3FFFFFFFFFFFFFFFULL ) | 0x8000000000000000ULL;
    a  = ( ab >> 32U );
    b  = ( ab & 0xFFFFFFFFU);
    c  = ( cd >> 32U );
    d  = ( cd & 0xFFFFFFFFU);
    ss << std::hex << std::nouppercase << std::setfill('0');
    ss << std::setw(8) << (a) << '-';
    ss << std::setw(4) << (b >> 16U) << '-';
    ss << std::setw(4) << (b & 0xFFFFU) << '-';
    ss << std::setw(4) << (c >> 16U) << '-';
    ss << std::setw(4) << (c & 0xFFFFU);
    ss << std::setw(8) << d;
    return ss.str();
}

std::string joinVec(std::vector<std::string>* vec, std::string start, std::string sep, std::string end, std::string delimiter) {
    typename std::vector<std::string>::iterator it;
    std::ostringstream os;
    os << start;
    for (it=vec->begin();it!=vec->end();++it) {
        os << delimiter << *it << delimiter;
        if (it != vec->end() - 1) {
            os << sep;
        }
    }
    os<<end;
    return os.str();
}

std::string time2String(long stamp) {
    std::chrono::time_point<std::chrono::system_clock> timePoint((std::chrono::seconds(stamp)));
    std::time_t time = std::chrono::system_clock::to_time_t(timePoint);
    struct tm stamp_tm;
    localtime_r(&time, &stamp_tm);
    std::ostringstream os;
    os << std::put_time(&stamp_tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}


std::string joinDateList(std::list<QuantLib::Date>* list, std::string start, std::string sep, std::string end, std::string fmt) {
    typename std::list<QuantLib::Date>::iterator it;
    std::ostringstream os;
    os << start;
    for (it=list->begin();it!=list->end();++it) {
        os << "'"<< QuantLib::io::formatted_date(*it, fmt) << "'";
        if (*it != list->back()) {
            os << sep;
        }
    }
    os<<end;
    return os.str();
}

std::string joinDateVec(std::vector<QuantLib::Date>* vec, std::string start, std::string sep, std::string end, std::string fmt) {
    typename std::vector<QuantLib::Date>::iterator it;
    std::ostringstream os;
    os << start;
    for (it=vec->begin();it!=vec->end();++it) {
        os << "'"<< QuantLib::io::formatted_date(*it, fmt) << "'";
        if (it != vec->end()-1) {
            os << sep;
        }
    }
    os<<end;
    return os.str();
}


time_t convertTimeStr2TimeStamp(std::string& timeStr){
    struct tm timeinfo;
    strptime(timeStr.c_str(), "%Y%m%d %H:%M:%S",  &timeinfo);
    time_t timeStamp = mktime(&timeinfo);
    return timeStamp*1000;
}

time_t getTimeStamp(std::string& date){
    std::stringstream ss;
    ss<<date<<" 23:59:59";
    std::string content = ss.str();

//    return 1680763808000;
    return convertTimeStr2TimeStamp(content);
}

time_t getTimestampBeforeOpen(std::string& date){
    std::stringstream ss;
    ss<<date<<" 09:30:00";
    std::string content = ss.str();

//    return 1680763808000;
    return convertTimeStr2TimeStamp(content);
}

time_t getTimestampAfterClose(std::string& date) {
    std::stringstream ss;
    ss<<date<<" 15:00:00";
    std::string content = ss.str();

//    return 1680763808000;
    return convertTimeStr2TimeStamp(content);
}

void mapValueToArray(std::unordered_map<std::string, double> *map, double *arr) {
    int i = 0;
    for (const auto &item : *map) {
        arr[i++] = item.second;
    }
}

int compareDoubleValue(double value1,double value2) {
    if (fabs(value1 - value2) < 1e-12) {
        return 0;
    } else if (value1 > value2 + 1e-12) {
        return 1;
    } else {
        return -1;
    }
}
std::string toUpper(std::string str) {
    std::string result(str);

    for(std::string::iterator it = result.begin(); it != result.end(); it++)
        *it = std::toupper(*it);
    return result;
}

std::string toLower(std::string str) {
    std::string result(str);

    for(std::string::iterator it = result.begin(); it != result.end(); it++)
        *it = std::tolower(*it);
    return result;
}

std::string getFirstNumberSubstr(const std::string& str) {
    size_t numStart = str.find_first_of("0123456789");
    if (numStart == std::string::npos) {
        return "";
    }
    size_t nonNumStart = str.find_first_not_of("0123456789", numStart);
    
    return str.substr(numStart, nonNumStart - numStart);
}
