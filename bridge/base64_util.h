#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tgcall {

bool DecodeBase64(const std::string &input, std::vector<uint8_t> &output);
std::string EncodeBase64(const std::vector<uint8_t> &input);

}  // namespace tgcall
