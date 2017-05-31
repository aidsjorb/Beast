//
// Copyright (c) 2013-2017 Vinnie Falco (vinnie dot falco at gmail dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <beast/core.hpp>
#include <beast/http.hpp>
#include <beast/core/detail/clamp.hpp>
#include <beast/core/detail/read_size_helper.hpp>
#include <beast/test/pipe_stream.hpp>
#include <beast/test/string_istream.hpp>
#include <beast/test/string_ostream.hpp>
#include <beast/test/yield_to.hpp>
#include <beast/unit_test/suite.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

namespace beast {
namespace http {

class design_test
    : public beast::unit_test::suite
    , public beast::test::enable_yield_to
{
public:
    // two threads, for some examples using a pipe
    design_test()
        : enable_yield_to(2)
    {
    }

    template<bool isRequest>
    bool
    equal_body(string_view sv, string_view body)
    {
        test::string_istream si{
            get_io_service(), sv.to_string()};
        message<isRequest, string_body, fields> m;
        multi_buffer b;
        try
        {
            read(si, b, m);
            return m.body == body;
        }
        catch(std::exception const& e)
        {
            log << "equal_body: " << e.what() << std::endl;
            return false;
        }
    }

    //--------------------------------------------------------------------------
    //
    // Example: Expect 100-continue
    //
    //--------------------------------------------------------------------------

    /** Send a message with Expect: 100-continue

        This function will send a message with the Expect: 100-continue
        field by first sending the header, then waiting for a successful
        response from the server before continuing to send the body. If
        a non-successful server response is received, the function
        returns immediately.

        @note The Expect header field is inserted into the message
        if it does not already exist, and set to "100-continue".
    */
    template<
        class SyncStream,
        class DynamicBuffer,
        class Body, class Fields>
    void do_expect_100_continue(
        SyncStream& stream,             // the stream to use
        DynamicBuffer& buffer,          // buffer used for reading
        request<Body, Fields>& req)     // the request to send
    {
        static_assert(is_sync_stream<SyncStream>::value,
            "SyncStream requirements not met");

        static_assert(is_dynamic_buffer<DynamicBuffer>::value,
            "DynamicBuffer requirements not met");

        // Insert or replace the Expect field
        req.fields.replace("Expect", "100-continue");

        serializer<true, Body, Fields> sr{req};

        // Send just the header
        write_header(stream, sr);

        BOOST_ASSERT(sr.is_header_done());
        BOOST_ASSERT(! sr.is_done());

        // Read the response from the server.
        // A robust client could set a timeout here.
        {
            response<string_body> res;
            read(stream, buffer, res);
            if(res.status != 100)
            {
                // The server indicated that it will not
                // accept the request, so skip sending the body.
                return;
            }
        }

        // Server is OK with the request, send the body
        write(stream, sr);
    }

    template<class Stream>
    void
    serverExpect100Continue(Stream& stream, yield_context yield)
    {
        flat_buffer buffer;
        message_parser<true, string_body, fields> parser;

        // read the header
        async_read_header(stream, buffer, parser, yield);
        if(parser.get().fields["Expect"] ==
            "100-continue")

        {
            // send 100 response
            message<false, empty_body, fields> res;
            res.version = 11;
            res.status = 100;
            res.reason("Continue");
            res.fields.insert("Server", "test");
            async_write(stream, res, yield);
        }

        BEAST_EXPECT(! parser.is_done());
        BEAST_EXPECT(parser.get().body.empty());

        // read the rest of the message
        async_read(stream, buffer, parser.base(), yield);
    }

    template<class Stream>
    void
    clientExpect100Continue(Stream& stream, yield_context yield)
    {
        flat_buffer buffer;
        request<string_body, fields> req;
        req.version = 11;
        req.method("POST");
        req.target("/");
        req.fields.insert("User-Agent", "test");
        req.body = "Hello, world!";
        prepare(req);

        do_expect_100_continue(stream, buffer, req);
    }

    void
    doExpect100Continue()
    {
        test::pipe p{ios_};
        yield_to(
            [&](yield_context yield)
            {
                serverExpect100Continue(p.server, yield);
            },
            [&](yield_context yield)
            {
                clientExpect100Continue(p.client, yield);
            });
    }

    //--------------------------------------------------------------------------
    //
    // Example: CGI child process relay
    //
    //--------------------------------------------------------------------------

    /** Send the output of a child process as an HTTP response.

        The output of the child process comes from a @b SyncReadStream. Data
        will be sent continuously as it is produced, without the requirement
        that the entire process output is buffered before being sent. The
        response will use the chunked transfer encoding.

        @param input A stream to read the child process output from.

        @param output A stream to write the HTTP response to.

        @param ec Set to the error, if any occurred.
    */
    template<class SyncReadStream, class SyncWriteStream>
    void
    do_cgi_response(SyncReadStream& input, SyncWriteStream& output, error_code& ec)
    {
        using boost::asio::buffer_cast;
        using boost::asio::buffer_size;

        // Set up the response. We use the buffer_body type,
        // allowing serialization to use manually provided buffers.
        message<false, buffer_body, fields> res;

        res.status = 200;
        res.version = 11;
        res.fields.insert("Server", "Beast");
        res.fields.insert("Transfer-Encoding", "chunked");

        // No data yet, but we set more = true to indicate
        // that it might be coming later. Otherwise the
        // serializer::is_done would return true right after
        // sending the header.
        res.body.data = nullptr;
        res.body.more = true;

        // Create the serializer. We set the split option to
        // produce the header immediately without also trying
        // to acquire buffers from the body (which would return
        // the error http::need_buffer because we set `data`
        // to `nullptr` above).
        auto sr = make_serializer(res);

        // Send the header immediately.
        write_header(output, sr, ec);
        if(ec)
            return;

        // Alternate between reading from the child process
        // and sending all the process output until there
        // is no more output.
        do
        {
            // Read a buffer from the child process
            char buffer[2048];
            auto bytes_transferred = input.read_some(
                boost::asio::buffer(buffer, sizeof(buffer)), ec);
            if(ec == boost::asio::error::eof)
            {
                ec = {};

                // `nullptr` indicates there is no buffer
                res.body.data = nullptr;

                // `false` means no more data is coming
                res.body.more = false;
            }
            else
            {
                if(ec)
                    return;

                // Point to our buffer with the bytes that
                // we received, and indicate that there may
                // be some more data coming
                res.body.data = buffer;
                res.body.size = bytes_transferred;
                res.body.more = true;
            }
            
            // Write everything in the body buffer
            write(output, sr, ec);

            // This error is returned by body_buffer during
            // serialization when it is done sending the data
            // provided and needs another buffer.
            if(ec == error::need_buffer)
            {
                ec = {};
                continue;
            }
            if(ec)
                return;
        }
        while(! sr.is_done());
    }

    void
    doCgiResponse()
    {
        error_code ec;
        std::string const body = "Hello, world!\n";
        test::string_ostream so{get_io_service(), 3};
        test::string_istream si{get_io_service(), body, 6};
        do_cgi_response(si, so, ec);
        BEAST_EXPECT(equal_body<false>(so.str, body));
    }

    //--------------------------------------------------------------------------
    //
    // Deferred Body type commitment
    //
    //--------------------------------------------------------------------------

    void
    doDeferredBody()
    {
        test::pipe p{ios_};
        ostream(p.server.buffer) <<
            "POST / HTTP/1.1\r\n"
            "User-Agent: test\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, world!";

        flat_buffer buffer;
        header_parser<true, fields> parser;
        auto bytes_used =
            read_some(p.server, buffer, parser);
        buffer.consume(bytes_used);

        message_parser<true, string_body, fields> parser2(
            std::move(parser));

        while(! parser2.is_done())
        {
            bytes_used = read_some(p.server, buffer, parser2);
            buffer.consume(bytes_used);
        }
    }

    //--------------------------------------------------------------------------
    //
    // Write using caller provided buffers
    //
    //--------------------------------------------------------------------------

    void
    doBufferBody()
    {
        test::pipe p{ios_};
        message<true, buffer_body, fields> m;
        std::string s = "Hello, world!";
        m.version = 11;
        m.method("POST");
        m.target("/");
        m.fields.insert("User-Agent", "test");
        m.fields.insert("Content-Length", s.size());
        auto sr = make_serializer(m);
        error_code ec;
        for(;;)
        {
            m.body.data = s.data();
            m.body.size = std::min<std::size_t>(3, s.size());
            m.body.more = s.size() > 3;
            write(p.client, sr, ec);
            if(ec != error::need_more)
            {
                BEAST_EXPECT(ec);
                return;
            }
            s.erase(s.begin(), s.begin() + m.body.size);
            if(sr.is_done())
                break;
        }
        BEAST_EXPECT(p.server.str() ==
            "POST / HTTP/1.1\r\n"
            "User-Agent: test\r\n"
            "Content-Length: 13\r\n"
            "\r\n"
            "Hello, world!");
    }

    //--------------------------------------------------------------------------
#if 0
    // VFALCO This is broken
    /*
        Efficiently relay a message from one stream to another
    */
    template<
        bool isRequest,
        class SyncWriteStream,
        class DynamicBuffer,
        class SyncReadStream>
    void
    relay(
        SyncWriteStream& out,
        DynamicBuffer& b,
        SyncReadStream& in)
    {
        flat_buffer buffer{4096}; // 4K limit
        header_parser<isRequest, fields> parser;
        serializer<isRequest, buffer_body<
            typename flat_buffer::const_buffers_type>,
                fields> ws{parser.get()};
        error_code ec;
        do
        {
      	    auto const state0 = parser.state();
            auto const bytes_used =
                read_some(in, buffer, parser, ec);
            BEAST_EXPECTS(! ec, ec.message());
            switch(state0)
            {
            case parse_state::header:
            {
                BEAST_EXPECT(parser.is_header_done());
                for(;;)
                {
                    ws.write_some(out, ec);
                    if(ec == http::error::need_more)
                    {
                        ec = {};
                        break;
                    }
                    if(ec)
                        BOOST_THROW_EXCEPTION(system_error{ec});
                }
                break;
            }

            case parse_state::chunk_header:
            {
                // inspect parser.chunk_extension() here
                if(parser.is_done())
                    boost::asio::write(out,
                        chunk_encode_final());
                break;
            }

            case parse_state::body:
            case parse_state::body_to_eof:
            case parse_state::chunk_body:
            {
                if(! parser.is_done())
                {
                    auto const body = parser.body();
                    boost::asio::write(out, chunk_encode(
                        false, boost::asio::buffer(
                            body.data(), body.size())));
                }
                break;
            }
          
            case parse_state::complete:
                break;
            }
            buffer.consume(bytes_used);
        }
        while(! parser.is_done());
    }

    void
    testRelay()
    {
        // Content-Length
        {
            test::string_istream is{ios_,
                "GET / HTTP/1.1\r\n"
                "Content-Length: 5\r\n"
                "\r\n" // 37 byte header
                "*****",
                3 // max_read
            };
            test::string_ostream os{ios_};
            flat_buffer b{16};
            relay<true>(os, b, is);
        }

        // end of file
        {
            test::string_istream is{ios_,
                "HTTP/1.1 200 OK\r\n"
                "\r\n" // 19 byte header
                "*****",
                3 // max_read
            };
            test::string_ostream os{ios_};
            flat_buffer b{16};
            relay<false>(os, b, is);
        }

        // chunked
        {
            test::string_istream is{ios_,
                "GET / HTTP/1.1\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "5;x;y=1;z=\"-\"\r\n*****\r\n"
                "3\r\n---\r\n"
                "1\r\n+\r\n"
                "0\r\n\r\n",
                2 // max_read
            };
            test::string_ostream os{ios_};
            flat_buffer b{16};
            relay<true>(os, b, is);
        }
    }
#endif

    //--------------------------------------------------------------------------
    /*
        Read the request header, then read the request body content using
        a fixed-size buffer, i.e. read the body in chunks of 4k for instance.
        The end of the body should be indicated somehow and chunk-encoding
        should be decoded by beast.
    */
    template<bool isRequest,
        class SyncReadStream, class BodyCallback>
    void
    doFixedRead(SyncReadStream& stream, BodyCallback const& cb)
    {
#if 0
        flat_buffer buffer{4096}; // 4K limit
        header_parser<isRequest, fields> parser;
        std::size_t bytes_used;
        bytes_used = read_some(stream, buffer, parser);
        BEAST_EXPECT(parser.is_header_done());
        buffer.consume(bytes_used);
        do
        {
            bytes_used =
                read_some(stream, buffer, parser);
            if(! parser.body().empty())
                cb(parser.body());
            buffer.consume(bytes_used);
        }
        while(! parser.is_done());
#endif
        fail("", __FILE__, __LINE__);
    }
    
    struct bodyHandler
    {
        void
        operator()(string_view const& body) const
        {
            // called for each piece of the body,
        }
    };

    void
    testFixedRead()
    {
        using boost::asio::buffer;
        using boost::asio::buffer_cast;
        using boost::asio::buffer_size;

        // Content-Length
        {
            test::string_istream is{ios_,
                "GET / HTTP/1.1\r\n"
                "Content-Length: 1\r\n"
                "\r\n"
                "*"
            };
            doFixedRead<true>(is, bodyHandler{});
        }

        // end of file
        {
            test::string_istream is{ios_,
                "HTTP/1.1 200 OK\r\n"
                "\r\n" // 19 byte header
                "*****"
            };
            doFixedRead<false>(is, bodyHandler{});
        }

        // chunked
        {
            test::string_istream is{ios_,
                "GET / HTTP/1.1\r\n"
                "Transfer-Encoding: chunked\r\n"
                "\r\n"
                "5;x;y=1;z=\"-\"\r\n*****\r\n"
                "3\r\n---\r\n"
                "1\r\n+\r\n"
                "0\r\n\r\n",
                2 // max_read
            };
            doFixedRead<true>(is, bodyHandler{});
        }
    }

    //--------------------------------------------------------------------------

    void
    run()
    {
        doExpect100Continue();
        doBufferBody();
        doCgiResponse();
#if 0
        doDeferredBody();

        //testRelay(); // VFALCO Broken with serializer changes
        testFixedRead();
#endif
        pass();
    }
};

BEAST_DEFINE_TESTSUITE(design,http,beast);

} // http
} // beast
