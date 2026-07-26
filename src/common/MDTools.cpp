#include "MDTools.h"
#include "util/logger.h"
#include <filesystem>
#include <iostream>

std::vector<std::string> findDirectories(std::string path)
{
    LOG_INFO(app_log::logger(), "find path: {} files.", path);
    std::vector<std::string> targets;

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(path))
        {
            if (entry.is_directory())
            {
                // std::cout << entry.path() << std::endl;
                targets.push_back(entry.path().filename().string());
            }
        }
    }
    catch (const std::filesystem::filesystem_error &ex)
    {
        LOG_ERROR(app_log::logger(), "Error: {}", ex.what());
    }
    std::sort(targets.begin(), targets.end(), [](const auto &a, const auto &b)
              { return a < b; });
    return targets;
}

std::vector<std::string> findParquetFiles(std::string path)
{
    std::vector<std::string> codes;

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(path))
        {
            std::string filename = entry.path().filename().string();
            // 检查文件扩展名是否为.parquet
            if (filename.size() > 8 && filename.substr(filename.size() - 8) == ".parquet")
            {
                // 去除.parquet后缀，只保留代码部分
                std::string code = filename.substr(0, filename.size() - 8);
                codes.push_back(code);
            }
        }
    }
    catch (const std::filesystem::filesystem_error &ex)
    {
        LOG_ERROR(app_log::logger(), "Error: {}", ex.what());
    }
    std::sort(codes.begin(), codes.end(), [](const auto &a, const auto &b)
              { return a < b; });
    return codes;
}

std::vector<std::string> findFiles(std::string path, std::string suffix)
{
    std::vector<std::string> dates;

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(path))
        {
            std::string filename = entry.path().filename().string();
            // 检查文件扩展名是否为.parquet
            if (filename.size() > 8 && filename.substr(filename.size() - suffix.size()) == suffix)
            {
                // 去除.parquet后缀，只保留代码部分
                std::string code = filename.substr(0, filename.size() - suffix.size());
                dates.push_back(code);
            }
        }
    }
    catch (const std::filesystem::filesystem_error &ex)
    {
        LOG_ERROR(app_log::logger(), "Error: {}", ex.what());
    }
    std::sort(dates.begin(), dates.end(), [](const auto &a, const auto &b)
              { return a < b; });
    return dates;
}

std::vector<std::string> findCsvFiles(std::string path)
{
    std::vector<std::string> dates;

    try
    {
        for (const auto &entry : std::filesystem::directory_iterator(path))
        {
            std::string filename = entry.path().filename().string();
            // 检查文件扩展名是否为.csv
            if (filename.size() > 8 && filename.substr(filename.size() - 4) == ".csv")
            {
                // 去除.csv后缀，只保留代码部分
                std::string code = filename.substr(0, filename.size() - 4);
                dates.push_back(code);
            }
        }
    }
    catch (const std::filesystem::filesystem_error &ex)
    {
        LOG_ERROR(app_log::logger(), "Error: {}", ex.what());
    }
    std::sort(dates.begin(), dates.end(), [](const auto &a, const auto &b)
              { return a < b; });
    return dates;
}

std::string streamOrderData(const marketdata::Order *data)
{
    std::stringstream ss;
    ss << "code:" << data->securityCode
        << ", tradedate:" << data->tradingDay
        << ", time:" << data->time
        << ", price:" << data->price
        << ", volume:" << data->volume
        << ", appl_seq_num:" << data->applSeqNum
        << ", side:" << data->side
        << ", order_type:" << data->orderType
        << ", order_seq:" << data->orderDbNo
        << ", biz_index:" << data->bizIndex
        << ", channel_no:" << data->channelNo << "\n";
    return ss.str();
}

std::string streamOrderCsv(const marketdata::Order *data)
{
    std::stringstream ss;
    ss << "order," << data->securityCode
        << "," << data->tradingDay
        << "," << data->time
        << "," << data->price
        << "," << data->volume
        << "," << data->applSeqNum
        << "," << data->side
        << "," << data->orderType
        << "," << data->orderDbNo
        << "," << data->bizIndex
        << "," << data->channelNo << "\n";
    return ss.str();
}

