#pragma once

#include <expected>

#include "esp_err.h"
#include "network_error.h"

enum class AtErrc {
    Timeout,
    CommandError,
    CmeError,
    TransmitFailed,
    NotInitialized,
};

struct AtError {
    AtErrc code = AtErrc::CommandError;
    int cme = 0;
    esp_err_t esp = ESP_OK;

    NetworkError ToNetworkError() const {
        switch (code) {
            case AtErrc::Timeout:
                return NetworkError::Timeout();
            case AtErrc::CmeError:
                return NetworkError::CmeError(cme);
            case AtErrc::TransmitFailed:
                return NetworkError::TransmitFailed(esp);
            case AtErrc::NotInitialized:
                return NetworkError::NotInitialized();
            case AtErrc::CommandError:
            default:
                return NetworkError::AtCommandFailed(cme);
        }
    }
};

using AtResult = std::expected<void, AtError>;

template <typename T>
using AtValue = std::expected<T, AtError>;
