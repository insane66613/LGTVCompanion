#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <boost/asio.hpp>
#include <boost/asio/windows/stream_handle.hpp>

class IpcServer2
{
public:
	IpcServer2(std::wstring name,
		void (*callback)(std::wstring, LPVOID),
		LPVOID object,
		bool message_mode = false);
	~IpcServer2();
	bool send(std::wstring msg, int pipe = -1);
	bool terminate();

private:
	struct PipeInstance {
		HANDLE raw_handle = INVALID_HANDLE_VALUE;
		boost::asio::windows::stream_handle stream;
		boost::asio::strand<boost::asio::io_context::executor_type> strand;
		std::array<wchar_t, 1024> buffer{};
		std::atomic<bool> connected{ false };

		PipeInstance(boost::asio::io_context& io)
			: stream(io), strand(io.get_executor()) {
		}
	};
	std::atomic<HANDLE> pending_handle_{ INVALID_HANDLE_VALUE };

	void accept_loop();
	void start_read(std::shared_ptr<PipeInstance> pipe);
	void do_write(std::shared_ptr<PipeInstance> pipe, std::wstring msg);

private:
	boost::asio::io_context io_;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
	std::thread io_thread_;
	std::thread accept_thread_;

	std::wstring name_;
	bool message_mode_ = false;
	void (*callback_)(std::wstring, LPVOID) = nullptr;
	LPVOID object_ = nullptr;

	std::mutex pipes_mutex_;
	std::vector<std::shared_ptr<PipeInstance>> pipes_;
	std::atomic<bool> running_{ false };
};

class IpcClient2
{
public:
	IpcClient2(std::wstring name,
		void (*callback)(std::wstring, LPVOID),
		LPVOID object,
		bool message_mode = false);
	~IpcClient2();
	bool send(std::wstring msg);
	void sendAsync(
		std::wstring msg,
		std::function<void(bool)> completion,
		unsigned max_attempts = 41,
		unsigned retry_delay_ms = 25,
		unsigned retry_window_ms = 1000);
	bool terminate();

private:
	struct AsyncSendState {
		std::wstring message;
		std::function<void(bool)> completion;
		unsigned attempts = 0;
		unsigned max_attempts = 0;
		unsigned retry_delay_ms = 0;
		std::chrono::steady_clock::time_point deadline;
		boost::asio::steady_timer timer;

		AsyncSendState(
			boost::asio::io_context& io,
			std::wstring value,
			std::function<void(bool)> callback,
			unsigned attempts_limit,
			unsigned retry_delay,
			unsigned retry_window)
			: message(std::move(value)),
			completion(std::move(callback)),
			max_attempts(attempts_limit),
			retry_delay_ms(retry_delay),
			deadline(std::chrono::steady_clock::now() + std::chrono::milliseconds(retry_window)),
			timer(io) {}
	};

	void connect_loop();
	void start_read();
	void send_async_attempt(std::shared_ptr<AsyncSendState> state);

private:
	boost::asio::io_context io_;
	boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
	boost::asio::windows::stream_handle stream_;
	std::thread io_thread_;
	std::thread connect_thread_;

	std::wstring name_;
	bool message_mode_ = false;
	void (*callback_)(std::wstring, LPVOID) = nullptr;
	LPVOID object_ = nullptr;
	std::atomic<bool> reconnect_{ false };
	std::mutex send_mutex_;

	HANDLE raw_ = INVALID_HANDLE_VALUE;
	std::array<wchar_t, 1024> buffer_{};
	std::atomic<bool> running_{ false };
};