std::string streamTradeData(const marketdata::Trade *data)
{
    std::stringstream ss;
    ss << "code:" << data->securityCode
        << ", tradedate:" << data->tradingDay
        << ", time:" << data->time
        << ", price:" << data->price
        << ", volume:" << data->volume
        << ", appl_seq_num:" << data->applSeqNum
        << ", bid_appl_seq_num:" << data->bidApplSeqNum
        << ", offer_appl_seq_num:" << data->offerApplSeqNum
        << ", side:" << data->side
        << ", exec_type:" << data->execType
        << ", biz_index:" << data->bizIndex
        << ", channel_no:" << data->channelNo << "\n";
    return ss.str();
}

std::string streamTradeCsv(const marketdata::Trade *data)
{
    std::stringstream ss;
    ss << "trade," << data->securityCode
        << "," << data->tradingDay
        << "," << data->time
        << "," << data->price
        << "," << data->volume
        << "," << data->applSeqNum
        << "," << data->bidApplSeqNum
        << "," << data->offerApplSeqNum
        << "," << data->side
        << "," << data->execType
        << "," << data->bizIndex
        << "," << data->channelNo << "\n";
    return ss.str();
}

std::string streamSnapshotData(const marketdata::MDSnapshot *data)
{
    std::stringstream ss;
    ss << "code:" << data->securityCode << ", time:" << data->origTime
              << ", last_price:" << data->lastPrice << ", trades:" << data->numTrades
              << ", volume:" << data->volume << ", turnover:" << data->turnover
              << ", total_trade:" << data->totalTrade << ", total_volume:" << data->totalVolume
              << ", total_turnover:" << data->totalTurnover << ", total_bid_volume:" << data->totalBidVolume
              << ", total_offer_volume:" << data->totalOfferVolume
              << ", weighted_avg_bid_price:" << data->weightedAvgBidPrice
              << ", weighted_avg_offer_price:" << data->weightedAvgOfferPrice
              << ", bid_price1:" << data->bidPrice[0] << ", bid_price2:" << data->bidPrice[1]
              << ", bid_price3:" << data->bidPrice[2] << ", bid_price4:" << data->bidPrice[3]
              << ", bid_price5:" << data->bidPrice[4] << ", bid_price6:" << data->bidPrice[5]
              << ", bid_price7:" << data->bidPrice[6] << ", bid_price8:" << data->bidPrice[7]
              << ", bid_price9:" << data->bidPrice[8] << ", bid_price10:" << data->bidPrice[9]
              << ", bid_volume1:" << data->bidVolume[0] << ", bid_volume2:" << data->bidVolume[1]
              << ", bid_volume3:" << data->bidVolume[2] << ", bid_volume4:" << data->bidVolume[3]
              << ", bid_volume5:" << data->bidVolume[4] << ", bid_volume6:" << data->bidVolume[5]
              << ", bid_volume7:" << data->bidVolume[6] << ", bid_volume8:" << data->bidVolume[7]
              << ", bid_volume9:" << data->bidVolume[8] << ", bid_volume10:" << data->bidVolume[9]
              << ", offer_price1:" << data->offerPrice[0] << ", offer_price2:" << data->offerPrice[1]
              << ", offer_price3:" << data->offerPrice[2] << ", offer_price4:" << data->offerPrice[3]
              << ", offer_price5:" << data->offerPrice[4] << ", offer_price6:" << data->offerPrice[5]
              << ", offer_price7:" << data->offerPrice[6] << ", offer_price8:" << data->offerPrice[7]
              << ", offer_price9:" << data->offerPrice[8] << ", offer_price10:" << data->offerPrice[9]
              << ", offer_volume1:" << data->offerVolume[0] << ", offer_volume2:" << data->offerVolume[1]
              << ", offer_volume3:" << data->offerVolume[2] << ", offer_volume4:" << data->offerVolume[3]
              << ", offer_volume5:" << data->offerVolume[4] << ", offer_volume6:" << data->offerVolume[5]
              << ", offer_volume7:" << data->offerVolume[6] << ", offer_volume8:" << data->offerVolume[7]
              << ", offer_volume9:" << data->offerVolume[8] << ", offer_volume10:" << data->offerVolume[9]
              << ", bid_order1:" << data->bidOrder[0] << ", bid_order2:" << data->bidOrder[1]
              << ", bid_order3:" << data->bidOrder[2] << ", bid_order4:" << data->bidOrder[3]
              << ", bid_order5:" << data->bidOrder[4] << ", bid_order6:" << data->bidOrder[5]
              << ", bid_order7:" << data->bidOrder[6] << ", bid_order8:" << data->bidOrder[7]
              << ", bid_order9:" << data->bidOrder[8] << ", bid_order10:" << data->bidOrder[9]
              << ", offer_order1:" << data->offerOrder[0] << ", offer_order2:" << data->offerOrder[1]
              << ", offer_order3:" << data->offerOrder[2] << ", offer_order4:" << data->offerOrder[3]
              << ", offer_order5:" << data->offerOrder[4] << ", offer_order6:" << data->offerOrder[5]
              << ", offer_order7:" << data->offerOrder[6] << ", offer_order8:" << data->offerOrder[7]
              << ", offer_order9:" << data->offerOrder[8] << ", offer_order10:" << data->offerOrder[9] << "\n";
    return ss.str();
}

