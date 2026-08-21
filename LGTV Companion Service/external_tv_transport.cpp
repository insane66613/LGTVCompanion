#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00

#include "external_tv_transport.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <thread>
#include <vector>

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace ssl = asio::ssl;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

namespace {
std::string samsungKeyName(SamsungRemoteKey key) {
    switch (key) {
    case SamsungRemoteKey::Accessibility: return "KEY_AD";
    case SamsungRemoteKey::Enter: return "KEY_ENTER";
    case SamsungRemoteKey::Return: return "KEY_RETURN";
    case SamsungRemoteKey::Power: return "KEY_POWER";
    }
    return "";
}

std::string samsungPayload(SamsungRemoteKey key) {
    nlohmann::json payload = {
        {"method", "ms.remote.control"},
        {"params", {
            {"Cmd", "Click"},
            {"DataOfCmd", samsungKeyName(key)},
            {"Option", "false"},
            {"TypeOfRemote", "SendRemoteKey"}
        }}
    };
    return payload.dump();
}

std::string samsungTarget(const std::string& token) {
    std::string target = "/api/v2/channels/samsung.remote.control?name=TEdUViBDb21wYW5pb24=";
    if (!token.empty()) target += "&token=" + token;
    return target;
}

template <typename StartOperation>
boost::system::error_code runExternalTvAsync(asio::io_context& ioc, StartOperation&& start) {
    boost::system::error_code result = asio::error::would_block;
    bool completed = false;
    start([&](const boost::system::error_code& ec, auto&&...) {
        result = ec;
        completed = true;
    });
    ioc.restart();
    ioc.run();
    if (!completed) return asio::error::operation_aborted;
    return result;
}

template <typename WebSocket>
bool waitForSamsungAuthorization(WebSocket& ws, asio::io_context& ioc, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    beast::flat_buffer buffer;
    while (std::chrono::steady_clock::now() < deadline) {
        beast::get_lowest_layer(ws).expires_at(deadline);
        const auto ec = runExternalTvAsync(ioc, [&](auto done) {
            ws.async_read(buffer, std::move(done));
        });
        if (ec) {
            error = ec == beast::error::timeout
                ? "Samsung authorization timed out"
                : "Samsung authorization read failed: " + ec.message();
            return false;
        }
        const auto message = beast::buffers_to_string(buffer.data());
        buffer.consume(buffer.size());
        const auto authorization = SamsungController::channelAuthorizationFromJson(message);
        if (authorization == SamsungChannelAuthorization::Authorized) {
            error.clear();
            return true;
        }
        if (authorization == SamsungChannelAuthorization::Unauthorized) {
            error = "Samsung rejected remote-control authorization";
            return false;
        }
    }
    error = "Samsung did not send ms.channel.connect before authorization deadline";
    return false;
}

template <typename WebSocket>
bool writeSamsungPayload(WebSocket& ws, asio::io_context& ioc,
                         const std::string& payload, std::string& error) {
    beast::get_lowest_layer(ws).expires_after(5s);
    const auto ec = runExternalTvAsync(ioc, [&](auto done) {
        ws.async_write(asio::buffer(payload), std::move(done));
    });
    if (ec) {
        error = "Samsung key write failed: " + ec.message();
        return false;
    }
    return true;
}

template <typename WebSocket>
void closeSamsungWebSocket(WebSocket& ws, asio::io_context& ioc) {
    auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
    timeout.handshake_timeout = 1500ms;
    ws.set_option(timeout);
    beast::get_lowest_layer(ws).expires_after(1500ms);
    const auto ec = runExternalTvAsync(ioc, [&](auto done) {
        ws.async_close(websocket::close_code::normal, std::move(done));
    });
    if (ec) {
        beast::error_code ignored;
        beast::get_lowest_layer(ws).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(ws).socket().close(ignored);
    }
}

bool findStringKey(const nlohmann::json& node, const std::string& key, std::string& value) {
    if (node.is_object()) {
        const auto it = node.find(key);
        if (it != node.end() && it->is_string()) {
            value = it->get<std::string>();
            return true;
        }
        for (const auto& item : node.items()) {
            if (findStringKey(item.value(), key, value)) return true;
        }
    } else if (node.is_array()) {
        for (const auto& item : node) {
            if (findStringKey(item, key, value)) return true;
        }
    }
    return false;
}

bool findPowerValue(const nlohmann::json& node, int& value) {
    if (node.is_object()) {
        for (const char* key : {"VALUE", "value"}) {
            const auto it = node.find(key);
            if (it != node.end()) {
                if (it->is_number_integer()) {
                    value = it->get<int>();
                    return true;
                }
                if (it->is_boolean()) {
                    value = it->get<bool>() ? 1 : 0;
                    return true;
                }
            }
        }
        for (const auto& item : node.items()) {
            if (findPowerValue(item.value(), value)) return true;
        }
    } else if (node.is_array()) {
        for (const auto& item : node) {
            if (findPowerValue(item, value)) return true;
        }
    }
    return false;
}

bool parseMac(const std::string& text, std::array<unsigned char, 6>& mac) {
    std::string hex;
    for (const char ch : text) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) hex.push_back(ch);
    }
    if (hex.size() != 12) return false;
    try {
        for (std::size_t i = 0; i < 6; ++i)
            mac[i] = static_cast<unsigned char>(std::stoul(hex.substr(i * 2, 2), nullptr, 16));
        return true;
    } catch (...) {
        return false;
    }
}

