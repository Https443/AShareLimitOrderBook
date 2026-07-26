#ifndef MARKETDATA_MDTOOLS_H
#define MARKETDATA_MDTOOLS_H
#include <vector>
#include <string>
#include <sstream>
#include "MarketDataStruct.h"
#include <functional>

std::vector<std::string> findDirectories(std::string path);
std::vector<std::string> findParquetFiles(std::string path);
std::vector<std::string> findFiles(std::string path, std::string suffix);
std::vector<std::string> findCsvFiles(std::string path);

// 组合两个哈希值的辅助函数
template <class T>
inline void hash_combine(std::size_t& seed, const T& v)
{
    std::hash<T> hasher;
    seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

// 计算两个值的组合哈希值
template <typename T1, typename T2>
inline size_t combine_hash(const T1& val1, const T2& val2)
{
    size_t seed = 0;
    hash_combine(seed, val1);
    hash_combine(seed, val2);
    return seed;
}

inline size_t getNearMemoryPageSize(size_t &size)
{
    const size_t originSize = 4 * 1024 * 8;
    uint64_t t = 1;
    size_t setSize = originSize * t;
    while (setSize < size)
    {
        t++;
        setSize = originSize * t;
    }
    return setSize;
}

// 将固定长度的证券代码字段（可能无'\0'终止）转换为可用于map key/对比的字符串
// - 去掉尾部的'\0'与空格
// - 保留中间的任意字符（不做trim-left，不做校验）
inline std::string normalizeSecurityCode(const char* code, size_t maxLen = marketdata::ConstField::kRSecurityCodeLen)
{
    if (!code || maxLen == 0) return {};
    size_t n = maxLen;
    while (n > 0 && (code[n - 1] == '\0' || code[n - 1] == ' '))
        --n;
    return std::string(code, n);
}

std::string streamOrderData(const marketdata::Order *data);
std::string streamOrderCsv(const marketdata::Order *data);
std::string streamTradeData(const marketdata::Trade *data);
std::string streamTradeCsv(const marketdata::Trade *data);
std::string streamSnapshotData(const marketdata::MDSnapshot *data);
std::string streamSnapshotCsv(const marketdata::MDSnapshot *data);

void printOrder(const marketdata::Order *data);
void printOrderCsv(const marketdata::Order *data);
void printTrade(const marketdata::Trade *data);
void printTradeCsv(const marketdata::Trade *data);
void printSnapshot(const marketdata::MDSnapshot *data);
void printSnapshotCsv(const marketdata::MDSnapshot *data);

#endif
