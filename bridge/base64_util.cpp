#include "base64_util.h"

namespace tgcall {
namespace {

int Base64DecodeChar(char value) {
  if (value >= 'A' && value <= 'Z') {
    return value - 'A';
  }
  if (value >= 'a' && value <= 'z') {
    return value - 'a' + 26;
  }
  if (value >= '0' && value <= '9') {
    return value - '0' + 52;
  }
  if (value == '+' || value == '-') {
    return 62;
  }
  if (value == '/' || value == '_') {
    return 63;
  }
  return -1;
}

}  // namespace

bool DecodeBase64(const std::string &input, std::vector<uint8_t> &output) {
  output.clear();
  int value = 0;
  int bits = -8;
  for (char inputChar : input) {
    if (inputChar == '=') {
      break;
    }
    const int decoded = Base64DecodeChar(inputChar);
    if (decoded < 0) {
      return false;
    }
    value = (value << 6) | decoded;
    bits += 6;
    if (bits >= 0) {
      output.push_back(static_cast<uint8_t>((value >> bits) & 0xff));
      bits -= 8;
    }
  }
  return true;
}

std::string EncodeBase64(const std::vector<uint8_t> &input) {
  static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2) / 3) * 4);

  size_t index = 0;
  while (index + 2 < input.size()) {
    const uint32_t value = (static_cast<uint32_t>(input[index]) << 16) |
                           (static_cast<uint32_t>(input[index + 1]) << 8) |
                           static_cast<uint32_t>(input[index + 2]);
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(kAlphabet[(value >> 6) & 0x3f]);
    output.push_back(kAlphabet[value & 0x3f]);
    index += 3;
  }

  const size_t remaining = input.size() - index;
  if (remaining == 1) {
    const uint32_t value = static_cast<uint32_t>(input[index]) << 16;
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back('=');
    output.push_back('=');
  } else if (remaining == 2) {
    const uint32_t value = (static_cast<uint32_t>(input[index]) << 16) |
                           (static_cast<uint32_t>(input[index + 1]) << 8);
    output.push_back(kAlphabet[(value >> 18) & 0x3f]);
    output.push_back(kAlphabet[(value >> 12) & 0x3f]);
    output.push_back(kAlphabet[(value >> 6) & 0x3f]);
    output.push_back('=');
  }

  return output;
}

}  // namespace tgcall
