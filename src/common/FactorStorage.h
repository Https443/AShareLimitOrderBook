#ifndef FACTOR_STORAGE_H
#define FACTOR_STORAGE_H

#include <ostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>


class FactorStorage
{
    public:
    FactorStorage();
    ~FactorStorage();

    void writeFactorToCSV(std::string outPath, std::string date,
        std::shared_ptr<std::unordered_map<std::string, std::unordered_map<std::string, double>>> factorData);
};


#endif