#ifndef CONFIG_H
#define CONFIG_H

#include "util/ConfigReader.h"
#include <shared_mutex>
#include <memory>
#include <stdint.h>

class Config {
private:
    std::string file_path;
    ConfigReader* reader = NULL;
public:
    Config();
    ~Config();
    void SetConfigPath(std::string path);
    ConfigReader* GetConfigReader();
    int64_t ReadConfig(std::string key,int64_t defaultValue);
    std::string ReadConfig(std::string key,std::string defaultValue);

    std::string ReadConfigWithEnv(std::string key,std::string defaultValue);

    std::map<std::string, std::string> getReaderContents();
};

typedef std::shared_ptr<Config> auto_config_ptr;

#endif
