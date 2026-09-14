// Query the libraries linked by the extension, rather than an unrelated ffmpeg executable.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

static std::string JsonString(const char *value) {
	std::string result = "\"";
	for (const unsigned char *p = reinterpret_cast<const unsigned char *>(value); *p; ++p) {
		if (*p == '"' || *p == '\\') {
			result += '\\';
			result += static_cast<char>(*p);
		} else if (*p < 0x20) {
			char escaped[7];
			std::snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
			result += escaped;
		} else {
			result += static_cast<char>(*p);
		}
	}
	return result + '"';
}

int main(int argc, char **argv) {
	bool allow_gpl = false;
	std::string output;
	for (int i = 1; i < argc; ++i) {
		if (std::strcmp(argv[i], "--allow-gpl") == 0) {
			allow_gpl = true;
		} else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
			output = argv[++i];
		} else {
			std::cerr << "Usage: lerobot_ffmpeg_license_probe [--allow-gpl] [--output FILE]\n";
			return 2;
		}
	}
	struct Library {
		const char *name;
		const char *license;
		const char *configuration;
		unsigned version;
	};
	const Library libraries[] = {{"libavcodec", avcodec_license(), avcodec_configuration(), avcodec_version()},
	                             {"libavformat", avformat_license(), avformat_configuration(), avformat_version()},
	                             {"libavutil", avutil_license(), avutil_configuration(), avutil_version()},
	                             {"libswscale", swscale_license(), swscale_configuration(), swscale_version()}};
	std::string report = "{\n  \"libraries\": [\n";
	bool valid = true;
	for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i) {
		const auto &lib = libraries[i];
		const bool lgpl = std::strcmp(lib.license, "LGPL version 2.1 or later") == 0 ||
		                  std::strcmp(lib.license, "LGPL version 3 or later") == 0;
		const bool gpl = std::strcmp(lib.license, "GPL version 2 or later") == 0 ||
		                 std::strcmp(lib.license, "GPL version 3 or later") == 0;
		const bool gpl_components = std::strstr(lib.configuration, "--enable-gpl") ||
		                            std::strstr(lib.configuration, "--enable-libx264") ||
		                            std::strstr(lib.configuration, "--enable-libx265");
		if ((!lgpl && !(allow_gpl && gpl)) || (!allow_gpl && gpl_components) ||
		    std::strstr(lib.configuration, "--enable-nonfree")) {
			std::cerr << lib.name << ": rejected FFmpeg build (" << lib.license << "). "
			          << "Use an LGPL FFmpeg build; GPL development builds require "
			          << "LEROBOT_ALLOW_GPL=ON or the gpl-codecs manifest feature. "
			          << "Nonfree builds cannot be packaged.\n";
			valid = false;
		}
		report += i ? ",\n" : "";
		report += "    {\"name\": " + JsonString(lib.name) + ", \"license\": " + JsonString(lib.license) +
		          ", \"configuration\": " + JsonString(lib.configuration) +
		          ", \"version\": " + std::to_string(lib.version) + "}";
	}
	report += "\n  ]\n}\n";
	if (!valid) {
		return 1;
	}
	if (output.empty()) {
		std::cout << report;
	} else {
		std::ofstream file(output);
		file << report;
		if (!file) {
			std::cerr << "Could not write FFmpeg license report: " << output << '\n';
			return 1;
		}
	}
	return 0;
}
