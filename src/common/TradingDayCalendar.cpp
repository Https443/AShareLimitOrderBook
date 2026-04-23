#include "TradingDayCalendar.h"

#include "util/StdException.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace
{
    std::string trimWhitespace(const std::string &text)
    {
        size_t begin = 0;
        while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])))
        {
            ++begin;
        }

        size_t end = text.size();
        while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        {
            --end;
        }

        return text.substr(begin, end - begin);
    }
}

TradingDayCalendar::TradingDayCalendar(std::string csvPath)
{
    setCsvPath(std::move(csvPath));
}

void TradingDayCalendar::setCsvPath(std::string csvPath)
{
    m_csvPath = std::move(csvPath);
}

void TradingDayCalendar::loadAllTradingDays()
{
    STDTHROWIF(
        m_csvPath.empty(),
        STD_ERROR_CODE,
        "trading day csv path is empty",
        "csvPath is empty");

    std::ifstream input(m_csvPath);
    STDTHROWIF(
        !input.is_open(),
        STD_ERROR_CODE,
        "cannot open trading day csv: " << m_csvPath.string(),
        "csvPath=" << m_csvPath.string());

    m_tradingDays.clear();
    m_tradingDaySet.clear();
    m_loadedCsvPath = m_csvPath.string();

    std::string line;
    int lineNumber = 0;
    while (std::getline(input, line))
    {
        ++lineNumber;
        const std::string trimmed = trimWhitespace(line);
        if (trimmed.empty() || trimmed[0] == '#')
        {
            continue;
        }

        if (lineNumber == 1 && trimmed.find_first_of("0123456789") == std::string::npos)
        {
            continue;
        }

        const std::string tradingDay = normalizeTradingDay(trimmed);
        if (m_tradingDaySet.insert(tradingDay).second)
        {
            m_tradingDays.push_back(tradingDay);
        }
    }

    std::sort(m_tradingDays.begin(), m_tradingDays.end());
    STDTHROWIF(
        m_tradingDays.empty(),
        STD_ERROR_CODE,
        "trading day csv has no valid trading day: " << m_csvPath.string(),
        "csvPath=" << m_csvPath.string());
}

const std::vector<std::string> &TradingDayCalendar::getTradingDays() const
{
    return m_tradingDays;
}

bool TradingDayCalendar::isTradingDay(const std::string &tradingDay) const
{
    const std::string normalizedTradingDay = normalizeTradingDay(tradingDay);
    return m_tradingDaySet.find(normalizedTradingDay) != m_tradingDaySet.end();
}

std::string TradingDayCalendar::getPreviousTradingDay(const std::string &tradingDay) const
{
    if (m_tradingDays.empty())
    {
        return "";
    }

    const std::string normalizedTradingDay = normalizeTradingDay(tradingDay);
    auto it = std::lower_bound(m_tradingDays.begin(), m_tradingDays.end(), normalizedTradingDay);
    if (it == m_tradingDays.begin())
    {
        return "";
    }

    --it;
    return *it;
}

const std::string &TradingDayCalendar::getLoadedCsvPath() const
{
    return m_loadedCsvPath;
}

std::string TradingDayCalendar::normalizeTradingDay(const std::string &tradingDay)
{
    std::string normalized = trimWhitespace(tradingDay);
    if (normalized.size() == 10 && normalized[4] == '-' && normalized[7] == '-')
    {
        normalized.erase(7, 1);
        normalized.erase(4, 1);
    }

    STDTHROWIF(
        normalized.size() != 8,
        STD_ERROR_CODE,
        "invalid trading day format: " << tradingDay,
        "tradingDay=" << tradingDay << ", normalized=" << normalized);

    STDTHROWIF(
        !std::all_of(normalized.begin(), normalized.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; }),
        STD_ERROR_CODE,
        "invalid trading day format: " << tradingDay,
        "tradingDay=" << tradingDay << ", normalized=" << normalized);

    return normalized;
}
