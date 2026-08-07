#pragma once

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <vector>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
#include <nlohmann/json.hpp>
#pragma GCC diagnostic pop

using json = nlohmann::json;

class MpvIPC {
  public:
	using CustomMessageCallback = std::function<void(const std::string&, const std::vector<std::string> &)>;

	MpvIPC(const std::string &socketPath);
	~MpvIPC();

	bool connectToMpv();
	void disconnect();
	bool isConnected() const {
		return m_connected;
	}

	std::string getCurrentVideoPath();
	double getDuration();
	double getCurrentPlaybackTime() const {
		return m_lastKnownPosition;
	}
	bool getPlaying() const {
		return m_isPlaying;
	}

	void sendCustomMessage(const std::vector<std::string>& args);
	void sendCustomMessageTo(const std::string &target, const std::vector<std::string>& args);

	void onVideoChanged(std::function<void(const std::string&)> cb) {
		m_onVideoChanged = cb;
	}
	void onPlayPauseChanged(std::function<void(bool)> cb) {
		m_onPlayPause = cb;
	}
	void onSeek(std::function<void(double)> cb) {
		m_onSeek = cb;
	}
	void onMpvStateChanged(std::function<void(bool)> cb) {
		m_onMpvStateChanged = cb;
	}
	void onSpeedChanged(std::function<void(double)> cb) {
		m_onSpeedChanged = cb;
	}
	void onCustomMessage(CustomMessageCallback cb) {
		m_onCustomMessage = cb;
	}
	void sendCommand(const json &command);

  private:
	void listenLoop();
	json sendCommandWithResponse(const json &command);
	void setupPropertyObserving();
	void handleIncomingMessage(const std::string &rawMsg);

	bool m_seekPending;
	bool m_restartPending;
	std::string m_socketPath;
	int m_socketFd;
	std::atomic<bool> m_connected;
	std::atomic<bool> m_running;
	std::thread m_listenerThread;

	std::mutex m_socketMutex;

	std::mutex m_responseMutex;
	std::condition_variable m_responseCv;
	uint64_t m_requestIdCounter = 0;
	std::unordered_map<uint64_t, json> m_pendingResponses;

	double m_lastKnownPosition = 0.0;
	bool m_isPlaying = false;

	std::function<void(const std::string&)> m_onVideoChanged;
	std::function<void(bool)> m_onPlayPause;
	std::function<void(double)> m_onSeek;
	std::function<void(bool)> m_onMpvStateChanged;
	std::function<void(double)> m_onSpeedChanged;
	CustomMessageCallback m_onCustomMessage;
};
