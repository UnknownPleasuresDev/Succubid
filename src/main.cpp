#include <iostream>
#include <string>
#include <chrono>
#include <thread>
#include <atomic>
#include <csignal>
#include <filesystem>
#include <cstdlib>
#include <iomanip>
#include <memory>
#include <getopt.h>

#include "MpvIPC.hpp"
#include "HandyAPIClient.hpp"
#include "HandyServer.hpp"
#include "ScriptSelector.hpp"

namespace fs = std::filesystem;

enum class LogLevel {
	INFO,
	WARN,
	ERR,
	DEBUG
};

void log(LogLevel level, const std::string &message) {
	static std::mutex logMutex;

	auto now = std::chrono::system_clock::now();
	auto time_t_now = std::chrono::system_clock::to_time_t(now);

	std::tm tm_buf{};
	localtime_r(&time_t_now, &tm_buf);

	std::ostringstream ss;
	ss << "[" << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S") << "] ";

	switch(level) {
		case LogLevel::INFO:
			ss << "[INFO] ";
			break;
		case LogLevel::WARN:
			ss << "[WARN] ";
			break;
		case LogLevel::ERR:
			ss << "[ERROR] ";
			break;
		case LogLevel::DEBUG:
			ss << "[DEBUG] ";
			break;
	}
	ss << message << "\n";

	std::lock_guard<std::mutex> lock(logMutex);
	std::ostream &os = (level == LogLevel::ERR) ? std::cerr : std::cout;
	os << ss.str() << std::flush;
}

volatile std::sig_atomic_t g_running = 1;

bool stringToBool(std::string value) {
	std::transform(
		value.begin(),
		value.end(),
		value.begin(),
	[](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});

	return value == "true" ||
		   value == "1" ||
		   value == "yes" ||
		   value == "on";
}

void signalHandler(int signum) {
	(void)signum;
	// yes, i assume every signal is SIGINT, fight me about it
	g_running = 0;
}


std::string getEnvOrDefault(const std::string &key, const std::string &defaultValue = "") {
	const char* val = std::getenv(key.c_str());
	return val ? std::string(val) : defaultValue;
}

std::mutex setupMutex;
std::condition_variable cvSetup;
std::atomic<bool> isPreparingScript{false};
std::atomic<bool> scriptReady{false};

std::mutex seekMutex;
std::condition_variable cvSeek;
std::atomic<double> pendingSeekTime{-1.0};
std::chrono::steady_clock::time_point lastSeekTimestamp;

struct TrackedTask {
	std::jthread thread;
	std::shared_ptr<std::atomic<bool>> finished;
};

std::mutex bgMutex;
std::vector<TrackedTask> bgThreads;

template <typename Func>
void spawnBg(Func&& task) {
	std::lock_guard<std::mutex> lock(bgMutex);

	std::erase_if(bgThreads, [](const TrackedTask & t) {
		return t.finished && t.finished->load();
	});

	auto finishedFlag = std::make_shared<std::atomic<bool>>(false);

	bgThreads.push_back(TrackedTask{
		.thread = std::jthread([task = std::forward<Func>(task), finishedFlag]() mutable {
			try {
				task();
			} catch(...) {}
			finishedFlag->store(true);
		}),
		.finished = finishedFlag
	});
}

