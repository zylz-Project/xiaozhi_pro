#ifndef WZH_DINOSAUR_ML307_AT_H_
#define WZH_DINOSAUR_ML307_AT_H_

#include <at_uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// esp-ml307 3.7+ returns std::expected from every AT operation. These helpers
// preserve the CME code for callers that must distinguish a modem rejection
// from a timeout while keeping the board adapters readable.
inline bool AtCommandSucceeded(const AtResult& result) { return result.has_value(); }

inline int AtCommandCme(const AtResult& result) {
    return result.has_value() ? 0 : result.error().cme;
}

inline bool IsMl307PdpDeactivated(const std::string& command,
                                  const std::vector<AtArgumentValue>& args) {
    return command == "MIPCALL" && args.size() >= 2 && args[0].type == AtArgumentValue::Type::Int &&
           args[0].int_value == 1 && args[1].type == AtArgumentValue::Type::Int &&
           args[1].int_value == 0;
}

inline bool IsMl307TransportLost(const std::string& command,
                                 const std::vector<AtArgumentValue>& args) {
    return command == "MATREADY" || command == "FIFO_OVERFLOW" ||
           IsMl307PdpDeactivated(command, args);
}

// A delayed response can satisfy the first probe. Require two consecutive
// acknowledgements and allow a short window for the modem to finish a timed-out
// command before handing the UART to a different transport.
inline bool RecoverMl307AtParser(AtUart& uart, int probe_timeout_ms) {
    int acknowledgements = 0;
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (const auto result = uart.SendCommand("AT", probe_timeout_ms); result) {
            if (++acknowledgements == 2)
                return true;
        } else {
            if (result.error().code == AtErrc::NotInitialized ||
                result.error().code == AtErrc::TransmitFailed)
                return false;
            acknowledgements = 0;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return false;
}

#endif
