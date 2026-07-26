#include "CorrelationCalculator.h"
#include "util/StdException.h"
#include <algorithm>
#include "commonUtil.h"


namespace hq_factor {

void CorrelationCalculator::resolveTie(std::vector<double> *ranks, std::vector<int> *tiesTrace)
{
    // constant value of ranks over tiesTrace
    double c = (*ranks)[tiesTrace->at(0)];

    // length of sequence of tied ranks
    int length = tiesTrace->size();
    double value = (2.0 * c + length - 1.0) / 2.0;
    for (const auto &item : *tiesTrace)
    {
        (*ranks)[item] = value;
    }
}

void CorrelationCalculator::rank(std::vector<double> *data, std::vector<double> *out, int size)
{
    std::vector<IntDoublePair *> ranks;
    for (int i = 0; i < data->size(); i++)
    {
        auto p = new IntDoublePair(i, data->at(i));
        ranks.push_back(p);
    }
    std::sort(ranks.begin(), ranks.end(), [](IntDoublePair *d1, IntDoublePair *d2)
              { return d1->value < d2->value; });
    int pos = 1;
    (*out)[ranks.at(0)->position] = pos;
    std::vector<int> *tiesTrace = new std::vector<int>();
    tiesTrace->push_back(ranks.at(0)->position);
    for (int i = 1; i < ranks.size(); i++)
    {
        if (ranks[i]->value > ranks[i - 1]->value)
        {
            // tie sequence has ended (or had length 1)
            pos = i + 1;
            if (tiesTrace->size() > 1)
            { // if seq is nontrivial, resolve
                resolveTie(out, tiesTrace);
            }
            DELETEANDNULL(tiesTrace)
            tiesTrace = new std::vector<int>();
            tiesTrace->push_back(ranks[i]->position);
        }
        else
        {
            // tie sequence continues
            tiesTrace->push_back(ranks[i]->position);
        }
        (*out)[ranks.at(i)->position] = pos;
    }
    if (tiesTrace->size() > 1)
    { // handle tie sequence at end
        resolveTie(out, tiesTrace);
    }
    DELETEANDNULL(tiesTrace);
    for (auto itor = ranks.begin(); itor != ranks.end();)
    {
        auto pair = (*itor);
        DELETEANDNULL(pair);
        itor = ranks.erase(itor);
    }
}

double CorrelationCalculator::calculatePearson(std::vector<double> *yVector, std::vector<double> *xVector)
{
    if (yVector->empty() || xVector->empty())
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double length = xVector->size();
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumXX = 0.0;
    double sumYY = 0.0;

    // ( E(XY) - E(X)E(Y) ) / (sqrt( E(X*X) - E(X) * E(X) ) * sqrt( E(Y*Y) - E(Y) * E(Y) ))
    for (int m = 0; m < length; m++)
    {
        double x = xVector->at(m);
        double y = yVector->at(m);
        sumX += x;
        sumY += y;

        sumXY += x * y;
        sumXX += x * x;
        sumYY += y * y;
    }

    double averageX = sumX / length;
    double averageY = sumY / length;

    double stdX = std::sqrt(sumXX / length - averageX * averageX);
    if (stdX == 0.0 || isNaNOrInf(stdX))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double stdY = std::sqrt(sumYY / length - averageY * averageY);
    if (stdY == 0.0 || isNaNOrInf(stdY))
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    double covXY = sumXY / length - averageX * averageY;
    return covXY / (stdX * stdY);
}

double CorrelationCalculator::calculateSpearman(std::vector<double> *yVector, std::vector<double> *xVector)
{
    // 公式1
    //    double sumd = 0.0;
    //    int length = yVector->size();
    //    for (int m = 0; m < length; m++) {
    //        double x = xVector->at(m);
    //        double y = yVector->at(m);
    //        double d = std::pow(x-y, 2);
    //        d = compareDouble(d, 0.0) == 0.0 ? 0.0 : d;
    //        sumd += d;
    //    }
    //    double correlation = 1.0 - 6.0 * sumd / (length * (std::pow(length, 2.0) - 1.0));
    //    return compareDouble(correlation, 0.0) == 0.0 ? 0.0 : correlation;

    // 公式2
    double size = yVector->size();
    double xAvg = 0.0;
    double yAvg = 0.0;
    for (int i = 0; i < yVector->size(); i++)
    {
        xAvg += xVector->at(i);
        yAvg += yVector->at(i);
    }
    xAvg = xAvg / size;
    yAvg = yAvg / size;

    double sumMinusAvg = 0.0;
    double sumXd = 0.0;
    double sumYd = 0.0;

    for (int m = 0; m < yVector->size(); m++)
    {
        double x = xVector->at(m) - xAvg;
        double y = yVector->at(m) - yAvg;
        double d = x * y;
        d = compareDouble(d, 0.0) == 0 ? 0 : d;
        sumMinusAvg += d;
        sumXd += std::pow(x, 2.0);
        sumYd += std::pow(y, 2.0);
    }
    return sumMinusAvg / std::pow(sumXd * sumYd, 0.5);
}


double CorrelationCalculator::calculateAutocorrelation(const std::vector<double>& data, int lag) {
    if (lag <= 0 || data.size() <= lag) {
        return std::numeric_limits<double>::quiet_NaN();
    }

    // 创建滞后版本的数据
    std::vector<double> original(data.begin(), data.end() - lag);
    std::vector<double> shifted(data.begin() + lag, data.end());

    // 使用现有的Pearson相关计算方法
    return calculatePearson(&shifted, &original);
}

std::vector<double> CorrelationCalculator::calculateAutocorrelationVec(const std::vector<double>& data, int maxLag)
{
    std::vector<double> result;
    result.reserve(maxLag);

    for (int lag = 1; lag <= maxLag; lag++) {
        result.push_back(calculateAutocorrelation(data, lag));
    }

    return result;
}

double CorrelationCalculator::calculateQStatistic(const std::vector<double>& autocorrelations, double sampleSize) {
    if (autocorrelations.empty() || sampleSize <= 0) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    // 计算Q统计量
    // Q = n(n+2) * sum(rho_k^2 / (n-k))
    // 其中rho_k是滞后k的自相关系数，n是样本大小
    double qStatistic = 0.0;
    for (size_t k = 0; k < autocorrelations.size(); ++k) {
        if (!std::isnan(autocorrelations[k]))
        {
            qStatistic += (autocorrelations[k] * autocorrelations[k]) / (sampleSize - autocorrelations.size());
        }
    }
    
    qStatistic *= sampleSize * (sampleSize + 2);
    
    return qStatistic;
}


CorrelationCalculator::IntDoublePair::IntDoublePair(int position, double value) : position(position), value(value) {}

}
