#pragma once

#include "PCH.h"

class Settings
{
public:
	static Settings& GetSingleton()
	{
		static Settings instance;
		return instance;
	}

	static constexpr const char* kPath = "Data/SKSE/Plugins/DialogueDucker.ini";

	void Load()
	{
		std::ifstream file(kPath);
		std::string   line;
		while (std::getline(file, line)) {
			const auto equals = line.find('=');
			if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[' || equals == std::string::npos) {
				continue;
			}
			const auto key = Trim(line.substr(0, equals));
			const auto value = Trim(line.substr(equals + 1));
			try {
				if (key == "bEnabled") {
					enabled = Flag(value);
				} else if (key == "bLog") {
					log = Flag(value);
				} else if (key == "fDuck") {
					duck = std::clamp(std::stof(value), 0.0f, 18.0f);
				}
			} catch (...) {
				logger::warn("DialogueDucker.ini: could not read '{}'", line);
			}
		}
		logger::info("Settings: enabled {}, log {}, duck {:.1f} dB", enabled, log, duck);
	}

	enum Group : std::size_t
	{
		kOther,
		kBackground,
		kVoice,
		kMusic
	};

	bool  enabled{ true };
	bool  log{ false };
	float duck{ 6.5f };

private:
	static bool Flag(const std::string& a_value) { return a_value == "true" || a_value == "1"; }

	static std::string Trim(std::string a_text)
	{
		const auto first = a_text.find_first_not_of(" \t\r");
		if (first == std::string::npos) {
			return {};
		}
		const auto last = a_text.find_last_not_of(" \t\r");
		return a_text.substr(first, last - first + 1);
	}
};
