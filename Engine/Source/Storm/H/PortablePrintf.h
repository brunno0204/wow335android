#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>

// Translate Microsoft integer length modifiers at the CRT boundary. This also
// serves ordered printf, whose parsed specifier may still contain I64/I32.
inline std::string StormPortableFormat(const char* format) {
    std::string result;
    for (size_t i = 0; format[i]; ++i) {
        if (format[i] != '%') { result += format[i]; continue; }
        result += '%';
        if (format[i + 1] == '%') { result += format[++i]; continue; }
        while (format[++i]) {
            if (format[i] == 'I' && format[i + 1] == '6' && format[i + 2] == '4') {
                result += "ll"; i += 2; continue;
            }
            if (format[i] == 'I' && format[i + 1] == '3' && format[i + 2] == '2') { i += 2; continue; }
            result += format[i];
            if (std::string("diouxXfFeEgGaAcspn").find(format[i]) != std::string::npos) break;
        }
        if (!format[i]) break;
    }
    return result;
}
inline int StormPortableVsnprintf(char* destination, size_t size, const char* format, va_list args) {
    const auto translated = StormPortableFormat(format);
    return std::vsnprintf(destination, size, translated.c_str(), args);
}
inline int StormPortableSnprintf(char* destination, size_t size, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int result = StormPortableVsnprintf(destination, size, format, args);
    va_end(args);
    return result;
}
