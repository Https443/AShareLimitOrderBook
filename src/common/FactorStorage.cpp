#include "FactorStorage.h"

FactorStorage::FactorStorage()
{
    // 默认构造函数实现
}

FactorStorage::~FactorStorage()
{
    // 默认析构函数实现
}

// outPath 为输出文件名称
// date 为因子日期
// factorData 布局map(code, map(id, value))
void FactorStorage::writeFactorToCSV(std::string outPath, std::string date,std::shared_ptr<std::unordered_map<std::string, std::unordered_map<std::string, double>>> factorData)
{
    std::ofstream file = std::ofstream(outPath, std::ios_base::out | std::ios_base::trunc);
    for (auto &singalFactor: *factorData)
    {
        std::string code = singalFactor.first;
        for (auto &factor: singalFactor.second)
        {
            file << factor.first << "," << date << "," << code << "," << factor.second << std::endl;
        }
    }
    file.close();
}