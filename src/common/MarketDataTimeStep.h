#ifndef MARKETDATA_TIME_STEP_H
#define MARKETDATA_TIME_STEP_H
#include "MarketDataStruct.h"
#include <memory>
#include <stdint.h>
#include <vector>
#include <functional>
#include <map>
#include <set>

namespace marketdata
{
    enum class TimeStepType
    {
        SECONDS_30,
        MINUTE_1,
        MINUTE_5
    };

    static const int64_t kTs0915 = 91500000;
    static const int64_t kTs1131 = 113100000;
    static const int64_t kTs1300 = 130000000;
    static const int64_t kTs1501 = 150100000;

    class MarketDataTimeStep
    {
        public:
        MarketDataTimeStep(TimeStepType timeStep);
        ~MarketDataTimeStep();

        void printTimeSteps();

        void snapshotToTimeStepData(const MDSnapshot *snapshot,
            std::function<void (int64_t time, const MDSnapshot *snapshot)> callback,
            int64_t amStartTime = kTs0915,
            int64_t amEndTime = kTs1131,
            int64_t pmStartTime = kTs1300,
            int64_t pmEndTime = kTs1501);
        void finish(std::function<void (int64_t time, const MDSnapshot *snapshot)> callback);

        inline const std::vector<int64_t> *getTimeStep()
        {
            return &m_timeStep;
        }

        private:
        void timePart();

        private:
        TimeStepType m_timeStepType;
        std::vector<int64_t> m_timeStep;
        std::map<std::string, std::pair<int64_t, marketdata::MDSnapshot>> m_codeCurrentSnap;
        std::set<std::string> m_codeNotFirst;
    };

}

#endif