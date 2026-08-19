#define WIN32_LEAN_AND_MEAN
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#define _WINSOCK_DEPRECATED_NO_WARNINGS

#include "ipc_v2.h"

IpcServer2::IpcServer2(std::wstring name,
	void (*callback)(std::wstring, LPVOID),
	LPVOID object,
	bool message_mode)
	: name_(std::move(name)), message_mode_(message_mode), callback_(callback), object_(object)
	, work_(boost::asio::make_work_guard(io_))
{
	running_ = true;

	io_thread_ = std::thread([this] {
		io_.run();
		});

	accept_thread_ = std::thread([this] {
		accept_loop();
		});
}
IpcServer2::~IpcServer2()
{
	terminate();
}
void IpcServer2::accept_loop()
{
	BYTE sd[SECURITY_DESCRIPTOR_MIN_LENGTH];
	PSECURITY_DESCRIPTOR psd = (PSECURITY_DESCRIPTOR)sd;
	InitializeSecurityDescriptor(psd, SECURITY_DESCRIPTOR_REVISION);
	SetSecurityDescriptorDacl(psd, TRUE, nullptr, FALSE);
	SECURITY_ATTRIBUTES sa{ sizeof(sa), psd, FALSE };

	while (running_) {
		auto pipe = std::make_shared<PipeInstance>(io_);

		const DWORD pipe_mode = message_mode_
			? (PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE)
			: (PIPE_TYPE_BYTE | PIPE_READMODE_BYTE);
		pipe->raw_handle = CreateNamedPipeW(
			name_.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
			pipe_mode,
			50,
			4096, 4096,
			0,
			&sa);

		if (pipe->raw_handle == INVALID_HANDLE_VALUE)
			continue;
		
		HANDLE connect_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!connect_event)
		{
			CloseHandle(pipe->raw_handle);
			pipe->raw_handle = INVALID_HANDLE_VALUE;
			continue;
		}
		OVERLAPPED connect_overlapped{};
		connect_overlapped.hEvent = connect_event;
		pending_handle_ = pipe->raw_handle;

		bool connected = false;
		BOOL ok = ConnectNamedPipe(pipe->raw_handle, &connect_overlapped);
		if (ok)
		{
			connected = true;
		}
		else
		{
			const DWORD err = GetLastError();
			if (err == ERROR_PIPE_CONNECTED)
			{
				connected = true;
			}
			else if (err == ERROR_IO_PENDING)
			{
				while (running_)
				{
					const DWORD wait = WaitForSingleObject(connect_event, 100);
					if (wait == WAIT_OBJECT_0)
					{
						DWORD ignored = 0;
						connected = GetOverlappedResult(pipe->raw_handle, &connect_overlapped, &ignored, FALSE) == TRUE;
						break;
					}
					if (wait == WAIT_FAILED)
						break;
				}
				if (!running_ && !connected)
				{
					CancelIoEx(pipe->raw_handle, &connect_overlapped);
					DWORD ignored = 0;
					GetOverlappedResult(pipe->raw_handle, &connect_overlapped, &ignored, TRUE);
				}
			}
		}
		pending_handle_ = INVALID_HANDLE_VALUE;
		CloseHandle(connect_event);

		if (!connected || !running_)
		{
			CloseHandle(pipe->raw_handle);
			pipe->raw_handle = INVALID_HANDLE_VALUE;
			continue;
		}

		boost::system::error_code assign_error;
		pipe->stream.assign(pipe->raw_handle, assign_error);
		if (assign_error)
		{
			CloseHandle(pipe->raw_handle);
			pipe->raw_handle = INVALID_HANDLE_VALUE;
			continue;
		}
		pipe->connected = true;
		{
			std::lock_guard<std::mutex> lock(pipes_mutex_);
			if (!running_)
			{
				pipe->connected = false;
				pipe->stream.close();
				pipe->raw_handle = INVALID_HANDLE_VALUE;
				continue;
			}
			pipes_.push_back(pipe);
		}
		boost::asio::post(pipe->strand, [this, pipe] {
			if (running_ && pipe->connected)
				start_read(pipe);
			});
	}
}
void IpcServer2::start_read(std::shared_ptr<PipeInstance> pipe)
{
	auto self = pipe;

	pipe->stream.async_read_some(
		boost::asio::buffer(pipe->buffer),
		boost::asio::bind_executor(
			pipe->strand,
			[this, self](const boost::system::error_code& ec, std::size_t bytes)
			{
				if (ec) {
					self->connected = false;

					if (self->stream.is_open()) 
					{
						self->stream.close();
						self->raw_handle = INVALID_HANDLE_VALUE;
					}

					std::lock_guard<std::mutex> lock(pipes_mutex_); 
					pipes_.erase(std::remove(pipes_.begin(), pipes_.end(), self), pipes_.end());

					return;
				}

				std::wstring msg(self->buffer.data(), bytes / sizeof(wchar_t));
				callback_(msg, object_);

				start_read(self);
			}));
}
bool IpcServer2::send(std::wstring msg, int pipe_index)
{
	if (!running_) 
		return false;

	std::lock_guard<std::mutex> lock(pipes_mutex_);

	if (pipe_index >= 0 && pipe_index < (int)pipes_.size()) {
		auto& p = pipes_[pipe_index];
		if (p->connected)
			do_write(p, std::move(msg));
		return true;
	}

	for (auto& p : pipes_)
		if (p->connected)
			do_write(p, msg);

	return true;
}
void IpcServer2::do_write(std::shared_ptr<PipeInstance> pipe, std::wstring msg)
{
	auto buffer = std::make_shared<std::wstring>(std::move(msg));

	boost::asio::async_write(
		pipe->stream,
		boost::asio::buffer(buffer->data(), buffer->size() * sizeof(wchar_t)),
		boost::asio::bind_executor(
			pipe->strand,
			[buffer](const boost::system::error_code&, std::size_t) {}));
}
bool IpcServer2::terminate()
{
	if (!running_) 
		return false;

	running_ = false;

	{
		std::lock_guard<std::mutex> lock(pipes_mutex_);
		for (auto& p : pipes_) {
			p->connected = false;
			if (p->stream.is_open())
			{
				p->stream.close();
				p->raw_handle = INVALID_HANDLE_VALUE;
			}
		}
	}
	HANDLE h = pending_handle_.load();
	if (h != INVALID_HANDLE_VALUE)
		CancelIoEx(h, nullptr);

	work_.reset();
	io_.stop();
	if (io_thread_.joinable()) 
		io_thread_.join();
	if (accept_thread_.joinable()) 
		accept_thread_.join();

	pipes_.clear();
	return true;
}

