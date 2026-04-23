#ifndef COMMONUTIL_H
#define COMMONUTIL_H
#include <vector>
#include <regex>
#include "util/Config.h"
#include "util/CommonUtil.h"
#include "util/StdException.h"
#include <fstream>
#include <string>
#include <mutex>
#include <sstream>
#include <iostream>
#include <cstdint>

int compareDouble(double value1, double value2);
bool isNaNOrInf(double value);

// YYYYMMDDHHMMSSsss获取YYYYMMDDHHMMSSsss+pinMs返回新的YYYYMMDDHHMMSSsss
inline int64_t addXMilliseconds(long originalTime, int pinMs, bool is_time = false)
{
    int64_t timeOnly = 0;
    int64_t datePart = 0;
    if (is_time)
    {
        timeOnly = originalTime;
    }
    else
    {
        // 从 YYYYMMDDHHMMSSsss 提取 HHMMSSsss 部分
        timeOnly = originalTime % 1000000000;  // 取后9位 HHMMSSsss
        datePart = originalTime / 1000000000;  // 取前8位 YYYYMMDD
    }
    
    // 提取小时、分钟、秒和毫秒
    int64_t hours = timeOnly / 10000000;          // HH
    int64_t minutes = (timeOnly % 10000000) / 100000;  // MM
    int64_t seconds = (timeOnly % 100000) / 1000;     // SS
    int64_t milliseconds = timeOnly % 1000;          // sss
    
    int64_t totalMilliseconds = hours * 3600000 + minutes * 60000 + seconds * 1000 + milliseconds;
    totalMilliseconds += pinMs;  // 加x毫秒

    const int64_t kMsPerDay = 24LL * 60 * 60 * 1000;
    int64_t dayOffset = 0;
    if (totalMilliseconds >= 0)
    {
        dayOffset = totalMilliseconds / kMsPerDay;
    }
    else
    {
        dayOffset = -(((-totalMilliseconds) + kMsPerDay - 1) / kMsPerDay);
    }

    int64_t dayMs = totalMilliseconds - dayOffset * kMsPerDay;
    if (dayMs < 0)
    {
        dayMs += kMsPerDay;
        --dayOffset;
    }

    int64_t newHours = dayMs / 3600000;
    int64_t remaining = dayMs % 3600000;
    int64_t newMinutes = remaining / 60000;
    remaining = remaining % 60000;
    int64_t newSeconds = remaining / 1000;
    int64_t newMs = remaining % 1000;

    if (is_time)
    {
        return newHours * 10000000 + newMinutes * 100000 + newSeconds * 1000 + newMs;
    }
    else
    {
        struct CivilDate
        {
            int y;
            unsigned m;
            unsigned d;
        };

        auto daysFromCivil = [](int y, unsigned m, unsigned d) -> int64_t
        {
            y -= m <= 2;
            const int era = (y >= 0 ? y : y - 399) / 400;
            const unsigned yoe = static_cast<unsigned>(y - era * 400);
            const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
            const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
            return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
        };

        auto civilFromDays = [](int64_t z) -> CivilDate
        {
            z += 719468;
            const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
            const unsigned doe = static_cast<unsigned>(z - era * 146097);
            const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
            int y = static_cast<int>(yoe) + static_cast<int>(era * 400);
            const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
            const unsigned mp = (5 * doy + 2) / 153;
            const unsigned d = doy - (153 * mp + 2) / 5 + 1;
            const unsigned m = mp + (mp < 10 ? 3 : -9);
            y += (m <= 2);
            return {y, m, d};
        };

        const int year = static_cast<int>(datePart / 10000);
        const unsigned month = static_cast<unsigned>((datePart / 100) % 100);
        const unsigned day = static_cast<unsigned>(datePart % 100);

        int64_t days = daysFromCivil(year, month, day) + dayOffset;
        CivilDate newDate = civilFromDays(days);
        int64_t newDatePart = static_cast<int64_t>(newDate.y) * 10000 + static_cast<int64_t>(newDate.m) * 100 + static_cast<int64_t>(newDate.d);

        return newDatePart * 1000000000 + newHours * 10000000 + newMinutes * 100000 + newSeconds * 1000 + newMs;
    }
}

long getMarketDataTime(long originalTime);
void parse_csv_line(const std::string &line, std::vector<std::string> &data);

class ThreadSafeDateCsvWriter {
private:
    std::string m_filename;
    std::ofstream m_file;
    mutable std::mutex m_mutex;
    bool m_is_header_written;

public:
    explicit ThreadSafeDateCsvWriter(const std::string& filename)
        : m_filename(filename), m_is_header_written(false)
    {
        m_file.open(m_filename, std::ios_base::out | std::ios_base::trunc);
        if (!m_file.is_open()) {
            STDTHROW(STD_ERROR_CODE,"Cannot open file: " << m_filename, "Cannot open file: " << m_filename);
        }
    }
    ~ThreadSafeDateCsvWriter()
    {
        if (m_file.is_open()) {
            m_file.close();
        }
    }
    
    // 禁止拷贝构造和赋值操作符
    ThreadSafeDateCsvWriter(const ThreadSafeDateCsvWriter&) = delete;
    ThreadSafeDateCsvWriter& operator=(const ThreadSafeDateCsvWriter&) = delete;
    
    /**
     * 写入data数据头到CSV文件
     * @param header header字符串
     */
    inline bool writeHeader(const std::string& header)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
    
        if (!m_file.is_open()) {
            return false;
        }
        
        // 如果还没有写入头部，则先写入头部
        if (!m_is_header_written) {
            m_file << header << std::endl;
            m_file.flush(); // 确保立即写入磁盘
            m_is_header_written = true;
            return !m_file.fail();
        }
        else
        {
            return false;
        }
    }

    /**
     * 写入data数据到CSV文件
     * @param date data字符串
     */
    inline bool write(const std::string& data)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
    
        if (!m_file.is_open()) {
            return false;
        }
        
        m_file << data << std::endl;
        m_file.flush(); // 确保立即写入磁盘
        
        // 检查写入是否成功
        return !m_file.fail();
    }
    
    /**
     * 批量写入多个data到CSV文件
     * @param dates data字符串向量
     */
    inline bool writeDates(const std::vector<std::string>& datas)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
    
        if (!m_file.is_open()) {
            return false;
        }

        for (const auto& data : datas) {
            m_file << data << std::endl;
        }
        m_file.flush(); // 确保立即写入磁盘
        
        // 检查写入是否成功
        return !m_file.fail();
    }
    
    /**
     * 检查文件是否成功打开
     */
    inline bool isFileOpen() const
    {
        return m_file.is_open();
    }
};

#endif
