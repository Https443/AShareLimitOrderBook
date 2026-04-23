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

std::string streamOrderData(const marketdata::MDOrder *data)
{
    std::stringstream ss;
    ss << "code:" << data->security_code << ", time:" << data->datetime
              << ", price:" << data->price << ", volume:" << data->volume
              << ", appl_seq_num:" << data->appl_seq_num << ", side:" << data->side
              << ", order_type:" << data->order_type << ", biz_index:" << data->biz_index
              << ", channel_no:" << data->channel_no << "\n";
    return ss.str();
}

std::string streamOrderCsv(const marketdata::MDOrder *data)
{
    std::stringstream ss;
    ss << "order," << data->security_code << "," << data->datetime
              << "," << data->price << "," << data->volume
              << "," << data->appl_seq_num << "," << data->side
              << "," << data->order_type << "," << data->biz_index
              << "," << data->channel_no << "\n";
    return ss.str();
}

std::string streamTradeData(const marketdata::MDTrade *data)
{
    std::stringstream ss;
    ss << "code:" << data->security_code << ", time:" << data->datetime
              << ", price:" << data->price << ", volume:" << data->volume
              << ", appl_seq_num:" << data->appl_seq_num << ", bid_appl_seq_num:" << data->bid_appl_seq_num
              << ", offer_appl_seq_num:" << data->offer_appl_seq_num << ", side:" << data->side
              << ", exec_type:" << data->exec_type << ", biz_index:" << data->biz_index
              << ", channel_no:" << data->channel_no << "\n";
    return ss.str();
}

std::string streamTradeCsv(const marketdata::MDTrade *data)
{
    std::stringstream ss;
    ss << "trade," << data->security_code << "," << data->datetime
              << "," << data->price << "," << data->volume
              << "," << data->appl_seq_num << "," << data->bid_appl_seq_num
              << "," << data->offer_appl_seq_num << "," << data->side
              << "," << data->exec_type << "," << data->biz_index
              << "," << data->channel_no << "\n";
    return ss.str();
}

std::string streamSnapshotData(const marketdata::MDSnapshot *data)
{
    std::stringstream ss;
    ss << "code:" << data->security_code << ", time:" << data->orig_time
              << ", last_price:" << data->last_price << ", trades:" << data->num_trades
              << ", volume:" << data->volume << ", turnover:" << data->turnover
              << ", total_trade:" << data->total_trade << ", total_volume:" << data->total_volume
              << ", total_turnover:" << data->total_turnover << ", total_bid_volume:" << data->total_bid_volume
              << ", total_offer_volume:" << data->total_offer_volume
              << ", weighted_avg_bid_price:" << data->weighted_avg_bid_price
              << ", weighted_avg_offer_price:" << data->weighted_avg_offer_price
              << ", bid_price1:" << data->bid_price[0] << ", bid_price2:" << data->bid_price[1]
              << ", bid_price3:" << data->bid_price[2] << ", bid_price4:" << data->bid_price[3]
              << ", bid_price5:" << data->bid_price[4] << ", bid_price6:" << data->bid_price[5]
              << ", bid_price7:" << data->bid_price[6] << ", bid_price8:" << data->bid_price[7]
              << ", bid_price9:" << data->bid_price[8] << ", bid_price10:" << data->bid_price[9]
              << ", bid_volume1:" << data->bid_volume[0] << ", bid_volume2:" << data->bid_volume[1]
              << ", bid_volume3:" << data->bid_volume[2] << ", bid_volume4:" << data->bid_volume[3]
              << ", bid_volume5:" << data->bid_volume[4] << ", bid_volume6:" << data->bid_volume[5]
              << ", bid_volume7:" << data->bid_volume[6] << ", bid_volume8:" << data->bid_volume[7]
              << ", bid_volume9:" << data->bid_volume[8] << ", bid_volume10:" << data->bid_volume[9]
              << ", offer_price1:" << data->offer_price[0] << ", offer_price2:" << data->offer_price[1]
              << ", offer_price3:" << data->offer_price[2] << ", offer_price4:" << data->offer_price[3]
              << ", offer_price5:" << data->offer_price[4] << ", offer_price6:" << data->offer_price[5]
              << ", offer_price7:" << data->offer_price[6] << ", offer_price8:" << data->offer_price[7]
              << ", offer_price9:" << data->offer_price[8] << ", offer_price10:" << data->offer_price[9]
              << ", offer_volume1:" << data->offer_volume[0] << ", offer_volume2:" << data->offer_volume[1]
              << ", offer_volume3:" << data->offer_volume[2] << ", offer_volume4:" << data->offer_volume[3]
              << ", offer_volume5:" << data->offer_volume[4] << ", offer_volume6:" << data->offer_volume[5]
              << ", offer_volume7:" << data->offer_volume[6] << ", offer_volume8:" << data->offer_volume[7]
              << ", offer_volume9:" << data->offer_volume[8] << ", offer_volume10:" << data->offer_volume[9]
              << ", bid_order1:" << data->bid_order[0] << ", bid_order2:" << data->bid_order[1]
              << ", bid_order3:" << data->bid_order[2] << ", bid_order4:" << data->bid_order[3]
              << ", bid_order5:" << data->bid_order[4] << ", bid_order6:" << data->bid_order[5]
              << ", bid_order7:" << data->bid_order[6] << ", bid_order8:" << data->bid_order[7]
              << ", bid_order9:" << data->bid_order[8] << ", bid_order10:" << data->bid_order[9]
              << ", offer_order1:" << data->offer_order[0] << ", offer_order2:" << data->offer_order[1]
              << ", offer_order3:" << data->offer_order[2] << ", offer_order4:" << data->offer_order[3]
              << ", offer_order5:" << data->offer_order[4] << ", offer_order6:" << data->offer_order[5]
              << ", offer_order7:" << data->offer_order[6] << ", offer_order8:" << data->offer_order[7]
              << ", offer_order9:" << data->offer_order[8] << ", offer_order10:" << data->offer_order[9] << "\n";
    return ss.str();
}

