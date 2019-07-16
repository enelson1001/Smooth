// Smooth - C++ framework for writing applications based on Espressif's ESP-IDF.
// Copyright (C) 2017 Per Malmberg (https://github.com/PerMalmberg)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.

#include <smooth/application/network/http/HTTPServerClient.h>
#include <smooth/application/network/http/IResponseOperation.h>
#include <smooth/application/network/http/websocket/responses/WSResponse.h>

namespace smooth::application::network::http
{
    static const char* tag = "HTTPServerClient";
    using namespace websocket;
    using namespace websocket::responses;

    void HTTPServerClient::event(
            const core::network::event::DataAvailableEvent<HTTPProtocol>& event)
    {
        if (mode == Mode::HTTP)
        {
            http_event(event);
        }
        else
        {
            websocket_event(event);
        }
    }

    void
    HTTPServerClient::event(
            const smooth::core::network::event::TransmitBufferEmptyEvent&)
    {
        if (current_operation)
        {
            std::vector<uint8_t> data;
            auto res = current_operation->get_data(content_chunk_size, data);

            if (res == ResponseStatus::Error)
            {
                Log::error(tag, "Current operation reported error, closing server client.");
                this->close();
            }
            else if (res == ResponseStatus::EndOfData)
            {
                current_operation.reset();
                // Immediately send next
                send_first_part();
            }
            else if (res == ResponseStatus::HasMoreData
                     || res == ResponseStatus::LastData)
            {
                HTTPPacket p{data};
                auto& tx = this->get_buffers()->get_tx_buffer();
                tx.put(p);
            }
        }
        else
        {
            send_first_part();
        }
    }


    void HTTPServerClient::disconnected()
    {
    }


    void HTTPServerClient::connected()
    {
        this->socket->set_receive_timeout(DefaultKeepAlive);
    }


    void HTTPServerClient::reset_client()
    {
        operations.clear();
        current_operation.reset();
        mode = Mode::HTTP;
        ws_server.reset();
    }


    bool HTTPServerClient::parse_url(std::string& raw_url)
    {
        separate_request_parameters(raw_url);

        auto res = encoding.decode(raw_url);

        return res;
    }


    void HTTPServerClient::separate_request_parameters(std::string& url)
    {
        // Only supporting key=value format.
        request_parameters.clear();

        auto pos = std::find(url.begin(), url.end(), '?');
        if (pos != url.end())
        {
            encoding.decode(url, pos, url.end());

            pos++;
            while (pos != url.end())
            {
                auto equal_sign = std::find(pos, url.end(), '=');
                if (equal_sign != url.end())
                {
                    // Find ampersand or end of string
                    auto ampersand = std::find(equal_sign, url.end(), '&');
                    auto key = std::string(pos, equal_sign);
                    auto value = std::string{++equal_sign, ampersand};
                    request_parameters[key] = value;
                    if (ampersand != url.end())
                    {
                        ++ampersand;
                    }
                    pos = ampersand;
                }
                else
                {
                    pos = url.end();
                }
            }
        }

        pos = std::find(url.begin(), url.end(), '?');
        if (pos != url.end())
        {
            url.erase(pos, url.end());
        }
    }


    void HTTPServerClient::reply(
            std::unique_ptr<IResponseOperation> response, bool place_first)
    {
        if (mode == Mode::HTTP)
        {
            using namespace std::chrono;
            const auto timeout = duration_cast<seconds>(this->socket->get_receive_timeout());

            if (timeout.count() > 0)
            {
                response->add_header(CONNECTION, "keep-alive");
                response->set_header(KEEP_ALIVE, "timeout=" + std::to_string(timeout.count()));
            }
        }

        if (place_first)
        {
            operations.insert(operations.begin(), std::move(response));
        }
        else
        {
            operations.emplace_back(std::move(response));
        }

        if (!current_operation)
        {
            send_first_part();
        }
    }

    void HTTPServerClient::reply_error(std::unique_ptr<IResponseOperation> response)
    {
        operations.clear();
        response->add_header(CONNECTION, "close");
        operations.emplace_back(std::move(response));
        if (!current_operation)
        {
            send_first_part();
        }
    }

