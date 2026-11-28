"""Focused host checks for the dinosaur ML307 HTTP transport."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DINO = ROOT / 'main/boards/wzh_dinosaur'


class DinosaurMl307HttpTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which('g++'), 'g++ required')
    def test_request_framing_and_fragmented_response_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for name in ('driver', 'esp_adc'):
                (path / name).mkdir()
            (path / 'driver/gpio.h').write_text('''#pragma once
using gpio_num_t = int;
#define GPIO_NUM_NC -1
''' + '\n'.join(f'#define GPIO_NUM_{i} {i}' for i in range(49)) + '\n')
            (path / 'driver/ledc.h').write_text('#pragma once\n')
            (path / 'driver/spi_master.h').write_text('#pragma once\n')
            (path / 'esp_adc/adc_oneshot.h').write_text('#pragma once\n')
            (path / 'sdkconfig.h').write_text('#pragma once\n')
            (path / 'esp_log.h').write_text('''#pragma once
template <class... T> void log_stub(T...) {}
#define ESP_LOGI(...) log_stub(__VA_ARGS__)
#define ESP_LOGW(...) log_stub(__VA_ARGS__)
#define ESP_LOGE(...) log_stub(__VA_ARGS__)
#define ESP_LOGD(...) log_stub(__VA_ARGS__)
''')
            (path / 'freertos').mkdir()
            (path / 'freertos/FreeRTOS.h').write_text('''#pragma once
#define pdMS_TO_TICKS(ms) (ms)
''')
            (path / 'freertos/task.h').write_text('''#pragma once
inline void taskYIELD() {}
inline void vTaskDelay(int) {}
''')
            (path / 'http.h').write_text('''#pragma once
#include <cstddef>
#include <string>
#include "network_error.h"
class Http {
public:
    virtual ~Http() = default;
    virtual void SetTimeout(int) = 0;
    virtual void SetHeader(const std::string&, const std::string&) = 0;
    virtual void SetContent(std::string&&) = 0;
    virtual void SetKeepAlive(bool) = 0;
    virtual NetworkResult<> Open(const std::string&, const std::string&) = 0;
    virtual void Close() = 0;
    virtual NetworkResult<int> Read(char*, size_t) = 0;
    virtual NetworkResult<int> Write(const char*, size_t) = 0;
    virtual NetworkResult<int> GetStatusCode() = 0;
    virtual std::string GetResponseHeader(const std::string&) const = 0;
    virtual size_t GetBodyLength() = 0;
    virtual std::string ReadAll() = 0;
protected:
    NetworkResult<> Fail(NetworkError error) {
        last_error_ = error;
        return std::unexpected(error);
    }
    template <typename T>
    NetworkResult<T> FailValue(NetworkError error) {
        last_error_ = error;
        return std::unexpected(error);
    }
    NetworkError last_error_{};
};
''')
            (path / 'at_uart.h').write_text('''#pragma once
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "at_error.h"

struct AtArgumentValue {
    enum class Type { String, Int, Double };
    Type type = Type::String;
    std::string string_value;
    int int_value = 0;
    double double_value = 0;
    std::string ToString() const {
        if (type == Type::Int) return std::to_string(int_value);
        if (type == Type::Double) return std::to_string(double_value);
        return "\\"" + string_value + "\\"";
    }
};

using UrcCallback = std::function<void(const std::string&,
                                       const std::vector<AtArgumentValue>&)>;

class AtUart {
public:
    int GetBaudRate() const { return 115200; }

    AtResult SendCommand(const std::string& command, size_t = 3000, bool = true) {
        commands.push_back(command);
        if (command == "AT") {
            if (at_probe_failures > 0) {
                --at_probe_failures;
                return std::unexpected(AtError{AtErrc::Timeout, 0, 0});
            }
            return {};
        }
        if (command.rfind("AT+MHTTPCREATE=", 0) == 0) {
            if (create_timeout_once) {
                create_timeout_once = false;
                return std::unexpected(AtError{AtErrc::Timeout, 0, 0});
            }
            if (!suppress_create_urc)
                Emit("MHTTPCREATE", {IntValue(next_create_id)});
        } else if (command.rfind("AT+MHTTPDEL=", 0) == 0) {
            if (delete_timeout_once) {
                delete_timeout_once = false;
                return std::unexpected(AtError{AtErrc::Timeout, 0, 0});
            }
        } else if (command.rfind("AT+MHTTPREQUEST=", 0) == 0) {
            Emit("MHTTPURC", {StringValue("ind"), IntValue(0)});
        }
        return {};
    }

    AtResult SendCommandWithData(const std::string& command, size_t, bool,
                                 const char* data = nullptr, size_t length = 0) {
        commands.push_back(command);
        data_commands.push_back(command);
        data_payloads.emplace_back(data ? std::string(data, length) : std::string());
        return {};
    }

    std::string EncodeHex(const std::string& data) {
        std::string encoded;
        EncodeHexAppend(encoded, data.data(), data.size());
        return encoded;
    }

    void EncodeHexAppend(std::string& out, const char* data, size_t length) {
        static const char kHex[] = "0123456789ABCDEF";
        for (size_t i = 0; i < length; ++i) {
            const unsigned char value = static_cast<unsigned char>(data[i]);
            out.push_back(kHex[value >> 4]);
            out.push_back(kHex[value & 0x0F]);
        }
    }

    void DecodeHexAppend(std::string& out, const char* data, size_t length) {
        auto digit = [](char ch) {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return 0;
        };
        for (size_t i = 0; i + 1 < length; i += 2) {
            out.push_back(static_cast<char>((digit(data[i]) << 4) | digit(data[i + 1])));
        }
    }

    std::list<UrcCallback>::iterator RegisterUrcCallback(UrcCallback callback) {
        return callbacks.insert(callbacks.end(), std::move(callback));
    }

    void UnregisterUrcCallback(std::list<UrcCallback>::iterator iterator) {
        callbacks.erase(iterator);
    }

    void Emit(const std::string& command, const std::vector<AtArgumentValue>& arguments) {
        const auto snapshot = callbacks;
        for (const auto& callback : snapshot)
            callback(command, arguments);
    }

    static AtArgumentValue StringValue(const std::string& value) {
        AtArgumentValue argument;
        argument.type = AtArgumentValue::Type::String;
        argument.string_value = value;
        return argument;
    }

    static AtArgumentValue IntValue(int value) {
        AtArgumentValue argument;
        argument.type = AtArgumentValue::Type::Int;
        argument.int_value = value;
        argument.string_value = std::to_string(value);
        return argument;
    }

    std::vector<std::string> commands;
    std::vector<std::string> data_commands;
    std::vector<std::string> data_payloads;
    bool create_timeout_once = false;
    bool delete_timeout_once = false;
    bool suppress_create_urc = false;
    int at_probe_failures = 0;
    int next_create_id = 0;

private:
    std::list<UrcCallback> callbacks;
};
''')
            (path / 'test.cc').write_text(r'''
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "at_uart.h"
#include "dinosaur_ml307_http.cc"

using Args = std::vector<AtArgumentValue>;

static AtArgumentValue Str(const std::string& value) {
    return AtUart::StringValue(value);
}

static AtArgumentValue Int(int value) {
    return AtUart::IntValue(value);
}

static void Reset(const std::shared_ptr<AtUart>& uart) {
    uart->commands.clear();
    uart->data_commands.clear();
    uart->data_payloads.clear();
}

static bool Has(const std::vector<std::string>& values, const std::string& value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

static bool HasPrefix(const std::vector<std::string>& values, const std::string& prefix) {
    return std::any_of(values.begin(), values.end(), [&](const std::string& value) {
        return value.rfind(prefix, 0) == 0;
    });
}

static size_t IndexOfPrefix(const std::vector<std::string>& values, const std::string& prefix) {
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i].rfind(prefix, 0) == 0)
            return i;
    }
    return std::string::npos;
}

static size_t CountPrefix(const std::vector<std::string>& values, const std::string& prefix) {
    return std::count_if(values.begin(), values.end(), [&](const std::string& value) {
        return value.rfind(prefix, 0) == 0;
    });
}

static void EmitHeader(AtUart& uart, int status, const std::string& data, size_t total) {
    std::string encoded;
    uart.EncodeHexAppend(encoded, data.data(), data.size());
    uart.Emit("MHTTPURC", {Str("header"), Int(0), Int(status), Int(static_cast<int>(total)),
                           Str(encoded)});
}

static void EmitContent(AtUart& uart, const std::string& data) {
    std::string encoded;
    uart.EncodeHexAppend(encoded, data.data(), data.size());
    uart.Emit("MHTTPURC", {Str("content"), Int(0), Int(static_cast<int>(data.size())),
                           Int(static_cast<int>(data.size())),
                           Int(static_cast<int>(data.size())), Str(encoded)});
}

template <typename T>
T Value(const NetworkResult<T>& result) {
    assert(result);
    return *result;
}

int main() {
    auto uart = std::make_shared<AtUart>();

    // A fixed-length body is staged in 4 KiB pieces. Every piece except the
    // last advertises more content; the final piece closes the content input.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        http.SetHeader("X-One", "1");
        http.SetHeader("X-Two", "2");
        http.SetContent(std::string(5000, 'a'));
        assert(http.Open("POST", "http://example.com/post"));
        assert(Has(uart->commands, "AT+MHTTPHEADER=0,1,8,\"X-One: 1\""));
        assert(Has(uart->commands, "AT+MHTTPHEADER=0,1,8,\"X-Two: 2\""));
        assert(Has(uart->commands, "AT+MHTTPHEADER=0,0,20,\"Content-Length: 5000\""));
        assert(Has(uart->commands, "AT+MHTTPCONTENT=0,1,4096"));
        assert(Has(uart->commands, "AT+MHTTPCONTENT=0,0,904"));
        assert(uart->data_payloads.size() == 2);
        assert(uart->data_payloads[0] == std::string(4096, 'a'));
        assert(uart->data_payloads[1] == std::string(904, 'a'));
        http.Close();
    }

    // A timed-out allocation must not leave an id that a later request can
    // mistake for its own. The next Open synchronises the parser before
    // MHTTPCREATE and removes every slot this process does not own.
    {
        Reset(uart);
        uart->create_timeout_once = true;
        {
            DinosaurMl307Http http(uart);
            auto opened = http.Open("GET", "http://example.com/timeout");
            assert(!opened && opened.error().code == NetworkErrc::Timeout);
        }
        assert(Has(uart->commands, "AT"));
        assert(Has(uart->commands, "AT+MHTTPDEL=0"));

        // A delayed +MHTTPCREATE arriving after the adapter is gone must not
        // issue AT traffic from the callback. The next allocation sweeps it.
        const size_t before_late_urc = uart->commands.size();
        uart->Emit("MHTTPCREATE", {Int(2)});
        assert(uart->commands.size() == before_late_urc);

        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/after-timeout"));
        const size_t delete_pos = IndexOfPrefix(uart->commands, "AT+MHTTPDEL=2");
        const size_t create_pos = IndexOfPrefix(uart->commands, "AT+MHTTPCREATE=");
        assert(delete_pos != std::string::npos);
        assert(create_pos != std::string::npos);
        assert(delete_pos < create_pos);
        http.Close();
    }

    // A timed-out MHTTPDEL lets one delayed AT probe fail before the parser
    // is resynchronised for MQTT. A later HTTP allocation still sweeps the
    // modem slot because the original delete was never acknowledged.
    {
        Reset(uart);
        uart->delete_timeout_once = true;
        uart->at_probe_failures = 1;
        {
            DinosaurMl307Http http(uart);
            assert(http.Open("GET", "http://example.com/close-timeout"));
            const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
            EmitHeader(*uart, 200, headers, headers.size());
            EmitContent(*uart, "hello");
            assert(http.ReadAll() == "hello");
            http.Close();
        }
        const size_t close_pos = IndexOfPrefix(uart->commands, "AT+MHTTPDEL=");
        assert(close_pos != std::string::npos);
        assert(!HasPrefix(uart->commands, "AT+MHTTPTERM="));
        assert(std::count(uart->commands.begin() + close_pos + 1, uart->commands.end(), "AT") == 3);
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/after-close-timeout"));
        const auto at_pos = std::find(uart->commands.begin(), uart->commands.end(), "AT");
        const size_t delete_pos = IndexOfPrefix(uart->commands, "AT+MHTTPDEL=");
        const size_t create_pos = IndexOfPrefix(uart->commands, "AT+MHTTPCREATE=");
        assert(at_pos != uart->commands.end());
        assert(delete_pos != std::string::npos);
        assert(create_pos != std::string::npos);
        assert(static_cast<size_t>(at_pos - uart->commands.begin()) < delete_pos);
        assert(delete_pos < create_pos);
        assert(CountPrefix(uart->commands, "AT+MHTTPDEL=") == 4);
        http.Close();
    }

    // If the parser cannot be synchronised after a timeout, refuse to allocate
    // another modem slot until a later attempt proves the AT channel is sane.
    {
        Reset(uart);
        uart->create_timeout_once = true;
        uart->at_probe_failures = 2;
        DinosaurMl307Http http(uart);
        auto first = http.Open("GET", "http://example.com/unsynced");
        assert(!first && first.error().code == NetworkErrc::Timeout);
        assert(CountPrefix(uart->commands, "AT+MHTTPCREATE=") == 1);

        Reset(uart);
        uart->at_probe_failures = 1;
        auto second = http.Open("GET", "http://example.com/still-unsynced");
        assert(!second && second.error().code == NetworkErrc::Timeout);
        assert(!HasPrefix(uart->commands, "AT+MHTTPCREATE="));

        Reset(uart);
        assert(http.Open("GET", "http://example.com/recovered"));
        assert(CountPrefix(uart->commands, "AT+MHTTPCREATE=") == 1);
        http.Close();
    }

    // A stray create URC must not be claimed by an unrelated live instance or
    // turn into an AT command inside the callback.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/get"));
        const size_t before = uart->commands.size();
        uart->Emit("MHTTPCREATE", {Int(2)});
        assert(uart->commands.size() == before);
        http.Close();
    }

    // An explicitly declared Content-Length must match the body and must not
    // be duplicated by the automatic framing header.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        http.SetHeader("Content-Length", "3");
        http.SetContent("abc");
        assert(http.Open("POST", "http://example.com/post"));
        assert(Has(uart->commands, "AT+MHTTPHEADER=0,0,17,\"Content-Length: 3\""));
        assert(std::count_if(uart->commands.begin(), uart->commands.end(),
                             [](const std::string& value) {
                                 return value.find("\"Content-Length:") != std::string::npos;
                             }) == 1);
        http.Close();
    }

    // Reject a mismatched length and case-insensitive duplicate header names
    // before the modem sees a request.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        http.SetHeader("Content-Length", "5");
        http.SetContent("abc");
        auto opened = http.Open("POST", "http://example.com/post");
        assert(!opened && opened.error().code == NetworkErrc::InvalidArgument);
        assert(!HasPrefix(uart->commands, "AT+MHTTPREQUEST="));
    }
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        http.SetHeader("X-Test", "a");
        http.SetHeader("x-test", "b");
        auto opened = http.Open("GET", "http://example.com/get");
        assert(!opened && opened.error().code == NetworkErrc::InvalidArgument);
        assert(!HasPrefix(uart->commands, "AT+MHTTPREQUEST="));
    }

    // A GET with no caller headers needs no MHTTPHEADER command.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/get"));
        assert(!HasPrefix(uart->commands, "AT+MHTTPHEADER="));
        assert(HasPrefix(uart->commands, "AT+MHTTPREQUEST=0,1,0,"));
        http.Close();
    }

    // Response headers are case-insensitive, duplicate fields are merged, and
    // the body is delivered after the final content URC.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/get"));
        const std::string headers =
            "HTTP/1.1 200 OK\r\nX-Test: a\r\nX-Test: b\r\nContent-Length: 5\r\n\r\n";
        EmitHeader(*uart, 200, headers, headers.size());
        assert(Value(http.GetStatusCode()) == 200);
        assert(http.GetResponseHeader("x-test") == "a, b");
        assert(http.GetBodyLength() == 5);
        EmitContent(*uart, "hello");
        assert(http.ReadAll() == "hello");
        http.Close();
    }

    // A header fragmented in the status line and a header fragmented inside a
    // field must both be reassembled before parsing.
    {
        const std::string headers =
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n";
        {
            Reset(uart);
            DinosaurMl307Http http(uart);
            assert(http.Open("GET", "http://example.com/status"));
            const size_t split = headers.find("200") + 1;
            EmitHeader(*uart, 200, headers.substr(0, split), headers.size());
            assert(http.GetResponseHeader("content-type").empty());
            EmitHeader(*uart, 200, headers.substr(split), headers.size());
            assert(Value(http.GetStatusCode()) == 200);
            assert(http.GetResponseHeader("Content-Type") == "application/json");
            http.Close();
        }
        {
            Reset(uart);
            DinosaurMl307Http http(uart);
            assert(http.Open("GET", "http://example.com/field"));
            const size_t split = headers.find("Content-Type") + 5;
            EmitHeader(*uart, 200, headers.substr(0, split), headers.size());
            assert(http.GetResponseHeader("content-length").empty());
            EmitHeader(*uart, 200, headers.substr(split), headers.size());
            assert(http.GetBodyLength() == 2);
            http.Close();
        }
    }

    // A corrupted content URC invalidates a previously received HTTP 200
    // response. Close must abort the transfer before deleting its instance.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/corrupt"));
        const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        EmitHeader(*uart, 200, headers, headers.size());
        assert(Value(http.GetStatusCode()) == 200);
        uart->Emit("MHTTPURC", {Str("content"), Int(0), Int(5), Int(5), Int(5), Str("6869")});
        assert(http.ReadAll().empty());
        auto status = http.GetStatusCode();
        assert(!status && status.error().code == NetworkErrc::ReceiveFailed);
        char buffer[5];
        auto received = http.Read(buffer, sizeof(buffer));
        assert(!received && received.error().code == NetworkErrc::ReceiveFailed);
        const size_t before_close = uart->commands.size();
        http.Close();
        assert(uart->commands.size() == before_close + 2);
        assert(uart->commands[before_close] == "AT+MHTTPTERM=0");
        assert(uart->commands[before_close + 1] == "AT+MHTTPDEL=0");
    }

    // Firmware may include an explicit empty data field on the final
    // chunked URC. A fixed-length response with the same terminator is invalid
    // until it has received its entire body.
    for (bool chunked : {false, true}) {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/terminated"));
        const std::string headers = chunked ?
            "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" :
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        EmitHeader(*uart, 200, headers, headers.size());
        uart->Emit("MHTTPURC", {Str("content"), Int(0), Int(chunked ? 0 : 5), Int(2), Int(2),
                                Str("6869")});
        uart->Emit("MHTTPURC", {Str("content"), Int(0), Int(chunked ? 0 : 5), Int(2), Int(0),
                                Str("")});
        if (chunked) {
            assert(http.ReadAll() == "hi");
        } else {
            const auto status = http.GetStatusCode();
            assert(!status && status.error().code == NetworkErrc::ReceiveFailed);
        }
        http.Close();
    }

    // Interleaved or inconsistent header fragments fail without issuing AT
    // commands from the receive callback.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/get"));
        const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
        const size_t before = uart->commands.size();
        EmitHeader(*uart, 200, headers.substr(0, 10), headers.size());
        EmitHeader(*uart, 201, headers.substr(10), headers.size());
        assert(uart->commands.size() == before);
        auto status = http.GetStatusCode();
        assert(!status && status.error().code == NetworkErrc::ReceiveFailed);
        http.Close();
    }

    // ML307 may report a header block whose documented length ends at the
    // final field CRLF without an extra blank line. That is still complete.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        assert(http.Open("GET", "http://example.com/get"));
        const std::string headers = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n";
        EmitHeader(*uart, 200, headers, headers.size());
        assert(Value(http.GetStatusCode()) == 200);
        assert(http.GetBodyLength() == 0);
        assert(http.ReadAll().empty());
        http.Close();
    }

    // Streaming uploads use chunked mode, keep append semantics for data, and
    // terminate with the modem's zero-length CRLF command.
    {
        Reset(uart);
        DinosaurMl307Http http(uart);
        http.SetHeader("Transfer-Encoding", "chunked");
        assert(http.Open("POST", "http://example.com/upload"));
        assert(Value(http.Write("abc", 3)) == 3);
        assert(Value(http.Write("", 0)) == 0);
        assert(Has(uart->commands, "AT+MHTTPCONTENT=0,1,3"));
        assert(Has(uart->commands, "AT+MHTTPCONTENT=0,0,2,\"0D0A\""));
        http.Close();
    }

    return 0;
}
''')
            binary = path / 'test'
            result = subprocess.run(
                ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-pthread', '-I', str(path),
                 '-I', str(DINO), '-I', str(ROOT / 'scripts/tests/support'),
                 '-DDINOSAUR_ENABLE_4G=1', str(path / 'test.cc'), '-o',
                 str(binary)],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == '__main__':
    unittest.main()