bool sendWol(const std::string& mac_text, std::string& error) {
    std::array<unsigned char, 6> mac{};
    if (!parseMac(mac_text, mac)) {
        error = "invalid MAC address";
        return false;
    }
    std::array<unsigned char, 102> packet{};
    packet.fill(0xFF);
    for (std::size_t i = 6; i < packet.size(); ++i) packet[i] = mac[(i - 6) % 6];
    try {
        asio::io_context ioc;
        asio::ip::udp::socket socket(ioc);
        socket.open(asio::ip::udp::v4());
        socket.set_option(asio::socket_base::broadcast(true));
        socket.send_to(asio::buffer(packet),
                       asio::ip::udp::endpoint(asio::ip::address_v4::broadcast(), 9));
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
}

SamsungTizenTransport::SamsungTizenTransport(SamsungExternalTvSettings settings)
    : settings_(std::move(settings)) {}

SamsungPowerState SamsungTizenTransport::queryPowerState(std::string& error) const {
    try {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        beast::tcp_stream stream(ioc);
        const auto endpoints = resolver.resolve(settings_.ip, "8001");
        const auto deadline = std::chrono::steady_clock::now() + 4s;

        stream.expires_at(deadline);
        auto ec = runExternalTvAsync(ioc, [&](auto done) {
            stream.async_connect(endpoints, std::move(done));
        });
        if (ec) {
            error = "Samsung state probe connect failed: " + ec.message();
            return SamsungPowerState::Unknown;
        }

        http::request<http::empty_body> req{http::verb::get, "/api/v2/", 11};
        req.set(http::field::host, settings_.ip);
        req.set(http::field::user_agent, "LGTVCompanion");
        stream.expires_at(deadline);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            http::async_write(stream, req, std::move(done));
        });
        if (ec) {
            error = "Samsung state probe write failed: " + ec.message();
            return SamsungPowerState::Unknown;
        }

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        stream.expires_at(deadline);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            http::async_read(stream, buffer, res, std::move(done));
        });
        if (ec) {
            error = "Samsung state probe read failed: " + ec.message();
            return SamsungPowerState::Unknown;
        }

        beast::error_code ignored;
        stream.socket().shutdown(tcp::socket::shutdown_both, ignored);
        stream.socket().close(ignored);
        if (res.result_int() < 200 || res.result_int() >= 300) {
            error = "Samsung state probe HTTP " + std::to_string(res.result_int());
            return SamsungPowerState::Unknown;
        }
        const auto json = nlohmann::json::parse(res.body());
        std::string state;
        if (!findStringKey(json, "PowerState", state)) {
            error.clear();
            return SamsungPowerState::ReachableNoState;
        }
        for (auto& c : state) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        error.clear();
        if (state == "on") return SamsungPowerState::On;
        if (state == "pictureoff") return SamsungPowerState::PictureOff;
        if (state == "standby") return SamsungPowerState::Standby;
        return SamsungPowerState::ReachableNoState;
    } catch (const std::exception& e) {
        error = e.what();
        return SamsungPowerState::Unknown;
    }
}

