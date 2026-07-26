#ifndef COMMON_CORRELATIONCALCULATOR_H
#define COMMON_CORRELATIONCALCULATOR_H

#include <unordered_map>
#include <vector>

namespace hq_factor
{

    class CorrelationCalculator
    {
    public:
        CorrelationCalculator() = default;

        void rank(std::vector<double> *data, std::vector<double> *out, int size);

        void resolveTie(std::vector<double> *ranks, std::vector<int> *tiesTrace);

        double calculatePearson(std::vector<double> *yVector, std::vector<double> *xVector);

        double calculateSpearman(std::vector<double> *yVector, std::vector<double> *xVector);

        // 计算单一滞后值的自相关系数
        double calculateAutocorrelation(const std::vector<double>& data, int lag);
        // 计算向量的自相关系数，类似pandas.Series.autocorr功能
        std::vector<double> calculateAutocorrelationVec(const std::vector<double>& data, int maxLag = 60);
        // 计算Q统计量，基于自相关系数和样本大小
        double calculateQStatistic(const std::vector<double>& autocorrelations, double sampleSize);

    protected:
        struct IntDoublePair
        {
            int position;
            double value;

            IntDoublePair(int position, double value);

            ~IntDoublePair() = default;
        };
    };
}


#endif
