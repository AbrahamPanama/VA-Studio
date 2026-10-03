// SPDX-License-Identifier: LGPL-2.1-only OR MPL-1.1
/**
 * \file
 * \brief  Defines the different types of exceptions that 2geom can throw.
 *
 * There are two main exception classes: LogicalError and RangeError.
 * Logical errors are 2geom faults/bugs; RangeErrors are 'user' faults,
 * e.g. invalid arguments to lib2geom methods.
 * This way, the 'user' can distinguish between groups of exceptions
 * ('user' is the coder that uses lib2geom)
 *
 * Several macro's are defined for easily throwing exceptions 
 * (e.g. THROW_CONTINUITYERROR). 
 */
/* Copyright 2007 Johan Engelen <goejendaagh@zonnet.nl>
 */

#ifndef LIB2GEOM_SEEN_EXCEPTION_H
#define LIB2GEOM_SEEN_EXCEPTION_H

#include <exception>
#include <sstream>
#include <string>

namespace Geom {

/**
 * Base exception class, all 2geom exceptions should be derived from this one.
 */
class Exception : public std::exception {
public:
    Exception(const char * message, const char *file, const int line) {
        std::ostringstream os;
        os << "lib2geom exception: " << message << " (" << file << ":" << line << ")";
        msgstr = os.str();
    }

    ~Exception() noexcept override {} // necessary to destroy the string object!!!

    const char* what() const noexcept override {
        return msgstr.c_str();
    }
protected:
    std::string msgstr;
};
#define THROW_EXCEPTION(message) throw(Geom::Exception(message, __FILE__, __LINE__))

//-----------------------------------------------------------------------

class LogicalError : public Exception {
public:
    LogicalError(const char * message, const char *file, const int line)
        : Exception(message, file, line) {}
};
#define THROW_LOGICALERROR(message) throw(LogicalError(message, __FILE__, __LINE__))

class RangeError : public Exception {
public:
    RangeError(const char * message, const char *file, const int line)
        : Exception(message, file, line) {}
};
#define THROW_RANGEERROR(message) throw(RangeError(message, __FILE__, __LINE__))

//-----------------------------------------------------------------------
// Special case exceptions. Best used with the defines :)

class NotImplemented : public LogicalError {
public:
    NotImplemented(const char *file, const int line)
        : LogicalError("Method not implemented", file, line) {}
};
#define THROW_NOTIMPLEMENTED(i) throw(NotImplemented(__FILE__, __LINE__))

class InvariantsViolation : public LogicalError {
public:
    InvariantsViolation(const char *file, const int line)
        : LogicalError("Invariants violation", file, line) {}
};
#define THROW_INVARIANTSVIOLATION(i) throw(InvariantsViolation(__FILE__, __LINE__))
#define ASSERT_INVARIANTS(e)       ((e) ? (void)0 : THROW_INVARIANTSVIOLATION())

class NotInvertible : public RangeError {
public:
    NotInvertible(const char *file, const int line)
        : RangeError("Function does not have a unique inverse", file, line) {}
};
#define THROW_NOTINVERTIBLE(i) throw(NotInvertible(__FILE__, __LINE__))

class InfiniteSolutions : public RangeError {
public:
	InfiniteSolutions(const char *file, const int line)
        : RangeError("There are infinite solutions", file, line) {}
};
#define THROW_INFINITESOLUTIONS(i) throw(InfiniteSolutions(__FILE__, __LINE__))

class InfinitelyManySolutions : public RangeError {
private:
    char const *const _message;
public:
    InfinitelyManySolutions(const char *file, const int line, char const *message)
        : RangeError("There are infinitely many solutions", file, line)
        , _message{message}
    {}
    char const *what() const noexcept override { return _message; }
};
#define THROW_INFINITELY_MANY_SOLUTIONS(msg) throw(InfinitelyManySolutions(__FILE__, __LINE__, msg))

class ContinuityError : public RangeError {
public:
    ContinuityError(const char *file, const int line)
        : RangeError("Non-contiguous path", file, line) {}
};
#define THROW_CONTINUITYERROR(i) throw(ContinuityError(__FILE__, __LINE__))

struct SVGPathParseError : public std::exception {
    char const *what() const noexcept override { return "parse error"; }
};


} // namespace Geom

#endif


/*
  Local Variables:
  mode:c++
  c-file-style:"stroustrup"
  c-file-offsets:((innamespace . 0)(inline-open . 0)(case-label . +))
  indent-tabs-mode:nil
  fill-column:99
  End:
*/
// vim: filetype=cpp:expandtab:shiftwidth=4:tabstop=8:softtabstop=4:fileencoding=utf-8:textwidth=99 :