std::string streamSnapshotCsv(const marketdata::MDSnapshot *data)
{
    std::stringstream ss;
    ss << "snapshot," << data->securityCode << "," << data->origTime
              << "," << data->lastPrice << "," << data->numTrades
              << "," << data->volume << "," << data->turnover
              << "," << data->totalTrade << "," << data->totalVolume
              << "," << data->totalTurnover << "," << data->totalBidVolume
              << "," << data->totalOfferVolume << "," << data->weightedAvgBidPrice
              << "," << data->weightedAvgOfferPrice
              << "," << data->bidPrice[0] << "," << data->bidPrice[1]
              << "," << data->bidPrice[2] << "," << data->bidPrice[3]
              << "," << data->bidPrice[4] << "," << data->bidPrice[5]
              << "," << data->bidPrice[6] << "," << data->bidPrice[7]
              << "," << data->bidPrice[8] << "," << data->bidPrice[9]
              << "," << data->bidVolume[0] << "," << data->bidVolume[1]
              << "," << data->bidVolume[2] << "," << data->bidVolume[3]
              << "," << data->bidVolume[4] << "," << data->bidVolume[5]
              << "," << data->bidVolume[6] << "," << data->bidVolume[7]
              << "," << data->bidVolume[8] << "," << data->bidVolume[9]
              << "," << data->offerPrice[0] << "," << data->offerPrice[1]
              << "," << data->offerPrice[2] << "," << data->offerPrice[3]
              << "," << data->offerPrice[4] << "," << data->offerPrice[5]
              << "," << data->offerPrice[6] << "," << data->offerPrice[7]
              << "," << data->offerPrice[8] << "," << data->offerPrice[9]
              << "," << data->offerVolume[0] << "," << data->offerVolume[1]
              << "," << data->offerVolume[2] << "," << data->offerVolume[3]
              << "," << data->offerVolume[4] << "," << data->offerVolume[5]
              << "," << data->offerVolume[6] << "," << data->offerVolume[7]
              << "," << data->offerVolume[8] << "," << data->offerVolume[9]
              << "," << data->bidOrder[0] << "," << data->bidOrder[1]
              << "," << data->bidOrder[2] << "," << data->bidOrder[3]
              << "," << data->bidOrder[4] << "," << data->bidOrder[5]
              << "," << data->bidOrder[6] << "," << data->bidOrder[7]
              << "," << data->bidOrder[8] << "," << data->bidOrder[9]
              << "," << data->offerOrder[0] << "," << data->offerOrder[1]
              << "," << data->offerOrder[2] << "," << data->offerOrder[3]
              << "," << data->offerOrder[4] << "," << data->offerOrder[5]
              << "," << data->offerOrder[6] << "," << data->offerOrder[7]
              << "," << data->offerOrder[8] << "," << data->offerOrder[9] << "\n";
    return ss.str();
}

void printOrder(const marketdata::Order *data)
{
    std::cout << streamOrderData(data);
}

void printOrderCsv(const marketdata::Order *data)
{
    std::cout << streamOrderCsv(data);
}

void printTrade(const marketdata::Trade *data)
{
    std::cout << streamTradeData(data);
}

void printTradeCsv(const marketdata::Trade *data)
{
    std::cout << streamTradeCsv(data);
}

void printSnapshot(const marketdata::MDSnapshot *data)
{
    std::cout << streamSnapshotData(data);
}

void printSnapshotCsv(const marketdata::MDSnapshot *data)
{
    std::cout << streamSnapshotCsv(data);
}