bool SamsungTizenTransport::sendKeyTls(SamsungRemoteKey key, std::string& error) const {
    try {
        asio::io_context ioc;
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_verify_mode(ssl::verify_none);
        tcp::resolver resolver(ioc);
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ctx);
        const auto endpoints = resolver.resolve(settings_.ip, "8002");

        beast::get_lowest_layer(ws).expires_after(5s);
        auto ec = runExternalTvAsync(ioc, [&](auto done) {
            beast::get_lowest_layer(ws).async_connect(endpoints, std::move(done));
        });
        if (ec) {
            error = "Samsung WSS connect failed: " + ec.message();
            return false;
        }

        beast::get_lowest_layer(ws).expires_after(5s);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            ws.next_layer().async_handshake(ssl::stream_base::client, std::move(done));
        });
        if (ec) {
            error = "Samsung TLS handshake failed: " + ec.message();
            return false;
        }

        auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
        timeout.handshake_timeout = 5s;
        ws.set_option(timeout);
        beast::get_lowest_layer(ws).expires_after(5s);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            ws.async_handshake(settings_.ip, samsungTarget(settings_.token), std::move(done));
        });
        if (ec) {
            error = "Samsung WebSocket handshake failed: " + ec.message();
            return false;
        }

        if (!waitForSamsungAuthorization(ws, ioc, error)) return false;
        const auto payload = samsungPayload(key);
        if (!writeSamsungPayload(ws, ioc, payload, error)) return false;
        closeSamsungWebSocket(ws, ioc);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool SamsungTizenTransport::sendKeyPlain(SamsungRemoteKey key, std::string& error) const {
    try {
        asio::io_context ioc;
        tcp::resolver resolver(ioc);
        websocket::stream<beast::tcp_stream> ws(ioc);
        const auto endpoints = resolver.resolve(settings_.ip, "8001");

        beast::get_lowest_layer(ws).expires_after(5s);
        auto ec = runExternalTvAsync(ioc, [&](auto done) {
            beast::get_lowest_layer(ws).async_connect(endpoints, std::move(done));
        });
        if (ec) {
            error = "Samsung WS connect failed: " + ec.message();
            return false;
        }

        auto timeout = websocket::stream_base::timeout::suggested(beast::role_type::client);
        timeout.handshake_timeout = 5s;
        ws.set_option(timeout);
        beast::get_lowest_layer(ws).expires_after(5s);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            ws.async_handshake(settings_.ip, samsungTarget(""), std::move(done));
        });
        if (ec) {
            error = "Samsung WebSocket handshake failed: " + ec.message();
            return false;
        }

        if (!waitForSamsungAuthorization(ws, ioc, error)) return false;
        const auto payload = samsungPayload(key);
        if (!writeSamsungPayload(ws, ioc, payload, error)) return false;
        closeSamsungWebSocket(ws, ioc);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool SamsungTizenTransport::sendKey(SamsungRemoteKey key, std::string& error) const {
    std::string tls_error;
    if (sendKeyTls(key, tls_error)) return true;
    std::string plain_error;
    if (sendKeyPlain(key, plain_error)) return true;
    error = "WSS 8002: " + tls_error + "; WS 8001: " + plain_error;
    return false;
}

bool SamsungTizenTransport::wake(std::string& error) const {
    return sendWol(settings_.mac, error);
}

VizioSmartCastTransport::VizioSmartCastTransport(VizioExternalTvSettings settings)
    : settings_(std::move(settings)) {}

