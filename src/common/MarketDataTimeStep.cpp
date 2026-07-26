#include "MarketDataTimeStep.h"
#include <cstring>
#include <vector>
#include <algorithm>
#include "util/logger.h"

namespace marketdata {

MarketDataTimeStep::MarketDataTimeStep(TimeStepType timeStep)
{
    m_timeStepType = timeStep;
    timePart();
    m_codeNotFirst = std::set<std::string>();
    m_codeCurrentSnap = std::map<std::string, std::pair<int64_t, marketdata::MDSnapshot>>();
}

MarketDataTimeStep::~MarketDataTimeStep()
{

}

void MarketDataTimeStep::printTimeSteps()
{
    for (auto &ts : m_timeStep)
    {
        LOG_INFO(app_log::logger(), "calculate time step:{}", ts);
    }
}

void MarketDataTimeStep::snapshotToTimeStepData(const MDSnapshot *snapshot,
    std::function<void (int64_t time, const MDSnapshot *snapshot)> callback,
    int64_t amStartTime,
    int64_t amEndTime,
    int64_t pmStartTime,
    int64_t pmEndTime)
{
    if(m_timeStep.empty())
    {
        LOG_ERROR(app_log::logger(), "time step vec empty!");
        return;
    }

    std::string code = snapshot->securityCode;
    long timePart = snapshot->origTime % 1000000000; // 去掉前面的 YYYYMMDD

    if (!((timePart >= amStartTime && timePart < amEndTime) ||
        (timePart >= pmStartTime && timePart < pmEndTime)))
    {
        return;
    }

    int64_t stepTime = 0;
    for (auto &tsp : m_timeStep)
    {
        if (timePart < tsp)
        {
            stepTime = tsp;
            break;
        }
    }

    if (auto itor = m_codeNotFirst.find(code); itor == m_codeNotFirst.end())
    {
        m_codeNotFirst.insert(code);
        m_codeCurrentSnap[code] = std::make_pair(stepTime, *snapshot);
    }

    // LOGINFO("time_part:"<<time_part<<" stepTime:"<<stepTime);
    if (stepTime != m_codeCurrentSnap[code].first)
    {
        callback(m_codeCurrentSnap[code].first, &m_codeCurrentSnap[code].second);
        m_codeCurrentSnap[code].first = stepTime;
    }
    m_codeCurrentSnap[code].second = *snapshot;
}


void MarketDataTimeStep::finish(std::function<void (int64_t time, const MDSnapshot *snapshot)> callback)
{
    for (auto &pair : m_codeCurrentSnap)
    {
        callback(pair.second.first, &pair.second.second);
        // LOGINFO("snapshot code:"<<_pair.first<<" is end");
    }
}

void MarketDataTimeStep::timePart()
{
    int step = 0;
    // 因为不93000000之前的数据都记为93000000，所以这里直接取0930就可以
    int64_t ts0930Seconds = 9 * 3600 + 30 * 60;           // 09:30
    int64_t ts1131Seconds = 11 * 3600 + 31 * 60;          // 11:31
    int64_t ts1300Seconds = 13 * 3600 + 0 * 60;           // 13:00
    int64_t ts1501Seconds = 15 * 3600 + 1 * 60;           // 15:01

    switch (m_timeStepType)
    {
    case TimeStepType::SECONDS_30:
        step = 30;
        break;
    case TimeStepType::MINUTE_1:
        step = 60;
        break;
    case TimeStepType::MINUTE_5:
        step = 300;
        ts1131Seconds = 11 * 3600 + 35 * 60;          // 11:35
        ts1501Seconds = 15 * 3600 + 5 * 60;           // 15:05
        break;
    default:
        step = 60;
        break;
    }

    // 预分配vector容量以避免动态扩容
    const size_t morningCount = (ts1131Seconds - ts0930Seconds) / step + 1;
    const size_t afternoonCount = (ts1501Seconds - ts1300Seconds) / step + 1;
    m_timeStep.reserve(morningCount + afternoonCount);

    // 上午时段: 09:30 - 11:31
    for (int64_t i = ts0930Seconds; i <= ts1131Seconds; i += step)
    {
        uint64_t hour = i / 3600;
        uint64_t minute = (i - (hour * 3600)) / 60;
        uint64_t seconds = i - (hour * 3600) - (minute * 60);

        uint64_t timeStep = hour * 10000000 + minute * 100000 + seconds * 1000;
        m_timeStep.push_back(timeStep);
    }

    // 下午时段: 13:00 - 15:01
    for (int64_t i = ts1300Seconds; i <= ts1501Seconds; i += step)
    {
        uint64_t hour = i / 3600;
        uint64_t minute = (i - (hour * 3600)) / 60;
        uint64_t seconds = i - (hour * 3600) - (minute * 60);

        uint64_t timeStep = hour * 10000000 + minute * 100000 + seconds * 1000;
        m_timeStep.push_back(timeStep);
    }
}

}