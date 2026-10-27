#ifndef TGCALLS_CUSTOM_PARAMETERS_H
#define TGCALLS_CUSTOM_PARAMETERS_H

#include <limits>
#include <map>
#include <string>

#include "third-party/json11.hpp"

namespace tgcalls {

// Custom parameters arrive from the server as a JSON object string on
// Descriptor::config::customParameters. Each engine parses it once into a map;
// these read individual keys out of that map.
//
// getCustomParameterBool/Int default to off/zero, so an absent key means
// "unchanged behaviour" - the right default for a flag that opts into a new
// behaviour. getCustomParameterBoolDefaultTrue is for a flag guarding a fix
// that is already the intended behaviour: an absent key means "on", and only
// an explicit `false` from the server turns it off (a rollback switch).

inline bool getCustomParameterBool(std::map<std::string, json11::Json> const &parameters, std::string const &name) {
    const auto value = parameters.find(name);
    if (value != parameters.end() && value->second.is_bool() && value->second.bool_value()) {
        return true;
    } else {
        return false;
    }
}

inline bool getCustomParameterBoolDefaultTrue(std::map<std::string, json11::Json> const &parameters, std::string const &name) {
    const auto value = parameters.find(name);
    if (value != parameters.end() && value->second.is_bool()) {
        return value->second.bool_value();
    }
    return true;
}

// json11 stores a number with a fraction, an exponent or 10+ digits as a double,
// and its int_value() casts that unchecked (undefined behaviour past int's
// range), so the double is range-checked here: a number that does not fit an
// int reads as absent. A fraction is truncated.
inline int getCustomParameterInt(std::map<std::string, json11::Json> const &parameters, std::string const &name) {
    const auto value = parameters.find(name);
    if (value != parameters.end() && value->second.is_number()) {
        const double number = value->second.number_value();
        if (number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max()) {
            return static_cast<int>(number);
        }
    }
    return 0;
}

} // namespace tgcalls

#endif