bool VizioSmartCastTransport::request(const std::string& method, const std::string& target,
                                      const std::string& body, std::string& response,
                                      std::string& error) const {
    try {
        asio::io_context ioc;
        ssl::context ctx(ssl::context::tls_client);
        ctx.set_verify_mode(ssl::verify_none);
        tcp::resolver resolver(ioc);
        beast::ssl_stream<beast::tcp_stream> stream(ioc, ctx);
        const auto endpoints = resolver.resolve(settings_.ip, "7345");
        const auto deadline = std::chrono::steady_clock::now() + 5s;

        beast::get_lowest_layer(stream).expires_at(deadline);
        auto ec = runExternalTvAsync(ioc, [&](auto done) {
            beast::get_lowest_layer(stream).async_connect(endpoints, std::move(done));
        });
        if (ec) {
            error = "Vizio SmartCast connect failed: " + ec.message();
            return false;
        }

        beast::get_lowest_layer(stream).expires_at(deadline);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            stream.async_handshake(ssl::stream_base::client, std::move(done));
        });
        if (ec) {
            error = "Vizio SmartCast TLS handshake failed: " + ec.message();
            return false;
        }

        http::request<http::string_body> req;
        req.method(method == "GET" ? http::verb::get : http::verb::put);
        req.target(target);
        req.version(11);
        req.set(http::field::host, settings_.ip);
        // Match the proven SmartCast client envelope. This firmware returns
        // RESULT=SUCCESS yet can discard key commands when these ordinary
        // HTTP/1.1 client headers are omitted.
        req.set(http::field::user_agent, "python-requests/2.31.0");
        req.set(http::field::accept, "*/*");
        req.set(http::field::accept_encoding, "gzip, deflate");
        req.set(http::field::connection, "keep-alive");
        req.set("AUTH", settings_.auth);
        if (method != "GET") {
            req.set(http::field::content_type, "application/json");
            req.body() = body;
            req.prepare_payload();
        }

        beast::get_lowest_layer(stream).expires_at(deadline);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            http::async_write(stream, req, std::move(done));
        });
        if (ec) {
            error = "Vizio SmartCast write failed: " + ec.message();
            return false;
        }

        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        beast::get_lowest_layer(stream).expires_at(deadline);
        ec = runExternalTvAsync(ioc, [&](auto done) {
            http::async_read(stream, buffer, res, std::move(done));
        });
        if (ec) {
            error = "Vizio SmartCast read failed: " + ec.message();
            return false;
        }
        response = res.body();
        if (res.result_int() < 200 || res.result_int() >= 300) {
            error = "Vizio SmartCast HTTP " + std::to_string(res.result_int());
            return false;
        }
        try {
            const auto json = nlohmann::json::parse(response);
            std::string result;
            if (!findStringKey(json, "RESULT", result) && !findStringKey(json, "result", result)) {
                error = "Vizio SmartCast response missing STATUS/RESULT";
                return false;
            }
            for (auto& c : result)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (result != "success") {
                error = "Vizio SmartCast RESULT=" + result;
                return false;
            }
        } catch (const std::exception& e) {
            error = std::string("Vizio SmartCast invalid JSON: ") + e.what();
            return false;
        }

        // This firmware can discard an acknowledged key command if TLS is closed
        // immediately. Preserve the proven 2-second hold, then force-close without
        // close_notify. Other requests use a bounded asynchronous TLS shutdown.
        if (method != "GET" && target == "/key_command/") {
            std::this_thread::sleep_for(2000ms);
        } else {
            beast::get_lowest_layer(stream).expires_after(1500ms);
            runExternalTvAsync(ioc, [&](auto done) {
                stream.async_shutdown(std::move(done));
            });
        }
        beast::error_code ignored;
        beast::get_lowest_layer(stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
        beast::get_lowest_layer(stream).socket().close(ignored);
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

VizioPowerState VizioSmartCastTransport::queryPowerState(std::string& error) const {
    std::string response;
    if (!request("GET", "/state/device/power_mode", "", response, error))
        return VizioPowerState::Unknown;
    try {
        const auto json = nlohmann::json::parse(response);
        int value = 0;
        if (!findPowerValue(json, value)) {
            error = "Vizio power_mode VALUE missing";
            return VizioPowerState::Unknown;
        }
        error.clear();
        return VizioController::fromSmartCastPowerValue(value);
    } catch (const std::exception& e) {
        error = std::string("Vizio power_mode invalid JSON: ") + e.what();
        return VizioPowerState::Unknown;
    }
}

bool VizioSmartCastTransport::blankPanel(std::string& error) const {
    std::string response;
    const std::string path = "/menu_native/dynamic/tv_settings/timers/blank_screen";
    if (!request("GET", path, "", response, error)) return false;
    const auto hashval = VizioController::smartCastHashValFromJson(response);
    if (!hashval) {
        error = "Vizio blank-screen HASHVAL missing or invalid";
        return false;
    }
    nlohmann::json payload = {{"REQUEST", "ACTION"}, {"HASHVAL", *hashval}};
    return request("PUT", path, payload.dump(), response, error);
}

bool VizioSmartCastTransport::keyPress(int codeset, int code, std::string& error) const {
    std::string response;
    return request("PUT", "/key_command/", VizioController::smartCastKeyPayload(codeset, code), response, error);
}

bool VizioSmartCastTransport::unblankPanel(std::string& error) const {
    return keyPress(4, 0, error);  // BACK wakes a blanked panel without toggling power.
}

bool VizioSmartCastTransport::powerOff(std::string& error) const {
    std::string state_error;
    auto state = queryPowerState(state_error);
    if (state == VizioPowerState::Off) {
        error.clear();
        return true;
    }
    if (!VizioController::shouldSendPowerOff(state)) {
        error = "Vizio power state unknown; POW_OFF suppressed: " + state_error;
        return false;
    }

    const auto initial_state = state;
    std::string last_state_error;
    for (int command_attempt = 0; command_attempt < 2; ++command_attempt) {
        std::string key_error;
        if (!keyPress(11, 0, key_error)) {
            error = "Vizio POW_OFF command failed: " + key_error;
            return false;
        }

        for (int verify_attempt = 0; verify_attempt < 8; ++verify_attempt) {
            std::this_thread::sleep_for(1000ms);
            state = queryPowerState(last_state_error);
            if (state == VizioPowerState::Off) {
                error.clear();
                return true;
            }
        }

        // This model's nominal POW_OFF behaves toggle-like when repeated, so a
        // retry is safe only after the entire verification window still proves
        // that the first command left the set ON. OFF and UNKNOWN suppress it.
        if (command_attempt == 0 &&
            VizioController::shouldRetryPowerOffAfterVerification(initial_state, state)) {
            continue;
        }
        break;
    }

    if (state == VizioPowerState::On) {
        error = "Vizio POW_OFF accepted but device remained ON after guarded retry";
    } else {
        error = "Vizio POW_OFF accepted but OFF state was not verified: " + last_state_error;
    }
    return false;
}

bool VizioSmartCastTransport::wake(std::string& error) const {
    return sendWol(settings_.mac, error);
}

bool VizioSmartCastTransport::powerOn(std::string& error) const {
    std::string wol_error;
    wake(wol_error);

    // Match the reference controller semantics: WOL is only a trigger, never
    // proof that the TV is on. Verify authenticated SmartCast state first.
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (attempt > 0) std::this_thread::sleep_for(2500ms);
        std::string state_error;
        if (queryPowerState(state_error) == VizioPowerState::On) {
            error.clear();
            return true;
        }
    }

    std::string key_error;
    if (!keyPress(11, 1, key_error)) {
        error = "WOL: " + wol_error + "; SmartCast POW_ON: " + key_error;
        return false;
    }

    // Explicit power-on may take time to settle. Bound verification rather
    // than treating command acceptance as final device state.
    for (int attempt = 0; attempt < 12; ++attempt) {
        std::this_thread::sleep_for(2500ms);
        std::string state_error;
        if (queryPowerState(state_error) == VizioPowerState::On) {
            error.clear();
            return true;
        }
        if (attempt == 11)
            error = "Vizio explicit power-on accepted but ON state was not verified: " + state_error;
    }
    return false;
}
