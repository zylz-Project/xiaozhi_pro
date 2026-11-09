#ifndef CAPTIVE_DNS_RESPONSE_H_
#define CAPTIVE_DNS_RESPONSE_H_

#include <cstddef>
#include <cstdint>
#include <cstring>

// Build a bounded DNS response to one standard question. Ignore EDNS options
// after the question instead of putting the answer after an OPT record.
inline size_t BuildCaptiveDnsResponse(uint8_t* packet, size_t length, size_t capacity,
                                      const uint8_t gateway[4]) {
    if (!packet || !gateway || length < 17 || length > capacity || (packet[2] & 0xf8) != 0 ||
        packet[4] != 0 || packet[5] != 1)
        return 0;

    size_t pos = 12;
    size_t name_length = 0;
    bool terminated = false;
    while (pos < length) {
        const size_t label_length = packet[pos++];
        if (!label_length) {
            terminated = true;
            break;
        }
        if (label_length > 63 || pos + label_length > length ||
            (name_length += label_length + 1) > 253)
            return 0;
        pos += label_length;
    }
    if (!terminated || length - pos < 4)
        return 0;
    const uint16_t type = (uint16_t(packet[pos]) << 8) | packet[pos + 1];
    const uint16_t klass = (uint16_t(packet[pos + 2]) << 8) | packet[pos + 3];
    pos += 4;
    const bool answer = type == 1 && klass == 1;
    if (answer && capacity - pos < 16)
        return 0;

    packet[2] = 0x80 | (packet[2] & 0x01);  // response; preserve recursion desired
    packet[3] = 0x80;                       // recursion available; no error
    packet[6] = 0;
    packet[7] = answer ? 1 : 0;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;
    if (answer) {
        static constexpr uint8_t record[] = {0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
                                             0x00, 0x00, 0x00, 0x1c, 0x00, 0x04};
        std::memcpy(packet + pos, record, sizeof(record));
        pos += sizeof(record);
        std::memcpy(packet + pos, gateway, 4);
        pos += 4;
    }
    return pos;
}

#endif  // CAPTIVE_DNS_RESPONSE_H_
