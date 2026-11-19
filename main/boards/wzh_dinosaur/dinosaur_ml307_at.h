#ifndef WZH_DINOSAUR_ML307_AT_H_
#define WZH_DINOSAUR_ML307_AT_H_

#include <at_error.h>

// esp-ml307 3.7+ returns std::expected from every AT operation. These helpers
// preserve the CME code for callers that must distinguish a modem rejection
// from a timeout while keeping the board adapters readable.
inline bool AtCommandSucceeded(const AtResult& result) { return result.has_value(); }

inline int AtCommandCme(const AtResult& result) {
    return result.has_value() ? 0 : result.error().cme;
}

#endif
