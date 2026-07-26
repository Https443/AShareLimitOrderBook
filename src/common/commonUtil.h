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

// HHMMSSsss+pinMs返回新的HHMMSSsss
inline int64_t addXMilliseconds(int64_t originalTime, int pinMs)
{
    // 从 HHMMSSsssnnnnnn 提取 HHMMSSsss 部分
    int64_t timeOnly = originalTime;
    
    // 提取小时、分钟、秒和毫秒
    int64_t hours = timeOnly / 10000000L;          // HH
    int64_t minutes = (timeOnly % 10000000L) / 100000L;  // MM
    int64_t seconds = (timeOnly % 100000L) / 1000L;     // SS
    int64_t milliseconds = timeOnly % 1000L;          // sss
    
    int64_t totalMilliseconds = hours * 3600000L + minutes * 60000L + seconds * 1000L + milliseconds;
    totalMilliseconds += pinMs;  // 加x毫秒

    const int64_t kMsPerDay = 24LL * 60L * 60L * 1000L;
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

    return newHours * 10000000 + newMinutes * 100000 + newSeconds * 1000 + newMs;
}

long getMarketDataTime(long originalTime);
void parse_csv_line(const std::string &line, std::vector<std::string> &data);

class ThreadSafeDateCsvWriter {
private:
    std::string m_filename;
    std::ofstream m_file;
    mutable std::mutex m_mutex;
    bool m_isHeaderWritten;

public:
    explicit ThreadSafeDateCsvWriter(const std::string& filename)
        : m_filename(filename), m_isHeaderWritten(false)
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
        if (!m_isHeaderWritten) {
            m_file << header << std::endl;
            m_file.flush(); // 确保立即写入磁盘
            m_isHeaderWritten = true;
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
