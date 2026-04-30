#pragma once

#include <cstdio>

class c_console_logger {
public:
	int initialize();
	int shutdown();

	void debug(const char* fmt, ...);
	void info(const char* fmt, ...);
	void warn(const char* fmt, ...);
	void error(const char* file, int line, const char* fmt, ...);

	void flush();

	static c_console_logger g_console_logger;
private:
	const char* format(char* buffer, size_t size, const char* fmt, va_list args);
	void write_line(const char* prefix, const char* str);
	bool m_initialized = false;
	FILE* m_log_file = nullptr;
};

inline c_console_logger* console_logger() {
	return &c_console_logger::g_console_logger;
}

#define CONSOLE_LOG_DEBUG(fmt, ...) console_logger()->debug(fmt, __VA_ARGS__)
#define CONSOLE_LOG_INFO(fmt, ...) console_logger()->info(fmt, __VA_ARGS__)
#define CONSOLE_LOG_WARN(fmt, ...) console_logger()->warn(fmt, __VA_ARGS__)
#define CONSOLE_LOG_ERROR(fmt, ...) console_logger()->error(__FILE__, __LINE__, fmt, __VA_ARGS__)