std::string streamSnapshotCsv(const marketdata::MDSnapshot *data)
{
    std::stringstream ss;
    ss << "snapshot," << data->security_code << "," << data->orig_time
              << "," << data->last_price << "," << data->num_trades
              << "," << data->volume << "," << data->turnover
              << "," << data->total_trade << "," << data->total_volume
              << "," << data->total_turnover << "," << data->total_bid_volume
              << "," << data->total_offer_volume << "," << data->weighted_avg_bid_price
              << "," << data->weighted_avg_offer_price
              << "," << data->bid_price[0] << "," << data->bid_price[1]
              << "," << data->bid_price[2] << "," << data->bid_price[3]
              << "," << data->bid_price[4] << "," << data->bid_price[5]
              << "," << data->bid_price[6] << "," << data->bid_price[7]
              << "," << data->bid_price[8] << "," << data->bid_price[9]
              << "," << data->bid_volume[0] << "," << data->bid_volume[1]
              << "," << data->bid_volume[2] << "," << data->bid_volume[3]
              << "," << data->bid_volume[4] << "," << data->bid_volume[5]
              << "," << data->bid_volume[6] << "," << data->bid_volume[7]
              << "," << data->bid_volume[8] << "," << data->bid_volume[9]
              << "," << data->offer_price[0] << "," << data->offer_price[1]
              << "," << data->offer_price[2] << "," << data->offer_price[3]
              << "," << data->offer_price[4] << "," << data->offer_price[5]
              << "," << data->offer_price[6] << "," << data->offer_price[7]
              << "," << data->offer_price[8] << "," << data->offer_price[9]
              << "," << data->offer_volume[0] << "," << data->offer_volume[1]
              << "," << data->offer_volume[2] << "," << data->offer_volume[3]
              << "," << data->offer_volume[4] << "," << data->offer_volume[5]
              << "," << data->offer_volume[6] << "," << data->offer_volume[7]
              << "," << data->offer_volume[8] << "," << data->offer_volume[9]
              << "," << data->bid_order[0] << "," << data->bid_order[1]
              << "," << data->bid_order[2] << "," << data->bid_order[3]
              << "," << data->bid_order[4] << "," << data->bid_order[5]
              << "," << data->bid_order[6] << "," << data->bid_order[7]
              << "," << data->bid_order[8] << "," << data->bid_order[9]
              << "," << data->offer_order[0] << "," << data->offer_order[1]
              << "," << data->offer_order[2] << "," << data->offer_order[3]
              << "," << data->offer_order[4] << "," << data->offer_order[5]
              << "," << data->offer_order[6] << "," << data->offer_order[7]
              << "," << data->offer_order[8] << "," << data->offer_order[9] << "\n";
    return ss.str();
}

void printOrder(const marketdata::MDOrder *data)
{
    std::cout << streamOrderData(data);
}

void printOrderCsv(const marketdata::MDOrder *data)
{
    std::cout << streamOrderCsv(data);
}

void printTrade(const marketdata::MDTrade *data)
{
    std::cout << streamTradeData(data);
}

void printTradeCsv(const marketdata::MDTrade *data)
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
