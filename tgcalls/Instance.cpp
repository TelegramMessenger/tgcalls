#include "Instance.h"

#include <algorithm>
#include <stdarg.h>

namespace tgcalls {
namespace {

std::function<void(std::string const &)> globalLoggingFunction;

std::map<std::string, std::unique_ptr<Meta>> &MetaMap() {
	static auto result = std::map<std::string, std::unique_ptr<Meta>>();
	return result;
}

} // namespace

std::vector<std::string> Meta::Versions() {
	auto &map = MetaMap();
	auto result = std::vector<std::string>();
	result.reserve(map.size());
	for (const auto &entry : map) {
		result.push_back(entry.first);
	}
	return result;
}

int Meta::MaxLayer() {
	auto result = 0;
	for (const auto &entry : MetaMap()) {
		result = std::max(result, entry.second->connectionMaxLayer());
	}
	return result;
}

std::unique_ptr<Instance> Meta::Create(
		const std::string &version,
		Descriptor &&descriptor) {
	const auto i = MetaMap().find(version);
	return (i != MetaMap().end())
		? i->second->construct(std::move(descriptor))
		: nullptr;
}

void Meta::RegisterOne(std::unique_ptr<Meta> meta) {
	if (meta) {
		const auto version = meta->version();
		MetaMap().emplace(version, std::move(meta));
	}
}

void SetLoggingFunction(std::function<void(std::string const &)> loggingFunction) {
	globalLoggingFunction = loggingFunction;
}

} // namespace tgcalls

void __tgvoip_call_tglog(const char *format, ...) {
	va_list vaArgs;
	va_start(vaArgs, format);

	va_list vaCopy;
	va_copy(vaCopy, vaArgs);
	const int length = std::vsnprintf(nullptr, 0, format, vaCopy);
	va_end(vaCopy);

	std::vector<char> zc(length + 1);
	std::vsnprintf(zc.data(), zc.size(), format, vaArgs);
	va_end(vaArgs);

	if (tgcalls::globalLoggingFunction != nullptr) {
		tgcalls::globalLoggingFunction(std::string(zc.data(), zc.size()));
	}
}
