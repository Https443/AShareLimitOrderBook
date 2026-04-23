#ifndef COMMON_TRADING_DAY_CALENDAR_H
#define COMMON_TRADING_DAY_CALENDAR_H

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

class TradingDayCalendar
{
public:
    explicit TradingDayCalendar(std::string csvPath = "");

    void setCsvPath(std::string csvPath);

    void loadAllTradingDays();

    const std::vector<std::string> &getTradingDays() const;

    bool isTradingDay(const std::string &tradingDay) const;

    std::string getPreviousTradingDay(const std::string &tradingDay) const;

    const std::string &getLoadedCsvPath() const;

    static std::string normalizeTradingDay(const std::string &tradingDay);

private:
    std::filesystem::path m_csvPath;
    std::string m_loadedCsvPath;
    std::vector<std::string> m_tradingDays;
    std::unordered_set<std::string> m_tradingDaySet;
};

#endif