    void HTTPServerClient::send_first_part()
    {
        if (!operations.empty())
        {
            current_operation = std::move(operations.front());
            operations.pop_front();

            const auto& headers = current_operation->get_headers();

            std::vector<uint8_t> data{};
            auto res = current_operation->get_data(content_chunk_size, data);

            if (res == ResponseStatus::Error)
            {
                Log::error(tag, "Current operation reported error, closing server client.");
                this->close();
            }
            else
            {
                auto& tx = this->get_buffers()->get_tx_buffer();
                if (mode == Mode::HTTP)
                {
                    // Whether or not everything is sent, send the current (possibly header-only) packet.
                    HTTPPacket p{current_operation->get_response_code(), "1.1", headers, data};
                    tx.put(p);
                }
                else
                {
                    HTTPPacket p{data};
                    tx.put(p);
                }

                if (res == ResponseStatus::EndOfData)
                {
                    current_operation.reset();
                    // Immediately send next
                    send_first_part();
                }
            }
        }
    }


    bool HTTPServerClient::translate_method(
            const smooth::application::network::http::HTTPPacket& packet,
            smooth::application::network::http::HTTPMethod& method)
    {
        auto res = true;

        // HTTP verbs are case sensitive: https://tools.ietf.org/html/rfc7230#section-3.1.1
        if (packet.get_request_method() == "POST")
        {
            method = HTTPMethod::POST;
        }
        else if (packet.get_request_method() == "GET")
        {
            method = HTTPMethod::GET;
        }
        else if (packet.get_request_method() == "DELETE")
        {
            method = HTTPMethod::DELETE;
        }
        else if (packet.get_request_method() == "HEAD")
        {
            method = HTTPMethod::HEAD;
        }
        else if (packet.get_request_method() == "PUT")
        {
            method = HTTPMethod::PUT;
        }
        else
        {
            res = false;
        }

        return res;
    }


    void HTTPServerClient::set_keep_alive()
    {
        auto connection = request_headers.find("connection");
        if (connection != request_headers.end())
        {
            auto s = (*connection).second;
            if (string_util::icontains(s, "keep-alive"))
            {
                this->socket->set_receive_timeout(DefaultKeepAlive);
            }
        }
    }

    void HTTPServerClient::http_event(const core::network::event::DataAvailableEvent<HTTPProtocol>& event)
    {
        typename HTTPProtocol::packet_type packet;
        if (event.get(packet))
        {
            bool first_packet = !packet.is_continuation();
            bool last_packet = !packet.is_continued();

            bool res = true;

            if (first_packet)
            {
                // First packet, parse URL etc.
                request_headers.clear();
                std::swap(request_headers, packet.headers());
                requested_url = packet.get_request_url();
                res = parse_url(requested_url);
                set_keep_alive();

                mime.reset();
            }

            if (res)
            {
                auto* context = this->get_client_context();

                if (context)
                {
                    HTTPMethod method{};
                    if (translate_method(packet, method))
                    {
                        context->handle(method,
                                        *this,
                                        *this,
                                        requested_url,
                                        request_headers,
                                        request_parameters,
                                        packet.get_buffer(),
                                        mime,
                                        first_packet,
                                        last_packet);
                    }
                    else
                    {
                        // Unsupported method.
                        reply(std::make_unique<regular::responses::StringResponse>(ResponseCode::Method_Not_Allowed), false);
                    }
                }
            }
        }
    }

    void HTTPServerClient::websocket_event(const smooth::core::network::event::DataAvailableEvent<HTTPProtocol>& event)
    {
        typename HTTPProtocol::packet_type packet;
        if (event.get(packet))
        {
            auto ws_op = packet.ws_control_code();
            if (ws_op >= WebsocketProtocol::OpCode::Close)
            {
                if (ws_op == WebsocketProtocol::OpCode::Close)
                {
                    close();
                }
                else if (ws_op == WebsocketProtocol::OpCode::Ping)
                {
                    // Reply with a ping and place it first in the queue.
                    reply(std::make_unique<WSResponse>(WebsocketProtocol::OpCode::Pong), true);
                }
            }
            else
            {
                if(ws_server)
                {
                    bool first_part = !packet.is_continuation();
                    bool last_part = !packet.is_continued();
                    const auto& data = packet.data();
                    ws_server->data_received(first_part, last_part, packet.ws_control_code() == WebsocketProtocol::OpCode::Text, data);
                }
            }
        }
    }
}