#pragma once

#include <cctype>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <chrono>
#include <random>
#include <time.h>
#include "math.h"
#include <numeric>
#include "StdException.h"
#include <cstring>
#include "string.h"
#include <vector>
#include <deque>
#include <algorithm>
inline std::string GetUUID() {
    static std::random_device rd;
    static std::uniform_int_distribution<uint64_t> dist(0ULL, 0xFFFFFFFFFFFFFFFFULL);
    uint64_t ab = dist(rd);
    uint64_t cd = dist(rd);
    uint32_t a, b, c, d;
    std::stringstream ss;
    ab = (ab & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    cd = (cd & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    a = (ab >> 32U);
    b = (ab & 0xFFFFFFFFU);
    c = (cd >> 32U);
    d = (cd & 0xFFFFFFFFU);
    ss << std::hex << std::nouppercase << std::setfill('0');
    ss << std::setw(8) << (a) << '-';
    ss << std::setw(4) << (b >> 16U) << '-';
    ss << std::setw(4) << (b & 0xFFFFU) << '-';
    ss << std::setw(4) << (c >> 16U) << '-';
    ss << std::setw(4) << (c & 0xFFFFU);
    ss << std::setw(8) << d;
    return ss.str();
}

inline std::string getLocalTimeStr(std::string formatStr) {
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

inline long long getLocalTimestamp() {
    auto timeNow = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch());
    return timeNow.count();

}


inline int compareDoubleValue(double value1, double value2) {
    if (std::abs(value1 - value2) < 1e-10) {
        return 0;
    }
    else if (value1 > value2 + 1e-10) {
        return 1;
    }
    else {
        return -1;
    }
}

static const double ALGORITHM_NAN_VALUE = std::numeric_limits<double>::quiet_NaN();

/**
 * 检查股票代码是否属于上证A股或深证A股
 * 
 * 该函数通过分析股票代码的特定部分，判断其是否属于上证A股（SH）或深证A股（SZ）
 * 对于上证A股，其代码以'6'开头；对于深证A股，其代码以'3'或'0'开头
 * 
 * @param code 股票代码，格式为6位数字加上'.SH'或'.SZ'后缀
 * @return 如果股票代码属于上证A股或深证A股，则返回true，否则返回false
 */
inline bool isStockInShSzAShare(std::string& code) {
    // 检查股票代码是否属于上证A股
    if(memcmp(code.c_str()+7,"SH",2) == 0) {
        return code.c_str()[0] == '6';
    } 
    // 检查股票代码是否属于深证A股
    else if(memcmp(code.c_str()+7,"SZ",2) == 0) {
        return code.c_str()[0] == '3' || code.c_str()[0] == '0';
    }
    // 如果股票代码既不属于上证A股也不属于深证A股，则返回false
    return false;
}

/**
 * 检查股票代码是否属于上证A股或深证A股。
 * 
 * 该函数通过比较股票代码的交易所标识（SH或SZ）和相应的股票代码前缀，
 * 来确定股票是否属于上证A股或深证A股。
 * 
 * @param code 股票代码字符串，格式为"XXXXXXX.SH"或"XXXXXXX.SZ"。
 * @return 如果股票代码属于上证A股或深证A股，则返回true，否则返回false。
 */
inline bool isStockInShSzAShare(const char* code) {
    // 检查是否为上证A股，上证A股的代码以'6'开头，代码后跟着"SH"
    if(memcmp(code+7,"SH",2) == 0) {
        return code[0] == '6';
    }
    // 检查是否为深证A股，深证A股的代码以'3'或'0'开头，代码后跟着"SZ"
    else if(memcmp(code+7,"SZ",2) == 0) {
        return code[0] == '3' || code[0] == '0';
    }
    // 如果既不是上证A股也不是深证A股，则返回false
    return false;
}

/**
 * 检查给定的双精度浮点数值是否为NaN（非数字）或无穷大
 * 
 * @param value 待检查的双精度浮点数值
 * @return 如果值为NaN或无穷大，则返回true；否则返回false
 */
inline bool isNaNOrInf(double value) {
    return std::isnan(value) || std::isinf(value);
}


template <typename T>
/**
 * 计算给定vector中所有元素的和
 * 如果vector为空，则返回NaN（不是一个数字）
 * 
 * @param vec 指向包含元素的vector的指针
 * @return vector中所有元素的和，如果vector为空则返回NaN
 */
inline T getSumValue (std::vector<T> *vec) {
    // 检查vector是否为空，如果为空则返回NaN，否则计算并返回所有元素的和
    return vec->empty() ? std::numeric_limits<T>::quiet_NaN() : std::accumulate(vec->begin(), vec->end(), 0.0);
}

/**
 * 计算给定矢量中元素的平均值
 * 
 * 此函数通过计算矢量中所有元素的总和，然后除以元素的数量来求得平均值
 * 如果矢量为空，则返回NaN（不是一个数字），表示没有有效的平均值
 * 
 * @param vec 指向包含数字的矢量指针
 * @return 返回矢量中元素的平均值如果矢量为空，则返回NaN
 */
template <typename T>
inline double getAvgValue (std::vector<T> *vec) {
    // 使用空判断来防止除以零的操作，如果矢量为空，则返回NaN
    auto size = vec->size();
    if(vec->empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    return std::accumulate(vec->begin(), vec->end(), 0.0) * 1.0/ size;
}


template <typename T>
/**
 * 计算给定数值集合的标准差
 * 
 * 标准差是衡量数值集合中各数值偏离其平均值程度的统计指标此函数提供了计算标准差的功能，
 * 并允许选择是否进行偏差修正通过此函数，用户可以轻松获得数值集合的离散程度，这对于数据分析
 * 和统计研究等领域非常有用
 * 
 * @param values 指向包含数值的向量的指针如果为空或数据不足，函数将返回NaN
 * @param isBaisCorrect 一个布尔值，指示是否进行偏差修正默认为true，即进行偏差修正
 * @param numLimit 指定向量中至少应包含的元素数量，以使标准差计算有效默认值为3
 * @return 返回计算得到的标准差如果输入数据无效，则返回NaN
 */
inline double getStandardDeviation (std::vector<T> *values, bool isBaisCorrect=true,int numLimit=3) {
    // 检查输入数据的有效性，如果无效则返回NaN
    if (values == NULL || values->size() == 0 || values->size() < numLimit) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // 计算数据集的大小、总和及平均值
    int size = values->size();
    double sum = std::accumulate(values->begin(), values->end(), 0.0);
    double avg = sum / size;

    // 初始化用于计算方差的变量
    double minusAvg = 0.0;

    // 遍历数据集，计算每个数值与平均值差的平方和
    for (int k = 0; k < size; k++) {
        minusAvg += std::pow(values->at(k) - avg, 2);
    }

    // 根据是否使用偏差修正，选择不同的计算方式，并返回计算结果
    if (isBaisCorrect) {
        return std::sqrt(minusAvg / (size - 1));
    } else {
        return std::sqrt(minusAvg / size);
    }
}

template <typename T>
/**
 * 计算给定数值集合的标准差
 * 
 * @tparam T 数值集合中元素的类型，需要支持std::pow和std::sqrt运算
 * @param values 指向包含数值的向量的指针，用于计算标准差
 * @param avg 数值集合的平均值
 * @param isBaisCorrect 是否进行偏差修正，默认为true
 * @param numLimit 计算标准差所需的最小元素数量，默认为3
 * @return 返回计算得到的标准差；如果输入无效，返回NaN
 * 
 * 此函数通过计算数值集合中每个数值与平均值之差的平方的平均数，然后取其平方根来得到标准差
 * 如果isBaisCorrect参数为true，则使用N-1作为分母来计算，以修正样本标准差的偏差
 * 函数首先检查输入的有效性，包括values指针是否为NULL，向量是否为空，以及是否至少包含numLimit个元素
 * 如果输入无效，函数将返回NaN，表示结果无效
 */
inline double getStandardDeviation (std::vector<T> *values, double avg, bool isBaisCorrect=true,int numLimit=3) {
    // 检查输入有效性：values指针是否为NULL，向量是否为空，以及是否至少包含3个元素
    if (values == NULL || values->size() == 0 || values->size() < numLimit) {
        // 当输入无效时，返回NaN（非数字），表示结果无效
        return std::numeric_limits<double>::quiet_NaN();
    }
    
    // 获取数值集合的大小，用于后续计算
    int size = values->size();
    
    // 初始化用于累加每个数值与平均值之差的平方的变量
    double minusAvg = 0.0;
    
    // 遍历数值集合，计算每个数值与平均值之差的平方，并累加到minusAvg中
    for (const auto &item :*values) {
        minusAvg += std::pow(item - avg, 2);
    }
    
    // 根据isBaisCorrect参数决定使用N-1（偏差修正）还是N（无修正）作为分母
    // 返回计算得到的标准差
    if (isBaisCorrect) {
        return std::sqrt(minusAvg / (size - 1));
    } else {
        return std::sqrt(minusAvg / size);
    }
}


/**
 * 计算一组数值的中位数
 * 
 * @param values 一个指向包含数值的向量的指针
 * @return 给定数值向量的中位数
 */
template <typename T>
inline T computeMedian(std::vector<T>* values) {
    std::vector<T> vec(*values);
    std::sort(vec.begin(), vec.end(), std::less<T>());
    int length = values->size();
    if (length % 2 == 0) {
        return (vec.at(length / 2 - 1) + vec.at(length / 2)) / 2.0;
    } else {
        return vec.at(length / 2);
    }
}

template <typename T>
inline T computeMedian(std::deque<T> *values) {
     std::deque<T> vec(*values);
     std::sort(vec.begin(), vec.end(), std::less<T>());
     int length = values->size();
     if (length % 2 == 0) {
         return (vec.at(length / 2 - 1) + vec.at(length / 2)) / 2.0;
     } else {
         return vec.at(length / 2);
     }
 }

/**
 * 计算绝对中位差（Median Absolute Deviation, MAD）
 * 
 * @param values 指向包含样本值的向量的指针
 * @param median 样本值的中位数
 * @return 返回计算得到的绝对中位差
 * 
 * 此函数的目的是计算给定数据集的绝对中位差，这是一种衡量数据集变异性的统计量
 * 它表示各个数据点与数据集中位数的绝对偏差的中位数，是一种稳健的统计指标
 * 
 * 计算步骤如下：
 * 1. 对于数据集中的每个样本值，计算其与中位数的绝对差值
 * 2. 将这些绝对差值组成一个新的序列
 * 3. 计算这个新序列的中位数，即为绝对中位差（MAD）
 * 
 * 注意：此函数假定输入的中位数已经通过其他方式计算得到
 */
template <typename T>
inline double computeMad(std::vector<T> *values, T median) {
    // 创建一个向量来存储每个样本值与中位数的绝对差值
    std::vector<double> absError;
    
    // 遍历输入的样本值向量，计算每个样本值与中位数的绝对差值，并存储到absError向量中
    for (const auto &value : *values) {
        absError.push_back(std::abs(value*1.0 - median));
    }
    
    // 调用computeMedian函数计算绝对差值序列的中位数，即得到MAD
    double mad = computeMedian<double>(&absError);
    
    // 返回计算得到的MAD值
    return mad;
}

/**
 * 计算并返回经过Mad（中位数绝对偏差）限值处理后的值。
 * 
 * 此函数的目的是根据给定的中位数、Mad值、最大值、最小值以及是否需要删除异常值的标志，
 * 对输入值进行处理，以确保其落在合理的范围内。如果输入值被认为是异常值（即超出最大值或最小值范围），
 * 并且needDelete标志设置为true，则返回NaN（非数字）值。否则，将输入值限制在最小值和最大值之间。
 * 
 * @param input 待处理的输入值。
 * @param mad Mad值，用于计算但不直接使用于算法。
 * @param median 中位数值，用于计算但不直接使用于算法。
 * @param max 允许的最大值。
 * @param min 允许的最小值。
 * @param needDelete 标志，指示是否需要删除超出[min, max]范围的异常值。
 * @return 处理后的值，如果输入值为NaN或无穷大，或者被标记为异常值且needDelete为true，则返回NaN。
 */
inline double computeMadLimit(double input, double mad, double median, double max, double min, bool needDelete) {
    // 检查输入值是否为NaN或无穷大，如果是，则返回NaN值。
    if (isNaNOrInf(input)) {
        return ALGORITHM_NAN_VALUE;
    }
    // 检查输入值是否小于最小值或大于最大值。
    bool minFlag = compareDoubleValue(input, min) < 0;
    bool maxFlag = compareDoubleValue(input, max) > 0;
    // 如果输入值超出范围且needDelete标志为true，则返回NaN值。
    if ((minFlag || maxFlag) && needDelete) {
        return ALGORITHM_NAN_VALUE;
    }
    // 如果输入值小于最小值，则使用最小值；否则继续使用输入值。
    double v1 = minFlag ? min : input;
    // 如果v1大于最大值，则使用最大值，否则继续使用v1的值。
    double v2 = compareDoubleValue(v1, max) > 0 ? max : v1;
    // 返回最终处理后的值。
    return v2;
}

/**
 * 计算给定目标值在一组数据中的分位数
 * 
 * @param vector 一组双精度浮点数，代表数据序列
 * @param target 目标值，用于计算分位数
 * @param isPolate 指示是否进行插值的布尔标志
 * @return 返回目标值在数据序列中的分位数
 * 
 * 此函数首先检查目标值是否在数据序列的范围之外，如果是，则根据数据序列的大小计算并返回相应的分位数
 * 如果目标值在数据序列的范围内，则遍历数据序列，计算目标值的排名，并根据是否需要插值来计算最终的分位数
 */
template <typename T>
inline double computeFractile(std::vector<T>& vector, T target, bool isPolate=true) {
    // 获取数据序列的大小
    int size = vector.size();
    // 如果目标值小于数据序列中的最小值，则返回最小分位数
    if (compareDouble(vector.at(0), target) > 0) {
        return 1.0 / (size + 1.0);
    } else if (compareDouble(vector.at(size - 1), target) < 0) {
        // 如果目标值大于数据序列中的最大值，则返回最大分位数
        return size / (size + 1.0);
    }
    // 初始化排名和最后的排名变量
    int rank = 1;
    int lRank = 1;
    // 初始化最后一个值变量为数据序列的第一个元素
    double lastValue = vector.at(0);
    // 遍历数据序列，计算目标值的排名
    for (size_t i = 1; i < vector.size(); i++) {
        const auto v = vector.at(i);
        // 更新最后的排名为当前排名
        lRank = rank;
        // 如果当前元素与最后一个值不相等，则更新排名和最后一个值
        if (compareDouble(v, lastValue) != 0) {
            rank = i + 1;
            lastValue = vector.at(i);
        }
        // 如果当前元素等于目标值，则计算并返回当前排名的分位数
        if (compareDouble(v, target) == 0) {
            return rank * 1.0 / (size + 1.0);
        }
        // 如果目标值小于当前元素，则停止遍历
        if (compareDouble(target, v) < 0) {
            break;
        }
    }
    // 根据是否需要插值，计算并返回最终的分位数
    if (isPolate) {
        return rank * 1.0 / (size + 2.0);
    } else {
        return (lRank * 1.0 + rank * 1.0) / (2 * (size + 1));
    }
}
/**
 * 计算给定向量中特定值的分位数值。
 * 
 * 该函数首先检查目标值是否为NaN（非数字）或无穷大，以及向量是否为空。
 * 如果条件满足，则返回NaN。如果sort参数为true，则对向量进行排序。
 * 接着，函数将根据特定的算法计算并返回分位数值。
 * 
 * @param vector 待计算分位数值的双精度浮点数向量。
 * @param target 目标值，用于确定分位数的位置。
 * @param isPolate 指示是否进行插值的布尔标志。
 * @param sort 指示是否在计算前对向量进行排序的布尔标志。
 * @return 返回计算得到的分位数值。如果输入无效或向量为空，则返回NaN。
 */
template <typename T>
inline double getFractileValue(std::vector<T>& vector, T target, bool isPolate=true, bool sort=true) {
    // 检查目标值是否为NaN或无穷大，以及向量是否为空。
    if (isNaNOrInf(target) || vector.empty()) {
        // 如果条件满足，返回NaN。
        return std::numeric_limits<T>::quiet_NaN();
    }
    // 如果sort参数为true，则对向量进行排序。
    if (sort) {
        std::sort(vector.begin(), vector.end(), std::less<T>());
    }
    // 通过二分查找确定目标值在排序后向量中的位置。
    // double index = binarySearch(vector, target);
    // 根据目标值的位置和插值标志，计算并返回分位数值。
    return computeFractile<T>(vector, target, isPolate);
}
/**
 * @brief 对数据向量和索引向量进行排序
 * 
 * 本函数旨在同时对数据向量和与其关联的索引向量进行排序。排序可以根据升序或降序进行，
 * 并通过索引向量来追踪原始数据的顺序。这种排序方式对于需要同时保持数据原始位置信息和排序信息的场景特别有用。
 * 
 * @param data 包含实际数据的向量指针
 * @param index 与数据向量对应的索引向量指针
 * @param asc 指定排序方式，true为升序，false为降序
 */
template <typename T>
inline void sortVec (std::vector<T> *data, std::vector<int> *index=NULL, bool asc=true) {
    if(data->empty()) {
        return;
    }
    // 根据asc参数决定排序方式
    if (asc) {
        if(index) {
            // 升序排序索引向量，通过lambda表达式比较data中对应索引的值
            sort(index->begin(), index->end(),
                [&] (const int &a, const int &b) {
                return (data->at(a) < data->at(b));
            }
            );
        }
        // 升序排序数据向量
        sort(data->begin(), data->end(), std::less<T>());
    } else {
        if(index) {
            // 降序排序索引向量，通过lambda表达式比较data中对应索引的值
            sort(index->begin(), index->end(),
                [&] (const int &a, const int &b) {
                return (data->at(a) > data->at(b));
            }
            );
        }
        // 降序排序数据向量
        sort(data->begin(), data->end(), std::greater<T>());
    }
}

/**
 * 计算并返回一个向量的峰度值。
 * 
 * 峰度是衡量一组数据分布的尖峭程度的统计量。高峰度值表明数据分布有重尾和尖峰，而低峰度值表明数据分布较为平坦。
 * 
 * @param vector 输入的双精度浮点数向量，用于计算峰度值。
 * @return 返回计算得到的峰度值。
 */
template <typename T>
inline double getKurtValue(std::vector<T>& vector) {
    // 获取向量的大小
    double n = vector.size();
    // 计算向量的平均值
    double avg = getAvgValue<T>(&vector);
    // 计算向量的标准差
    double std = getStandardDeviation<T>(&vector, avg);
    // 初始化用于计算峰度的累加变量
    double sum = 0.0;
    // 遍历向量中的每个元素，计算峰度的累加部分
    for (const auto &item : vector) {
        sum += std::pow((item - avg) / std, 4);
    }
    // 计算峰度公式中的变量部分
    double var1 = n * (n + 1) / ((n - 1) * (n - 2) * (n - 3));
    // 计算峰度公式中的常量部分
    double var2 = 3 * std::pow(n - 1, 2) / ((n - 2) * (n - 3));
    // 返回计算得到的峰度值
    return var1 * sum - var2;
}

/**
 * 计算向量的偏度值
 * 偏度值用于衡量数据分布的不对称程度
 * @param vector 输入的向量，包含需要计算偏度值的数据
 * @return 返回向量的偏度值
 */
inline double getSkewValue(std::vector<double>& vector) {
    // 获取向量中元素的数量
    double n = vector.size();
    // 计算向量的平均值
    double avg = getAvgValue(&vector);
    // 计算向量的标准差
    double std = getStandardDeviation(&vector, avg);
    // 初始化用于计算偏度值的和
    double sum = 0.0;
    // 遍历向量中的每个元素
    for (const auto &item : vector) {
        // 计算每个元素对偏度值的贡献，并累加到总和中
        sum += std::pow((item - avg) / std, 3);
    }
    // 计算并返回偏度值
    return sum * n / ((n - 1) * (n - 2));
}
template <typename T>
inline double getSkewValue(std::vector<T>& vector,double var) {
    // 获取向量中元素的数量
    double n = vector.size();
    // 计算向量的平均值
    double avg = getAvgValue<T>(&vector);
    // 计算向量的标准差
    double std = getStandardDeviation<T>(&vector, avg);
    // 初始化用于计算偏度值的和
    double sum = 0.0;
    // 遍历向量中的每个元素
    for (const auto &item : vector) {
        // 计算每个元素对偏度值的贡献，并累加到总和中
        sum += std::pow((item - avg) / std, 3);
    }
    // 计算并返回偏度值
    return sum * var;
}

// Function to find the local extrema (maxima and minima) of a signal
inline void find_extrema(const std::vector<double>& signal, std::vector<int>& maxima_indices, std::vector<int>& minima_indices) {
    int n = signal.size();
    for (int i = 1; i < n - 1; ++i) {
        if (signal[i] > signal[i - 1] && signal[i] > signal[i + 1]) {
            maxima_indices.push_back(i);
        } else if (signal[i] < signal[i - 1] && signal[i] < signal[i + 1]) {
            minima_indices.push_back(i);
        }
    }
}

// Function to compute the mean envelope of a signal between extrema
inline std::vector<double> compute_envelope(const std::vector<double>& signal, const std::vector<int>& indices) {
    std::vector<double> envelope(signal.size(), 0.0);
    for (size_t i = 0; i < indices.size(); ++i) {
        envelope[indices[i]] = signal[indices[i]];
    }
    // Interpolate values for the envelope (for simplicity, use linear interpolation here)
    for (size_t i = 1; i < envelope.size(); ++i) {
        if (envelope[i] == 0.0) {
            size_t prev_idx = i - 1;
            while (envelope[prev_idx] == 0.0 && prev_idx >= 0) {
                --prev_idx;
            }
            if (prev_idx >= 0) {
                size_t next_idx = i;
                while (envelope[next_idx] == 0.0 && next_idx < envelope.size()) {
                    ++next_idx;
                }
                if (next_idx < envelope.size()) {
                    double slope = (envelope[next_idx] - envelope[prev_idx]) / (next_idx - prev_idx);
                    envelope[i] = envelope[prev_idx] + slope * (i - prev_idx);
                }
            }
        }
    }
    return envelope;
}

/**
 * @brief Performs Empirical Mode Decomposition (EMD) on a signal to extract Intrinsic Mode Functions (IMFs).
 * 
 * The EMD method decomposes a given signal into several IMFs and a residue, where each IMF represents a different frequency component of the signal.
 * This process is crucial for time-frequency analysis of non-stationary signals.
 * 
 * @param signal The input signal, represented as a vector of doubles.
 * @param outImfs The output IMFs, where each IMF is also a vector of doubles. This is a reference parameter, and the function will append the extracted IMFs to this vector.
 */
inline void EMD(const std::vector<double>& signal,std::vector<std::vector<double>>& outImfs,double residueLimit=1e-6,int stepLimit=-1) {
    // Initialize the residue as the input signal
    std::vector<double> residue = signal;

    // Loop until the residue meets the stopping criterion
    int count = 0;
    while (true) {
        // Find the indices of the maxima and minima in the residue
        std::vector<int> maxima_indices, minima_indices;
        find_extrema(residue, maxima_indices, minima_indices);

        // If there are no maxima or minima, stop the decomposition
        if (maxima_indices.empty() || minima_indices.empty()) {
            break;
        }

        // Compute the upper and lower envelopes of the residue
        std::vector<double> upper_envelope = compute_envelope(residue, maxima_indices);
        std::vector<double> lower_envelope = compute_envelope(residue, minima_indices);

        // Compute the local mean (m) of the residue
        std::vector<double> m(residue.size());
        for (size_t i = 0; i < residue.size(); ++i) {
            m[i] = (upper_envelope[i] + lower_envelope[i]) / 2.0;
        }

        // Compute the IMF by subtracting the local mean from the residue
        std::vector<double> imf(residue.size());
        for (size_t i = 0; i < residue.size(); ++i) {
            imf[i] = residue[i] - m[i];
        }

        // Add the computed IMF to the output list of IMFs
        outImfs.push_back(imf);
        // Update the residue for the next iteration
        residue = m;

        // Stopping criterion (e.g., when residue becomes small enough)
        double residue_energy = 0.0;
        for (double val : residue) {
            residue_energy += val * val;
        }
        // If the energy of the residue is below a threshold, stop the decomposition
        if (residue_energy < residueLimit) {
            break;
        }
        count++;
        if(stepLimit>0 && count>=stepLimit) {
            break;
        }
    }
}

inline double getDoubleMax() {
#ifdef _WIN32
    return DBL_MAX;
#else
    return std::numeric_limits<double>::max();
#endif
}

inline double minValue(double v1, double v2) {
#ifdef _WIN32
    return min(v1,v2);
#else
    return std::min(v1,v2);
#endif
}

inline double maxValue(double v1, double v2) {
#ifdef _WIN32
    return max(v1,v2);
#else
    return std::max(v1,v2);
#endif
}

inline int maxIntValue(int v1, int v2) {
#ifdef _WIN32
    return max(v1,v2);
#else
    return std::max(v1,v2);
#endif
}

inline int minIntValue(int v1, int v2) {
#ifdef _WIN32
    return min(v1,v2);
#else
    return std::min(v1,v2);
#endif
}

inline std::string formatStdCodeToStockCode(std::string stdCode) {
    if(memcmp(stdCode.c_str(),"6",1)==0) {
        return stdCode + ".SH";
    }
    return stdCode + ".SZ";
}

inline double getPearsonCorrValue(std::vector<double> &xVec,std::vector<double> &yVec) {
    double length = xVec.size();
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    double sumYY = 0.0;
    
    // ( E(XY) - E(X)E(Y) ) / (sqrt( E(X*X) - E(X) * E(X) ) * sqrt( E(Y*Y) - E(Y) * E(Y) ))
    for (int m = 0; m < length; m++) {
        double x = xVec.at(m);
        double y = yVec.at(m);
        sumX += x;
        sumY += y;

        sumXY += x * y;
        sumXX += x * x;
        sumYY += y * y;
    }

    double averageX = sumX / length;
    double averageY = sumY / length;

    double stdX = std::sqrt(sumXX / length - averageX * averageX);
    if (stdX == 0.0 ) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double stdY = std::sqrt(sumYY / length - averageY * averageY);
    if (stdY == 0.0 ) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double covXY = sumXY / length - averageX * averageY;
    return covXY / (stdX * stdY);
}