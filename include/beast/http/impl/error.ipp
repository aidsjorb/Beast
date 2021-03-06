//
// Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#ifndef BEAST_HTTP_IMPL_ERROR_IPP
#define BEAST_HTTP_IMPL_ERROR_IPP

#include <type_traits>

namespace boost {
namespace system {
template<>
struct is_error_code_enum<beast::http::error>
{
    static bool const value = true;
};
} // system
} // boost

namespace beast {
namespace http {
namespace detail {

class http_error_category : public error_category
{
public:
    const char*
    name() const noexcept override
    {
        return "http";
    }

    std::string
    message(int ev) const override
    {
        switch(static_cast<error>(ev))
        {
        default:
        case error::end_of_stream: return "end of stream";
        case error::partial_message: return "partial message";
        case error::need_more: return "need more";
        case error::buffer_overflow: return "buffer overflow";
        case error::bad_line_ending: return "bad line ending";
        case error::bad_method: return "bad method";
        case error::bad_path: return "bad path";
        case error::bad_version: return "bad version";
        case error::bad_status: return "bad status";
        case error::bad_reason: return "bad reason";
        case error::bad_field: return "bad field";
        case error::bad_value: return "bad value";
        case error::bad_content_length: return "bad Content-Length";
        case error::bad_transfer_encoding: return "bad Transfer-Encoding";
        case error::bad_chunk: return "bad chunk";
        }
    }

    error_condition
    default_error_condition(
        int ev) const noexcept override
    {
        return error_condition{ev, *this};
    }

    bool
    equivalent(int ev,
        error_condition const& condition
            ) const noexcept override
    {
        return condition.value() == ev &&
            &condition.category() == this;
    }

    bool
    equivalent(error_code const& error,
        int ev) const noexcept override
    {
        return error.value() == ev &&
            &error.category() == this;
    }
};

inline
error_category const&
get_http_error_category()
{
    static http_error_category const cat{};
    return cat;
}

} // detail

inline
error_code
make_error_code(error ev)
{
    return error_code{
        static_cast<std::underlying_type<error>::type>(ev),
            detail::get_http_error_category()};
}

} // http
} // beast

#endif