int main(int argc, char* argv[]) {
	if(geteuid() == 0) {
		std::cerr << "Error: This program must not be run as root.\n";
		return EXIT_FAILURE;
	}
	std::signal(SIGINT, signalHandler);
	std::signal(SIGTERM, signalHandler);
	HandyServer handyScriptServer;

	bool serve_locally = stringToBool(getEnvOrDefault("SUCCUBID_SERVE_LOCAL"));
	std::string connectionKey = getEnvOrDefault("SUCCUBID_HANDY_CONNECTION_KEY");
	std::string apiVersion = getEnvOrDefault("SUCCUBID_HANDY_API_VERSION", "FW3");
	std::string connectionAuth = getEnvOrDefault("SUCCUBID_HANDY_CONNECTION_AUTH");
	std::string socketPath = getEnvOrDefault("SUCCUBID_MPV_SOCKET_PATH", "/tmp/mpv.sock");
	std::string uploadServer = getEnvOrDefault("SUCCUBID_HANDY_UPLOAD_SERVER", "https://www.handyfeeling.com/api/hosting/v2/upload"); // fallback: https://tugbud.kaffesoft.com/cache
	bool gui = stringToBool(getEnvOrDefault("SUCCUBID_USE_GUI"));

	int opt;
	static struct option long_options[] = {
		{"key", required_argument, 0, 'k'},
		{"socket", required_argument, 0, 's'},
		{"upload", required_argument, 0, 'u'},
		{"firmware", required_argument, 0, 'f'},
		{"auth", required_argument, 0, 'a'},
		{"local", no_argument, 0, 'l'}, // not https for local serving, handy already blasts telemetry to any device that asks for it on the local network, no purpose encrypting funscripts, so i dont have to pull in the massive openSSL, also to avoid having to get lets encrypt certificates and a domain for your local network because handy will not accept self signed, seems like bad user experience
		{"gui", no_argument, 0, 'g'},   // script selector gui
		{"help", no_argument, 0, 'h'},
		{0, 0, 0, 0}
	};

	while((opt = getopt_long(argc, argv, "k:s:u:f:a:lgh", long_options, nullptr)) != -1) {
		switch(opt) {
			case 'k':
				connectionKey = optarg;
				break;

			case 's':
				socketPath = optarg;
				break;

			case 'u':
				uploadServer = optarg;
				break;

			case 'f':
				apiVersion = optarg;
				break;

			case 'a':
				connectionAuth = optarg;
				break;

			case 'l':
				serve_locally = true;
				break;

			case 'g':
				gui = true;
				break;

			case 'h':
				std::cout << "Usage: succubid -k <KEY> [OPTIONS]\n\n"
						  << "Required:\n"
						  << "  -k, --key <KEY>              Handy Connection Key\n"
						  << "Optional:\n"
						  << "  -f, --firmware <FW3|FW4>     Handy firmware version (default: FW3)\n"
						  << "  -a, --auth <KEY>             Handy Auth Key (required for FW4)\n"
						  << "  -s, --socket <PATH>          MPV Socket Path (default: /tmp/mpv.sock)\n"
						  << "  -u, --upload <URL>           Custom Upload Server URL\n"
						  << "  -l, --local                  Serve Funscripts Locally (FW3 only)\n"
						  << "  -g, --gui                    Show Script Selector GUI inside MPV if multiple scripts are available\n";
				return EXIT_SUCCESS;

			default:
				return EXIT_FAILURE;
		}
	}

	if(connectionKey.empty()) {
		log(LogLevel::ERR, "Missing required argument: -k <KEY>");
		std::cout << "Try 'succubid --help' for more information.\n";
		return EXIT_FAILURE;
	}

	std::transform(apiVersion.begin(), apiVersion.end(), apiVersion.begin(),
	[](unsigned char c) {
		return std::toupper(c);
	});

	if(apiVersion != "FW3" && apiVersion != "FW4") {
		std::cerr << "Error: firmware must be FW3 or FW4.\n";
		return EXIT_FAILURE;
	}

	if(apiVersion == "FW4") {
		if(connectionAuth.empty()) {
			std::cerr << "Error: FW4 requires --auth (or SUCCUBID_HANDY_CONNECTION_AUTH).\n";
			return EXIT_FAILURE;
		}

		if(serve_locally) {
			std::cerr << "Error: local serving is not supported on FW4.\n";
			return EXIT_FAILURE;
		}
	} else {
		connectionAuth = "";
	}

	HandyAPIClient::FirmwareVersion firmwareVersion = apiVersion == "FW4" ? HandyAPIClient::FW4 : HandyAPIClient::FW3;

	std::atomic<bool> isInitialConnect{true};

	log(LogLevel::INFO, "Succubid summoned!");

	if(serve_locally) {
		log(LogLevel::INFO, "Starting up local script server...");
		handyScriptServer.start();
	}

	HandyAPIClient handyClient(connectionKey, firmwareVersion, connectionAuth);

	log(LogLevel::INFO, "Verifying device status...");
	if(!handyClient.checkStatus()) {
		log(LogLevel::ERR, "Cannot connect to Handy. Please verify your connection key and network status.");
		return EXIT_FAILURE;
	}

	log(LogLevel::INFO, "Initializing HSSP sync settings...");
	try {
		handyClient.setMode(1);
		handyClient.synchronizeTime();
	} catch(const std::exception &e) {
		log(LogLevel::ERR, std::string("Failed during synchronization phase: ") + e.what());
		return EXIT_FAILURE;
	}

	MpvIPC mpv(socketPath);
	ScriptSelector scriptSelector(mpv);

	std::thread seekDebounceThread([&]() {
		while(g_running) {
			double seekTarget = -1.0;

			{
				std::unique_lock<std::mutex> lock(seekMutex);
				cvSeek.wait(lock, [&]() {
					return pendingSeekTime.load() >= 0.0 || !g_running;
				});

				if(!g_running) break;

				auto now = std::chrono::steady_clock::now();
				auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSeekTimestamp);

				if(elapsed < std::chrono::milliseconds(150)) {
					auto remaining = std::chrono::milliseconds(150) - elapsed;
					cvSeek.wait_for(lock, remaining);
					continue;
				}

				seekTarget = pendingSeekTime.exchange(-1.0);
			}

			if(seekTarget >= 0.0) {
				try {
					if(isPreparingScript) {
						std::unique_lock<std::mutex> lock(setupMutex);
						cvSetup.wait_for(lock, std::chrono::seconds(30), [&]() {
							return !isPreparingScript;
						});
					}

					if(!scriptReady) continue;

					long long timeMs = static_cast<long long>(seekTarget * 1000.0);
					log(LogLevel::INFO, "Seek stabilized to " + std::to_string(seekTarget) + "s. Syncing Handy...");
					handyClient.playScript(timeMs);
				} catch(const std::exception& e) {
					log(LogLevel::ERR, std::string("Sync event failed during debounced seek: ") + e.what());
				}
			}
		}
	});


	std::atomic<int> loadSequence{0};

	mpv.onVideoChanged([&](const std::string & videoPath) {
		if(videoPath.empty()) return;
		log(LogLevel::INFO, "MPV Video loaded: " + videoPath);

		int currentId = ++loadSequence;

		{
			std::lock_guard<std::mutex> lock(setupMutex);
			isPreparingScript = true;
			scriptReady = false;
		}

		spawnBg(([&, videoPath, currentId]() {
			auto markSetupFinished = [&](bool success) {
				{
					std::lock_guard<std::mutex> lock(setupMutex);
					isPreparingScript = false;
					scriptReady = success;
				}
				cvSetup.notify_all();
			};

			try {
				std::vector<std::string> scripts = handyClient.findScripts(videoPath);
				if(scripts.empty()) {
					log(LogLevel::WARN, "No matching script file found for: " + videoPath);
					markSetupFinished(false);
					return;
				}

				std::string scriptPath = scripts[0];

				if(gui && scripts.size() > 1) {
					std::vector<std::string> displayNames;
					displayNames.reserve(scripts.size());

					for(const auto &pathStr : scripts) {
						displayNames.push_back(std::filesystem::path(pathStr).filename().string());
					}

					std::optional<size_t> selection = scriptSelector.select(displayNames, std::chrono::seconds(15));

					if(currentId != loadSequence) {
						markSetupFinished(false);
						return;
					}

					if(selection.has_value()) {
						scriptPath = scripts[selection.value()];
						log(LogLevel::INFO, "User selected script variant: " + scriptPath);
					} else {
						log(LogLevel::INFO, "Selection skipped/timed out. Defaulting to: " + scriptPath);
					}
				}

				if(currentId != loadSequence) {
					markSetupFinished(false);
					return;
				}

				if(serve_locally) {
					log(LogLevel::INFO, "Found paired script at: " + scriptPath + ". Parsing...");
					std::string scriptUrl = handyScriptServer.hostScript(scriptPath);

					if(currentId != loadSequence) {
						markSetupFinished(false);
						return;
					}
					handyClient.setupScript(scriptUrl);
					log(LogLevel::INFO, "Sync context successfully updated with local script.");
				} else {
					log(LogLevel::INFO, "Found paired script at: " + scriptPath + ". Uploading...");
					json uploadResult = handyClient.upload(scriptPath, uploadServer);

					if(currentId != loadSequence) {
						markSetupFinished(false);
						return;
					}

					if(uploadResult.contains("url")) {
						std::string scriptUrl = uploadResult["url"];
						handyClient.setupScript(scriptUrl);
						log(LogLevel::INFO, "Sync context successfully updated with remote script.");
					} else {
						log(LogLevel::WARN, "Script upload completed, but server did not return a valid URL.");
						markSetupFinished(false);
						return;
					}
				}

				if(mpv.getPlaying()) {
					double currentSec = mpv.getCurrentPlaybackTime();
					long long timeMs = static_cast<long long>(currentSec * 1000.0);
					log(LogLevel::INFO, "Play detected at " + std::to_string(currentSec) + "s. Sending play sync to Handy (" + std::to_string(timeMs) + "ms)");
					handyClient.playScript(timeMs);
				}

				markSetupFinished(true);
			} catch(const std::exception& e) {
				log(LogLevel::ERR, std::string("Error during script lookup/upload: ") + e.what());
				markSetupFinished(false);
			}
		}));
	});

	mpv.onPlayPauseChanged([&](bool isPlaying) {
		spawnBg(([&, isPlaying]() {
			try {
				if(isPlaying) {
					if(isPreparingScript) {
						log(LogLevel::INFO, "Play event triggered during script setup. Queuing play command...");
						std::unique_lock<std::mutex> lock(setupMutex);
						bool completed = cvSetup.wait_for(lock, std::chrono::seconds(30), [&]() {
							return !isPreparingScript;
						});

						if(!completed || !scriptReady) {
							log(LogLevel::WARN, "Play command dropped: script setup timed out or failed.");
							return;
						}
					}

					if(!scriptReady) return;

					if(isInitialConnect.exchange(false)) {
						log(LogLevel::INFO, "Play event skipped. (Initial Connection)");
						return;
					}

					double currentSec = mpv.getCurrentPlaybackTime();
					long long timeMs = static_cast<long long>(currentSec * 1000.0);
					log(LogLevel::INFO, "Play detected at " + std::to_string(currentSec) + "s. Sending play sync to Handy (" + std::to_string(timeMs) + "ms)");
					handyClient.playScript(timeMs);
				} else {
					log(LogLevel::INFO, "Pause detected. Pausing Handy script...");
					handyClient.stopScript();
				}
			} catch(const std::exception& e) {
				log(LogLevel::ERR, std::string("Sync event failed during play/pause: ") + e.what());
			}
		}));
	});

	mpv.onSeek([&](double timeSeconds) {
		{
			std::lock_guard<std::mutex> lock(seekMutex);
			pendingSeekTime = timeSeconds;
			lastSeekTimestamp = std::chrono::steady_clock::now();
		}
		cvSeek.notify_one();
	});

	mpv.onSpeedChanged([&](double speed) {
		try {
			handyClient.setCurrentSpeed(speed);
		} catch(const std::exception& e) {
			log(LogLevel::ERR, std::string("Sync event failed during speed change: ") + e.what());
		}
	});

	mpv.onMpvStateChanged([&](bool connected) {
		if(!connected) {
			//prevent unwanted orgasms
			log(LogLevel::WARN, "MPV player disconnected. Stopping device...");
			try {
				handyClient.stopScript();
			} catch(const std::exception& e) {
				log(LogLevel::ERR, "Failed to stop Handy script.");
			}
		} else {
			log(LogLevel::INFO, "Connection established to active MPV instance!");
			isInitialConnect = true;
		}
	});

	log(LogLevel::INFO, "Entering connection loop. Waiting for MPV socket activity...");
	while(g_running) {
		if(!mpv.isConnected()) {
			if(fs::exists(socketPath)) {
				log(LogLevel::INFO, "Socket file detected. Attempting to connect...");
				mpv.connectToMpv();
			}
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}

	log(LogLevel::INFO, "Shutdown signal received. Stopping...");

	cvSeek.notify_all();
	cvSetup.notify_all();

	if(seekDebounceThread.joinable()) {
		seekDebounceThread.join();
	}

	bgThreads.clear();

	log(LogLevel::INFO, "Stopping services and cleaning up...");
	if(mpv.isConnected()) {
		mpv.disconnect();
	}

	try {
		handyClient.stopScript();
	} catch(...) {
		log(LogLevel::ERR, "Failed to stop Handy script.");
	}

	log(LogLevel::INFO, "Succubid exorcised!");
	return EXIT_SUCCESS;
}
