#pragma once
#include <string>
#include <memory>
#include <iostream>


namespace Kmswm {
template<typename ... Args>
inline std::string string_format(const std::string& format, Args ... args) {
	size_t size = snprintf(nullptr, 0, format.c_str(), args ...) + 1; // Extra space for '\0'
	std::unique_ptr<char[]> buf(new char[ size ]);
	snprintf(buf.get(), size, format.c_str(), args ...);
	return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

void Panic(int code, const char* format, ...);

suseconds_t GetDeltaMicroseconds(struct timeval a, struct timeval b);
}//namespace Kmswm
