

#ifndef _STDEXCEPTION_H
#define _STDEXCEPTION_H

#include <boost/exception_ptr.hpp>
#include <boost/throw_exception.hpp>
#include <exception>
#include <execinfo.h>
#include <string>
#include <sstream>

class  StdException: public std::exception {
private:
    int _error_code;
    std::string _display_msg;
    std::string _debug_msg;
    std::string _method;
    std::string _file;
    int _line;
    int _sys_errno;
    std::string _sys_errmsg;
    std::string _full_msg;
    std::string _callstack;
public:
    StdException(int error_code, std::string display_msg,
                 std::string debug_msg, std::string method, std::string file, int line);

    ~StdException() throw();

    virtual const char *what() const throw();

    int error_code() const;

    std::string display_msg() const;

    std::string debug_msg() const;

    std::string file() const;

    std::string method() const;

    int sys_errno() const;

    std::string sys_errmsg() const;

    int line() const;
};

std::string get_stackinfo();

#define STD_SUCC_CODE 200
#define STD_ERROR_CODE 400
#define STD_SYSTEM_ERR_CODE -1

#define STDTHROW(ERROR_CODE, DISPLAY_STREAM, DEBUG_STREAM) { \
	std::ostringstream display_buffer; \
	display_buffer << DISPLAY_STREAM; \
	std::ostringstream debug_buffer; \
	debug_buffer << DEBUG_STREAM; \
	throw StdException(ERROR_CODE, display_buffer.str(), debug_buffer.str(), \
		__FUNCTION__, __FILE__, __LINE__);}

#define STDTHROWIF(CONDITION, ERROR_CODE, DISPLAY_STREAM, DEBUG_STREAM) \
	if (CONDITION) {STDTHROW(ERROR_CODE, DISPLAY_STREAM, DEBUG_STREAM)}

#define THROW(STR) {STDTHROW(999,STR,STR);}
#define THROWIF(CONDITION,STR) {if(CONDITION){THROW(STR);}}

#define STDTRY try { try
#define STDCATCH \
	catch (StdException) {	throw;	} \
	catch (boost::exception &ex) \
	{ \
		std::exception *se; \
		try { se = dynamic_cast<std::exception *>(&ex);	} catch (...) { se = NULL; } \
		const char *msg = (se) ? (se->what()) : (""); \
		const char **pfunc = boost::get_error_info<boost::throw_function>(ex); \
		const char **pfile = boost::get_error_info<boost::throw_file>(ex); \
		int *pline = boost::get_error_info<boost::throw_line>(ex); \
		const char *func = (pfunc) ? (*pfunc) : (__func__); \
		const char *file = (pfile) ? (*pfile) : (__FILE__); \
		int line= (pline) ? (*pline) : (__LINE__); \
		throw StdException(-1, msg, msg, func, file, line); \
	} \
	catch (std::exception &ex) {    \
	    throw StdException(-1, ex.what(), ex.what(),__func__,__FILE__,__LINE__); } \
	catch (...) { throw StdException(-1, "unknown exception", "unknown exception",__func__,__FILE__,__LINE__); } \
	} catch (StdException &ex)

#define FREEANDNULL(PTR) if (PTR) { free(PTR); PTR = NULL; }
#define DELETEANDNULL(PTR) if (PTR) { try { delete PTR; PTR = NULL; } catch (...) {;} }
#define CHECKNULL(PTR,STR) {THROWIF(!PTR,STR);}
#define CHECKNOTNULL(PTR,STR) {THROWIF(PTR,STR);}
#endif //_STDEXCEPTION_H