IpcClient2::IpcClient2(std::wstring name,
	void (*callback)(std::wstring, LPVOID),
	LPVOID object,
	bool message_mode)
	: name_(std::move(name)),
	message_mode_(message_mode),
	callback_(callback),
	object_(object),
	stream_(io_),
	work_(boost::asio::make_work_guard(io_))
{
	running_ = true;

	// Start IO thread
	io_thread_ = std::thread([this] {
		io_.run();
		});

	// Start (re)connect loop
	connect_thread_ = std::thread([this] {
		connect_loop();
		});
}
IpcClient2::~IpcClient2()
{
	terminate();
}
bool IpcClient2::terminate()
{
	if (!running_) 
		return false;

	running_ = false;

	// Serialize native-handle teardown against synchronous overlapped writes.
	{
		std::lock_guard<std::mutex> lock(send_mutex_);
		if (stream_.is_open())
		{
			stream_.close();
			raw_ = INVALID_HANDLE_VALUE;
		}
		else if (raw_ != INVALID_HANDLE_VALUE)
		{
			CloseHandle(raw_);
			raw_ = INVALID_HANDLE_VALUE;
		}
	}

	work_.reset();
	io_.stop();
	if (io_thread_.joinable()) 
		io_thread_.join();

	if (connect_thread_.joinable()) 
		connect_thread_.join();
	return true;
}
void IpcClient2::connect_loop()
{
	while (running_) {
		HANDLE connected_handle = CreateFileW(
			name_.c_str(),
			GENERIC_READ | GENERIC_WRITE,
			0,
			nullptr,
			OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED,
			nullptr);

		if (connected_handle == INVALID_HANDLE_VALUE)
		{
			DWORD err = GetLastError();
			if (err == ERROR_PIPE_BUSY) {
				WaitNamedPipeW(name_.c_str(), 100);
				continue;
			}		
			// Pipe not available yet → retry
			if (running_)
				Sleep(100);
		}
		else {
			if (message_mode_)
			{
				DWORD mode = PIPE_READMODE_MESSAGE;
				if (!SetNamedPipeHandleState(connected_handle, &mode, nullptr, nullptr))
				{
					CloseHandle(connected_handle);
					if (running_) Sleep(100);
					continue;
				}
			}
			{
				std::lock_guard<std::mutex> lock(send_mutex_);
				if (!running_)
				{
					CloseHandle(connected_handle);
					return;
				}
				raw_ = connected_handle;
			}
			// Connected. Assignment is serialized with send/terminate so the
			// native HANDLE cannot be closed or replaced while a write is pending.
			boost::asio::post(io_, [this] {
				std::lock_guard<std::mutex> lock(send_mutex_);
				if (!running_ || raw_ == INVALID_HANDLE_VALUE)
					return;
				stream_.assign(raw_);
				start_read();
				});

			while (running_)
			{
				if (reconnect_.exchange(false))
					break;
				Sleep(100);
			}
			if(!running_)
				return;
		}

	}
}
void IpcClient2::start_read()
{
	stream_.async_read_some(
		boost::asio::buffer(buffer_),
		[this](const boost::system::error_code& ec, std::size_t bytes)
		{
			if (ec) {
				// Serialize close/reconnect against a pending synchronous write.
				{
					std::lock_guard<std::mutex> lock(send_mutex_);
					if (stream_.is_open())
					{
						stream_.close();
						raw_ = INVALID_HANDLE_VALUE;
					}
				}
				
				if(running_)
					reconnect_.store(true);
				return;
			}

			std::wstring msg(buffer_.data(), bytes / sizeof(wchar_t));
			if (callback_) 
				callback_(msg, object_);

			start_read();
		});
}
bool IpcClient2::send(const std::wstring msg)
{
	std::lock_guard<std::mutex> lock(send_mutex_);
	if (!running_ || raw_ == INVALID_HANDLE_VALUE)
		return false;

	HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!event)
		return false;
	OVERLAPPED overlapped{};
	overlapped.hEvent = event;
	DWORD written = 0;
	BOOL ok = WriteFile(
		raw_,
		msg.data(),
		(DWORD)(msg.size() * sizeof(wchar_t)),
		&written,
		&overlapped);
	if (!ok && GetLastError() == ERROR_IO_PENDING)
	{
		if (WaitForSingleObject(event, 5000) == WAIT_OBJECT_0)
			ok = GetOverlappedResult(raw_, &overlapped, &written, FALSE);
		else
		{
			// CancelIoEx only requests cancellation. The OVERLAPPED and its event
			// must remain alive until the kernel reports final completion.
			CancelIoEx(raw_, &overlapped);
			DWORD ignored = 0;
			GetOverlappedResult(raw_, &overlapped, &ignored, TRUE);
			ok = FALSE;
		}
	}
	const bool complete = ok == TRUE && written == msg.size() * sizeof(wchar_t);
	CloseHandle(event);
	return complete;
}

