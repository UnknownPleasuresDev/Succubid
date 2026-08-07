#include "MpvIPC.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <chrono>

MpvIPC::MpvIPC(const std::string &socketPath)
	: m_seekPending(false), m_restartPending(false), m_socketPath(socketPath), m_socketFd(-1), m_connected(false), m_running(false), m_lastKnownPosition(0.0), m_isPlaying(false) {}

MpvIPC::~MpvIPC() {
	disconnect();
}

bool MpvIPC::connectToMpv() {
	{
		std::lock_guard<std::mutex> lock(m_socketMutex);
		if(m_connected)
			return true;

		m_socketFd = socket(AF_UNIX, SOCK_STREAM, 0);
		if(m_socketFd == -1)
			return false;

		sockaddr_un addr{};
		addr.sun_family = AF_UNIX;
		strncpy(addr.sun_path, m_socketPath.c_str(), sizeof(addr.sun_path) - 1);

		if(connect(m_socketFd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
			close(m_socketFd);
			m_socketFd = -1;
			return false;
		}

		m_connected = true;
		m_running = true;
	}

	m_listenerThread = std::thread(&MpvIPC::listenLoop, this);

	setupPropertyObserving();

	if(m_onMpvStateChanged)
		m_onMpvStateChanged(true);
	return true;
}

void MpvIPC::disconnect() {
	bool wasConnected = false;

	{
		std::lock_guard<std::mutex> lock(m_socketMutex);
		wasConnected = m_connected;
		m_running = false;
		m_connected = false;

		if(m_socketFd != -1) {
			shutdown(m_socketFd, SHUT_RDWR);
			close(m_socketFd);
			m_socketFd = -1;
		}
	}

	if(m_listenerThread.joinable()) {
		if(std::this_thread::get_id() != m_listenerThread.get_id()) {
			m_listenerThread.join();
		} else {
			m_listenerThread.detach();
		}
	}

	{
		std::lock_guard<std::mutex> lock(m_responseMutex);
		m_responseCv.notify_all();
	}

	if(wasConnected && m_onMpvStateChanged) {
		m_onMpvStateChanged(false);
	}
}

void MpvIPC::sendCommand(const json &command) {
	if(!m_connected)
		return;

	std::string payload = command.dump() + "\n";
	bool failed = false;

	{
		std::lock_guard<std::mutex> lock(m_socketMutex);

		const char* data = payload.data();
		size_t remaining = payload.size();

		while(remaining > 0) {
			ssize_t sent = send(m_socketFd, data, remaining, MSG_NOSIGNAL);

			if(sent < 0) {
				if(errno == EINTR)
					continue;

				failed = true;
				break;
			}

			if(sent == 0) {
				failed = true;
				break;
			}

			data += sent;
			remaining -= sent;
		}
	}

	if(failed)
		disconnect();
}

json MpvIPC::sendCommandWithResponse(const json &command) {
	if(!m_connected)
		return json::object();

	uint64_t id = 0;
	{
		std::lock_guard<std::mutex> lock(m_responseMutex);
		id = ++m_requestIdCounter;
	}

	json fullCommand = command;
	fullCommand["request_id"] = id;

	sendCommand(fullCommand);

	std::unique_lock<std::mutex> lock(m_responseMutex);

	bool success = m_responseCv.wait_for(lock, std::chrono::seconds(1), [this, id]() {
		return m_pendingResponses.find(id) != m_pendingResponses.end() || !m_running;
	});

	if(!success || !m_running) {
		return json::object();
	}

	json response = m_pendingResponses[id];
	m_pendingResponses.erase(id);
	return response;
}

void MpvIPC::setupPropertyObserving() {
	sendCommand({{"command", {"observe_property", 1, "path"}}});
	sendCommand({{"command", {"observe_property", 2, "pause"}}});
	sendCommand({{"command", {"observe_property", 3, "time-pos"}}});
	sendCommand({{"command", {"observe_property", 4, "speed"}}});
}

std::string MpvIPC::getCurrentVideoPath() {
	json resp = sendCommandWithResponse({{"command", {"get_property", "path"}}});
	if(resp.contains("data") && resp["data"].is_string()) {
		return resp["data"].get<std::string>();
	}
	return "";
}

double MpvIPC::getDuration() {
	json resp = sendCommandWithResponse({{"command", {"get_property", "duration"}}});
	if(resp.contains("data") && resp["data"].is_number()) {
		return resp["data"].get<double>();
	}
	return 0.0;
}

void MpvIPC::listenLoop() {
	char buffer[4096];
	std::string textBuffer;

	while(m_running) {
		ssize_t bytesRead = recv(m_socketFd, buffer, sizeof(buffer) - 1, 0);
		if(bytesRead <= 0) {
			break;
		}

		buffer[bytesRead] = '\0';
		textBuffer += buffer;

		size_t pos;
		while((pos = textBuffer.find('\n')) != std::string::npos) {
			std::string line = textBuffer.substr(0, pos);
			textBuffer.erase(0, pos + 1);

			if(!line.empty()) {
				handleIncomingMessage(line);
			}
		}
	}

	if(m_connected) {
		disconnect();
	}
}

void MpvIPC::handleIncomingMessage(const std::string &rawMsg) {
	try {
		json msg = json::parse(rawMsg);

		if(msg.contains("request_id")) {
			uint64_t id = msg["request_id"].get<uint64_t>();
			std::lock_guard<std::mutex> lock(m_responseMutex);
			m_pendingResponses[id] = msg;
			m_responseCv.notify_all();
			return;
		}

		if(msg.value("event", "") == "property-change") {
			std::string propName = msg.value("name", "");
			auto value = msg["data"];

			if(propName == "path" && !value.is_null()) {
				if(m_onVideoChanged)
					m_onVideoChanged(value.get<std::string>());
			} else if(propName == "speed" && !value.is_null()) {
				if(m_onSpeedChanged)
					m_onSpeedChanged(value.get<double>());
			} else if(propName == "pause" && !value.is_null()) {
				m_isPlaying = !value.get<bool>();
				if(m_onPlayPause)
					m_onPlayPause(!value.get<bool>());
			} else if(propName == "time-pos" && !value.is_null() && value.is_number()) {
				m_lastKnownPosition = value.get<double>();

				if(m_seekPending) {
					m_seekPending = false;

					if(m_onSeek)
						m_onSeek(m_lastKnownPosition);
				}
			}
		}

		if(msg.value("event", "") == "playback-restart") {
			if(m_restartPending) {
				m_seekPending = true;
				m_restartPending = false;
			}
		}

		if(msg.value("event", "") == "seek") {
			m_restartPending = true;
		}

		if(msg.value("event", "") == "client-message") {
			if(msg.contains("args") && msg["args"].is_array()) {
				std::vector<std::string> args;
				for(const auto &arg : msg["args"]) {
					if(arg.is_string()) {
						args.push_back(arg.get<std::string>());
					} else {
						args.push_back(arg.dump());
					}
				}

				CustomMessageCallback cb;
				{
					std::lock_guard<std::mutex> lock(m_socketMutex);
					cb = m_onCustomMessage;
				}

				if(cb && !args.empty()) {
					std::string target = args[0];
					cb(target, args);
				}
			}
		}
	} catch(const json::parse_error&) {
	}
}

void MpvIPC::sendCustomMessage(const std::vector<std::string>& args) {
	if(!m_connected || args.empty())
		return;

	json cmd = json::array();
	cmd.push_back("script-message");
	for(const auto &arg : args) {
		cmd.push_back(arg);
	}

	sendCommand({{"command", cmd}});
}

void MpvIPC::sendCustomMessageTo(const std::string &target, const std::vector<std::string>& args) {
	if(!m_connected || target.empty() || args.empty())
		return;

	json cmd = json::array();
	cmd.push_back("script-message-to");
	cmd.push_back(target);
	for(const auto &arg : args) {
		cmd.push_back(arg);
	}

	sendCommand({{"command", cmd}});
}
