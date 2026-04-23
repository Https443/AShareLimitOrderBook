

#ifndef CONFIGREADER_H
#define CONFIGREADER_H

#include <string>
#include <map>
#include <iostream>
#include <fstream>
#include <sstream>

class ConfigReader {
protected:
    std::string m_Delimiter;
    std::string m_Comment;
    std::map<std::string, std::string> m_Contents;

    typedef std::map<std::string, std::string>::iterator mapi;
    typedef std::map<std::string, std::string>::const_iterator mapci;
public:
    ConfigReader(const char* filename, std::string delimiter = "=", std::string comment = "#");
    ConfigReader();
    template<class T> T Read(const std::string& in_key) const;
    template<class T> T Read(const std::string& in_key, const T& in_value) const;
    template<class T> bool ReadInto(T& out_var, const std::string& in_key) const;
    template<class T> bool ReadInto(T& out_var, const std::string& in_key, const T& in_value) const;
    bool FileExist(std::string filename);
    void ReadFile(std::string filename, std::string delimiter = "=", std::string comment = "#");

    bool KeyExists(const std::string& in_key) const;

    template<class T> void Add(const std::string& in_key, const T& in_value);
    void Remove(const std::string& in_key);

    std::string GetDelimiter() const { return m_Delimiter; }
    std::string GetComment() const { return m_Comment; }
    std::string SetDelimiter(const std::string& in_s)
    {
        std::string old = m_Delimiter;  m_Delimiter = in_s;  return old;
    }
    std::string SetComment(const std::string& in_s)
    {
        std::string old = m_Comment;  m_Comment = in_s;  return old;
    }

    friend std::ostream& operator<<(std::ostream& os, const ConfigReader& cf);
    friend std::istream& operator >> (std::istream& is, ConfigReader& cf);

    const std::map<std::string, std::string> &GetContents() const;

protected:
    template<class T> static std::string T_as_string(const T& t);
    template<class T> static T string_as_T(const std::string& s);
    static void Trim(std::string& inout_s);


public:
    struct File_not_found {
        std::string filename;
        File_not_found(const std::string& filename_ = std::string())
        : filename(filename_) {}
    };
    struct Key_not_found {
        std::string key;
        Key_not_found(const std::string& key_ = std::string()): key(key_) {}
    };
};

template<class T> std::string ConfigReader::T_as_string(const T& t){
    std::ostringstream ost;
    ost << t;
    return ost.str();
}


template<class T> T ConfigReader::string_as_T(const std::string& s){
    T t;
    std::istringstream ist(s);
    ist >> t;
    return t;
}


template<> inline std::string ConfigReader::string_as_T<std::string>(const std::string& s){
    return s;
}


template<> inline bool ConfigReader::string_as_T<bool>(const std::string& s){
    // Convert from a string to a bool
    // Interpret "false", "F", "no", "n", "0" as false
    // Interpret "true", "T", "yes", "y", "1", "-1", or anything else as true
    bool b = true;
    std::string sup = s;
    for (std::string::iterator p = sup.begin(); p != sup.end(); ++p)
        *p = toupper(*p);  // make string all caps
        if (sup == std::string("FALSE") || sup == std::string("F") ||
        sup == std::string("NO") || sup == std::string("N") ||
        sup == std::string("0") || sup == std::string("NONE"))
            b = false;
        return b;
}

template<class T> T ConfigReader::Read(const std::string& key) const{
    // Read the value corresponding to key
    mapci p = m_Contents.find(key);
    if (p == m_Contents.end()) throw Key_not_found(key);
    return string_as_T<T>(p->second);
}

template<class T> T ConfigReader::Read(const std::string& key, const T& value) const{
    // Return the value corresponding to key or given default value
    // if key is not found
    mapci p = m_Contents.find(key);
    if (p == m_Contents.end()) return value;
    return string_as_T<T>(p->second);
}


template<class T> bool ConfigReader::ReadInto(T& var, const std::string& key) const {
    // Get the value corresponding to key and store in var
    // Return true if key is found
    // Otherwise leave var untouched
    mapci p = m_Contents.find(key);
    bool found = (p != m_Contents.end());
    if (found) var = string_as_T<T>(p->second);
    return found;
}


template<class T> bool ConfigReader::ReadInto(T& var, const std::string& key, const T& value) const{
    // Get the value corresponding to key and store in var
    // Return true if key is found
    // Otherwise set var to given default
    mapci p = m_Contents.find(key);
    bool found = (p != m_Contents.end());
    if (found)
        var = string_as_T<T>(p->second);
    else
        var = value;
    return found;
}


template<class T> void ConfigReader::Add(const std::string& in_key, const T& value){
    // Add a key with given value
    std::string v = T_as_string(value);
    std::string key = in_key;
    Trim(key);
    Trim(v);
    m_Contents[key] = v;
    return;
}


#endif
