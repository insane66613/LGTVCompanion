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

bool findIntegerKey(const nlohmann::json& node, const std::string& key, int& value) {
    if (node.is_object()) {
        const auto it = node.find(key);
        if (it != node.end() && it->is_number_integer()) {
            value = it->get<int>();
            return true;
        }
        for (const auto& item : node.items()) {
            if (findIntegerKey(item.value(), key, value)) return true;
        }
    } else if (node.is_array()) {
        for (const auto& item : node) {
            if (findIntegerKey(item, key, value)) return true;
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
        stream.expires_after(4s);
        stream.connect(resolver.resolve(settings_.ip, "8001"));
        http::request<http::empty_body> req{http::verb::get, "/api/v2/", 11};
        req.set(http::field::host, settings_.ip);
        req.set(http::field::user_agent, "LGTVCompanion");
        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        beast::error_code ec;
        stream.socket().shutdown(tcp::socket::shutdown_both, ec);
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
        beast::get_lowest_layer(ws).expires_after(5s);
        beast::get_lowest_layer(ws).connect(resolver.resolve(settings_.ip, "8002"));
        ws.next_layer().handshake(ssl::stream_base::client);
        ws.handshake(settings_.ip, samsungTarget(settings_.token));
        const auto payload = samsungPayload(key);
        ws.write(asio::buffer(payload));
        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
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
        beast::get_lowest_layer(ws).expires_after(5s);
        beast::get_lowest_layer(ws).connect(resolver.resolve(settings_.ip, "8001"));
        ws.handshake(settings_.ip, samsungTarget(""));
        const auto payload = samsungPayload(key);
        ws.write(asio::buffer(payload));
        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
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
        beast::get_lowest_layer(stream).expires_after(5s);
        beast::get_lowest_layer(stream).connect(resolver.resolve(settings_.ip, "7345"));
        stream.handshake(ssl::stream_base::client);

        http::request<http::string_body> req;
        req.method(method == "GET" ? http::verb::get : http::verb::put);
        req.target(target);
        req.version(11);
        req.set(http::field::host, settings_.ip);
        req.set(http::field::user_agent, "LGTVCompanion");
        req.set("AUTH", settings_.auth);
        if (method != "GET") {
            req.set(http::field::content_type, "application/json");
            req.body() = body;
            req.prepare_payload();
        }
        http::write(stream, req);
        beast::flat_buffer buffer;
        http::response<http::string_body> res;
        http::read(stream, buffer, res);
        beast::error_code ec;
        stream.shutdown(ec);
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
    int hashval = 0;
    try {
        const auto json = nlohmann::json::parse(response);
        if (!findIntegerKey(json, "HASHVAL", hashval) && !findIntegerKey(json, "hashval", hashval)) {
            error = "Vizio blank-screen HASHVAL missing";
            return false;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    nlohmann::json payload = {{"REQUEST", "ACTION"}, {"HASHVAL", hashval}};
    return request("PUT", path, payload.dump(), response, error);
}

bool VizioSmartCastTransport::keyPress(int codeset, int code, std::string& error) const {
    nlohmann::json payload = {{"KEYLIST", nlohmann::json::array({{
        {"CODESET", codeset}, {"CODE", code}, {"ACTION", "KEYPRESS"}
    }})}};
    std::string response;
    return request("PUT", "/key_command/", payload.dump(), response, error);
}

bool VizioSmartCastTransport::unblankPanel(std::string& error) const {
    return keyPress(4, 0, error);  // BACK wakes a blanked panel without toggling power.
}

bool VizioSmartCastTransport::powerOff(std::string& error) const {
    return keyPress(11, 0, error); // Explicit OFF, not POW_TOGGLE.
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
