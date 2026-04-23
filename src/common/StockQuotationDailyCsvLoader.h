#ifndef COMMON_STOCKQUOTATIONDAILYCSVLOADER_H
#define COMMON_STOCKQUOTATIONDAILYCSVLOADER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct StockQuotationDailyRecord
{
    std::string symbol;              // 证券代码
    std::string tradingDay;          // 交易日期，格式 YYYYMMDD
    int64_t preClosePrice;           // 昨收盘(交易所)，实际值需除以1000000
    int64_t openPrice;               // 开盘价，实际值需除以1000000
    int64_t closePrice;              // 收盘价，实际值需除以1000000
    int64_t highPrice;               // 最高价，实际值需除以1000000
    int64_t lowPrice;                // 最低价，实际值需除以1000000
    int64_t volume;                  // 成交量
    int64_t amount;                  // 成交金额，实际值需除以1000000
    int64_t avgPrice;                // 均价，实际值需除以1000000
    double change;                   // 涨跌
    double changeRatio;              // 涨跌幅
    int64_t totalShare;              // 总股本
    int64_t circulatedShare;         // 流通股本
    double turnoverRate1;            // 换手率
    double turnoverRate2;            // 换手率（基准.自由流通股本）
    int64_t marketValue;             // 总市值，实际值需除以1000000
    int64_t circulatedMarketValue;   // 流通市值，实际值需除以1000000
    double amplitude;                // 振幅
    int64_t limitDown;               // 跌停价格，实际值需除以1000000
    int64_t limitUp;                 // 涨停价，实际值需除以1000000
    std::string limitStatus;         // 涨跌停状态
};

class StockQuotationDailyCsvLoader
{
public:
    explicit StockQuotationDailyCsvLoader(std::string csvRootPath);

    void loadByDate(const std::string &tradingDay);

    const std::vector<StockQuotationDailyRecord> &getRecords() const;

    const StockQuotationDailyRecord *getRecord(const std::string &symbol) const;

    const std::string &getLoadedTradingDay() const;

private:
    static constexpr int kExpectedFieldCount = 22;

    std::filesystem::path m_csvRootPath;
    std::string m_loadedTradingDay;
    std::vector<StockQuotationDailyRecord> m_records;
    std::unordered_map<std::string, size_t> m_symbolToIndex;

    static std::filesystem::path buildCsvPath(
        const std::filesystem::path &csvRootPath,
        const std::string &tradingDay);

    static void validateHeader(const std::vector<std::string> &headerFields);

    static StockQuotationDailyRecord parseRecord(
        const std::vector<std::string> &fields,
        const std::string &tradingDay,
        int lineNumber);
};

#endif
