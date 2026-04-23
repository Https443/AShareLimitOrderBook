#include "StockQuotationDailyCsvLoader.h"

#include <array>
#include <cmath>
#include <fstream>
#include <limits>

#include "commonUtil.h"

namespace
{
    constexpr int64_t kDoubleScale = 1000000;

    int64_t parseIntegerField(
        const std::string &field,
        const std::string &fieldName,
        const std::string &tradingDay,
        int lineNumber)
    {
        if (field.empty())
        {
            return 0;
        }

        try
        {
            size_t pos = 0;
            const int64_t integerValue = std::stoll(field, &pos);
            if (pos == field.size())
            {
                return integerValue;
            }

            pos = 0;
            const long double decimalValue = std::stold(field, &pos);
            STDTHROWIF(
                pos != field.size(),
                STD_ERROR_CODE,
                "invalid integer field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field);

            const long double roundedValue = std::llround(decimalValue);
            STDTHROWIF(
                std::fabs(decimalValue - roundedValue) > 1e-9L,
                STD_ERROR_CODE,
                "non-integer numeric field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field);
            return static_cast<int64_t>(roundedValue);
        }
        catch (const std::exception &ex)
        {
            STDTHROW(
                STD_ERROR_CODE,
                "invalid integer field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field << ", ex=" << ex.what());
        }
    }

    int64_t parseScaledField(
        const std::string &field,
        const std::string &fieldName,
        const std::string &tradingDay,
        int lineNumber)
    {
        if (field.empty())
        {
            return 0;
        }

        try
        {
            size_t pos = 0;
            const long double value = std::stold(field, &pos);
            STDTHROWIF(
                pos != field.size(),
                STD_ERROR_CODE,
                "invalid decimal field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field);
            const long double scaled = value * static_cast<long double>(kDoubleScale);
            STDTHROWIF(
                !std::isfinite(scaled),
                STD_ERROR_CODE,
                "non-finite decimal field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field);
            STDTHROWIF(
                scaled < static_cast<long double>(std::numeric_limits<int64_t>::min()) ||
                    scaled > static_cast<long double>(std::numeric_limits<int64_t>::max()),
                STD_ERROR_CODE,
                "scaled decimal field out of int64 range " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field);
            return static_cast<int64_t>(std::llround(scaled));
        }
        catch (const std::exception &ex)
        {
            STDTHROW(
                STD_ERROR_CODE,
                "invalid decimal field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field << ", ex=" << ex.what());
        }
    }

    int64_t parseDoubleField(
        const std::string &field,
        const std::string &fieldName,
        const std::string &tradingDay,
        int lineNumber)
    {
        if (field.empty())
        {
            return 0;
        }

        try
        {
            size_t pos = 0;
            const double value = std::stod(field, &pos);
            STDTHROWIF(
                pos != field.size(),
                STD_ERROR_CODE,
                "invalid double field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field);
            return value;
        }
        catch (const std::exception &ex)
        {
            STDTHROW(
                STD_ERROR_CODE,
                "invalid decimal field " << fieldName << " at line " << lineNumber,
                "tradingDay=" << tradingDay << ", fieldName=" << fieldName
                               << ", rawField=" << field << ", ex=" << ex.what());
        }
    }
}

StockQuotationDailyCsvLoader::StockQuotationDailyCsvLoader(std::string csvRootPath)
    : m_csvRootPath(std::move(csvRootPath))
{
}

void StockQuotationDailyCsvLoader::loadByDate(const std::string &tradingDay)
{
    const std::filesystem::path csvPath = buildCsvPath(m_csvRootPath, tradingDay);

    std::ifstream input(csvPath);
    STDTHROWIF(
        !input.is_open(),
        STD_ERROR_CODE,
        "cannot open csv file: " << csvPath.string(),
        "tradingDay=" << tradingDay << ", csvPath=" << csvPath.string());

    m_records.clear();
    m_symbolToIndex.clear();
    m_loadedTradingDay = tradingDay;

    std::string line;
    int lineNumber = 0;
    bool headerLoaded = false;
    std::vector<std::string> fields;

    while (std::getline(input, line))
    {
        ++lineNumber;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        if (line.empty())
        {
            continue;
        }

        parse_csv_line(line, fields);
        if (!headerLoaded)
        {
            validateHeader(fields);
            headerLoaded = true;
            continue;
        }

        StockQuotationDailyRecord record = parseRecord(fields, tradingDay, lineNumber);
        if (record.volume == 0) continue;
        const auto [it, inserted] = m_symbolToIndex.emplace(record.symbol, m_records.size());
        STDTHROWIF(
            !inserted,
            STD_ERROR_CODE,
            "duplicate symbol in csv: " << record.symbol,
            "tradingDay=" << tradingDay << ", symbol=" << record.symbol
                           << ", csvPath=" << csvPath.string());
        m_records.emplace_back(std::move(record));
        (void)it;
    }

    STDTHROWIF(
        !headerLoaded,
        STD_ERROR_CODE,
        "csv file has no header: " << csvPath.string(),
        "tradingDay=" << tradingDay << ", csvPath=" << csvPath.string());
}

const std::vector<StockQuotationDailyRecord> &StockQuotationDailyCsvLoader::getRecords() const
{
    return m_records;
}

const StockQuotationDailyRecord *StockQuotationDailyCsvLoader::getRecord(const std::string &symbol) const
{
    const auto it = m_symbolToIndex.find(symbol);
    if (it == m_symbolToIndex.end())
    {
        return nullptr;
    }
    return &m_records[it->second];
}

const std::string &StockQuotationDailyCsvLoader::getLoadedTradingDay() const
{
    return m_loadedTradingDay;
}

std::filesystem::path StockQuotationDailyCsvLoader::buildCsvPath(
    const std::filesystem::path &csvRootPath,
    const std::string &tradingDay)
{
    return csvRootPath / (tradingDay + ".csv");
}

void StockQuotationDailyCsvLoader::validateHeader(const std::vector<std::string> &headerFields)
{
    static const std::array<std::string, kExpectedFieldCount> expectedHeader = {
        "SYMBOL",
        "TRADING_DAY",
        "PRECLOSEPRICE",
        "OPENPRICE",
        "CLOSEPRICE",
        "HIGHPRICE",
        "LOWPRICE",
        "VOLUME",
        "AMOUNT",
        "AVGPRICE",
        "CHANGE",
        "CHANGERATIO",
        "TOTALSHARE",
        "CIRCULATEDSHARE",
        "TURNOVERRATE1",
        "TURNOVERRATE2",
        "MARKETVALUE",
        "CIRCULATEDMARKETVALUE",
        "AMPLITUDE",
        "LIMITDOWN",
        "LIMITUP",
        "LIMITSTATUS"};

    STDTHROWIF(
        static_cast<int>(headerFields.size()) != kExpectedFieldCount,
        STD_ERROR_CODE,
        "unexpected csv header field count: " << headerFields.size(),
        "expectedFieldCount=" << kExpectedFieldCount
                              << ", actualFieldCount=" << headerFields.size());

    for (int i = 0; i < kExpectedFieldCount; ++i)
    {
        STDTHROWIF(
            headerFields[i] != expectedHeader[i],
            STD_ERROR_CODE,
            "unexpected csv header field: " << headerFields[i],
            "index=" << i << ", expectedField=" << expectedHeader[i]
                      << ", actualField=" << headerFields[i]);
    }
}

StockQuotationDailyRecord StockQuotationDailyCsvLoader::parseRecord(
    const std::vector<std::string> &fields,
    const std::string &tradingDay,
    int lineNumber)
{
    STDTHROWIF(
        static_cast<int>(fields.size()) != kExpectedFieldCount,
        STD_ERROR_CODE,
        "unexpected csv field count at line " << lineNumber,
        "tradingDay=" << tradingDay << ", expectedFieldCount=" << kExpectedFieldCount
                       << ", actualFieldCount=" << fields.size());

    StockQuotationDailyRecord record;
    record.symbol = fields[0];
    STDTHROWIF(
        record.symbol.empty(),
        STD_ERROR_CODE,
        "empty symbol at line " << lineNumber,
        "tradingDay=" << tradingDay << ", lineNumber=" << lineNumber);
    record.tradingDay = fields[1];
    record.preClosePrice = parseScaledField(fields[2], "PRECLOSEPRICE", tradingDay, lineNumber);
    record.openPrice = parseScaledField(fields[3], "OPENPRICE", tradingDay, lineNumber);
    record.closePrice = parseScaledField(fields[4], "CLOSEPRICE", tradingDay, lineNumber);
    record.highPrice = parseScaledField(fields[5], "HIGHPRICE", tradingDay, lineNumber);
    record.lowPrice = parseScaledField(fields[6], "LOWPRICE", tradingDay, lineNumber);
    record.volume = parseIntegerField(fields[7], "VOLUME", tradingDay, lineNumber);
    record.amount = parseScaledField(fields[8], "AMOUNT", tradingDay, lineNumber);
    record.avgPrice = parseScaledField(fields[9], "AVGPRICE", tradingDay, lineNumber);
    record.change = parseDoubleField(fields[10], "CHANGE", tradingDay, lineNumber);
    record.changeRatio = parseDoubleField(fields[11], "CHANGERATIO", tradingDay, lineNumber);
    record.totalShare = parseIntegerField(fields[12], "TOTALSHARE", tradingDay, lineNumber);
    record.circulatedShare = parseIntegerField(fields[13], "CIRCULATEDSHARE", tradingDay, lineNumber);
    record.turnoverRate1 = parseDoubleField(fields[14], "TURNOVERRATE1", tradingDay, lineNumber);
    record.turnoverRate2 = parseDoubleField(fields[15], "TURNOVERRATE2", tradingDay, lineNumber);
    record.marketValue = parseScaledField(fields[16], "MARKETVALUE", tradingDay, lineNumber);
    record.circulatedMarketValue = parseScaledField(fields[17], "CIRCULATEDMARKETVALUE", tradingDay, lineNumber);
    record.amplitude = parseDoubleField(fields[18], "AMPLITUDE", tradingDay, lineNumber);
    record.limitDown = parseScaledField(fields[19], "LIMITDOWN", tradingDay, lineNumber);
    record.limitUp = parseScaledField(fields[20], "LIMITUP", tradingDay, lineNumber);
    record.limitStatus = fields[21];

    STDTHROWIF(
        record.tradingDay != tradingDay,
        STD_ERROR_CODE,
        "csv trading day mismatch at line " << lineNumber,
        "actualTradingDay=" << record.tradingDay << ", symbol=" << record.symbol);

    return record;
}
