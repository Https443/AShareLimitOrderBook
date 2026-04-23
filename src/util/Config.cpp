

#include "Config.h"
#include <cctype>
#include <cstdlib> 
#include <cstring>
#include <sstream>
#include <string>

Config::Config() {
    file_path = "config/app.txt";
}

void Config::SetConfigPath(std::string path){
    this->file_path = path;
    this->reader = new ConfigReader(this->file_path.c_str());
}
ConfigReader* Config::GetConfigReader(){
    return this->reader;
}

int64_t Config::ReadConfig(std::string key,int64_t defaultValue){
    if(this->reader){
        return this->reader->Read(key,defaultValue);
    }

    return defaultValue;
}

std::string Config::ReadConfig(std::string key,std::string defaultValue){
    if(this->reader){
        return this->reader->Read(key,defaultValue);
    }

    return defaultValue;
}

std::string Config::ReadConfigWithEnv(std::string key,std::string defaultValue) {
    std::string result(key);

    for(std::string::iterator it = result.begin(); it != result.end(); it++)
        *it = std::toupper(*it);

    auto env = std::getenv(result.c_str());
    if(env != NULL && strcmp(env, "") != 0) {
        return std::string(env);
    } else {
        if(this->reader){
            return this->reader->Read(key,defaultValue);
        }
    }
    return defaultValue;
}

std::map<std::string, std::string> Config::getReaderContents() {
    return this->reader->GetContents();
}

Config::~Config(){
    if (this->reader) {
        delete this->reader;
        this->reader = nullptr;
    }
}