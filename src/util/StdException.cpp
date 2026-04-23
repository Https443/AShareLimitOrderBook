

#include <sstream>
#include <errno.h>
#include <string.h>
#include <iostream>
#include "util/StdException.h"
#include <execinfo.h>

StdException::StdException(int error_code, std::string display_msg,
                   std::string debug_msg, std::string method, std::string file, int line)
{
    _error_code = error_code;
    _display_msg = display_msg;
    _debug_msg = debug_msg;
    _method = method;
    _file = file;
    _line = line;
    _sys_errno = errno;
    if (_sys_errno)
    {
        char buffer[4096];
        strerror_r(_sys_errno, buffer, 4096);
        _sys_errmsg = buffer;
    }
    _callstack = get_stackinfo();
    std::ostringstream buffer;
    buffer << display_msg << " (error_code=" << error_code << ")" << std::endl;
    if (debug_msg.length() > 0)
        buffer << "debug: " << debug_msg;
    if (this->_sys_errno)
        buffer << "syserror: errno=" << this->_sys_errno << ", errmsg=" << _sys_errmsg;
    buffer << std::endl << "location: M=" << method << ", F=" << file << ", L=" << line;
    if (_callstack != "")
        buffer << std::endl << "stack: " << _callstack;
    _full_msg = buffer.str();
}

StdException::~StdException() throw() {}

const char* StdException::what() const throw()
{
    return _full_msg.c_str();
}

int StdException::error_code() const
{
    return _error_code;
}

std::string StdException::display_msg() const
{
    return _display_msg;
}

std::string StdException::debug_msg() const
{
    return _debug_msg;
}

std::string StdException::file() const
{
    return _file;
}

std::string StdException::method() const
{
    return _method;
}

int StdException::line() const
{
    return _line;
}

int StdException::sys_errno() const
{
    return _sys_errno;
}

std::string StdException::sys_errmsg() const
{
    return _sys_errmsg;
}

std::string  get_stackinfo()
{
#define MAX_STACK_SIZE 1024

    void *stacks[MAX_STACK_SIZE];
    int size = backtrace(stacks, MAX_STACK_SIZE);
    if (size <= 0) return "";
    char **stackinfos = backtrace_symbols(stacks, size);
    if (stackinfos == NULL) return "";
    std::ostringstream text_buffer;
    for (int i = 1; i < size; i++)
    {
        char *stackinfo = stackinfos[i];
        text_buffer << stackinfo << std::endl;
    }
    free(stackinfos);
    std::string text = text_buffer.str();
    return text;
}