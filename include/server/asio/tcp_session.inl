/*!
    \file tcp_session.inl
    \brief TCP session inline implementation
    \author Ivan Shynkarenka
    \date 14.12.2016
    \copyright MIT License
*/

namespace CppServer {
namespace Asio {

template <class TServer, class TSession>
inline TCPSession<TServer, TSession>::TCPSession(std::shared_ptr<TCPServer<TServer, TSession>> server)
    : _id(CppCommon::UUID::Generate()),
      _server(server),
      _io_service(server->service()->GetAsioService()),
      _strand(*_io_service),
      _strand_required(_server->_strand_required),
      _socket(*_io_service),
      _connected(false),
      _bytes_sent(0),
      _bytes_received(0),
      _reciving(false),
      _recive_buffer(CHUNK + 1),
      _sending(false),
      _send_buffer_flush_offset(0)
{
}

template <class TServer, class TSession>
inline void TCPSession<TServer, TSession>::Connect()
{
    // Apply the option: no delay
    if (_server->option_no_delay())
        _socket.set_option(asio::ip::tcp::no_delay(true));

    // Reset statistic
    _bytes_sent = 0;
    _bytes_received = 0;

    // Update the connected flag
    _connected = true;

    // Call the session connected handler
    onConnected();

    // Call the session connected handler in the server
    //_server->onConnected(self);

    // Call the empty send buffer handler
    onEmpty();

    // Try to receive something from the client
    TryReceive();
}

template <class TServer, class TSession>
inline bool TCPSession<TServer, TSession>::Disconnect(bool dispatch)
{
    if (!IsConnected())
        return false;

    // Dispatch or post the disconnect handler
    auto self(this->shared_from_this());
    auto disconnect_handler = make_alloc_handler(_connect_storage, [this, self]()
    {
        if (!IsConnected())
            return;

        // Close the session socket
        _socket.close();

        // Clear receive/send buffers
        ClearBuffers();

        // Update the connected flag
        _connected = false;

        // Call the session disconnected handler
        onDisconnected();

        // Call the session disconnected handler in the server
        //_server->onDisconnected(self);

        // Dispatch the unregister session handler
        auto unregister_session_handler = make_alloc_handler(_connect_storage, [this, self]()
        {
            _server->UnregisterSession(id());
        });
        if (_server->_strand_required)
            _server->_strand.dispatch(unregister_session_handler);
        else
            _server->_io_service->dispatch(unregister_session_handler);
    });
    if (_strand_required)
    {
        if (dispatch)
            _strand.dispatch(disconnect_handler);
        else
            _strand.post(disconnect_handler);
    }
    else
    {
        if (dispatch)
            _io_service->dispatch(disconnect_handler);
        else
            _io_service->post(disconnect_handler);
    }

    return true;
}

template <class TServer, class TSession>
inline size_t TCPSession<TServer, TSession>::Send(const void* buffer, size_t size)
{
    assert((buffer != nullptr) && "Pointer to the buffer should not be equal to 'nullptr'!");
    assert((size > 0) && "Buffer size should be greater than zero!");
    if ((buffer == nullptr) || (size == 0))
        return 0;

    if (!IsConnected())
        return 0;

    size_t result;
    {
        std::lock_guard<std::mutex> locker(_send_lock);

        // Fill the main send buffer
        const uint8_t* bytes = (const uint8_t*)buffer;
        _send_buffer_main.insert(_send_buffer_main.end(), bytes, bytes + size);
        result = _send_buffer_main.size();
    }

    // Dispatch the send handler
    auto self(this->shared_from_this());
    auto send_handler = make_alloc_handler(_send_storage, [this, self]()
    {
        // Try to send the main buffer
        TrySend();
    });
    if (_strand_required)
        _strand.dispatch(send_handler);
    else
        _io_service->dispatch(send_handler);

    return result;
}

template <class TServer, class TSession>
inline void TCPSession<TServer, TSession>::TryReceive()
{
    if (_reciving)
        return;

    if (!IsConnected())
        return;

    // Async receive with the receive handler
    _reciving = true;
    auto self(this->shared_from_this());
    auto async_receive_handler = make_alloc_handler(_recive_storage, [this, self](std::error_code ec, std::size_t size)
    {
        _reciving = false;

        if (!IsConnected())
            return;

        // Received some data from the client
        if (size > 0)
        {
            // Update statistic
            _bytes_received += size;
            _server->_bytes_received += size;

            // If the receive buffer is full increase its size
            if (_recive_buffer.size() == size)
                _recive_buffer.resize(2 * size);

            // Call the buffer received handler
            onReceived(_recive_buffer.data(), size);
        }

        // Try to receive again if the session is valid
        if (!ec)
            TryReceive();
        else
        {
            SendError(ec);
            Disconnect(true);
        }
    });
    if (_strand_required)
        _socket.async_read_some(asio::buffer(_recive_buffer.data(), _recive_buffer.size()), bind_executor(_strand, async_receive_handler));
    else
        _socket.async_read_some(asio::buffer(_recive_buffer.data(), _recive_buffer.size()), async_receive_handler);
}

template <class TServer, class TSession>
inline void TCPSession<TServer, TSession>::TrySend()
{
    if (_sending)
        return;

    if (!IsConnected())
        return;

    // Swap send buffers
    if (_send_buffer_flush.empty())
    {
        std::lock_guard<std::mutex> locker(_send_lock);

        // Swap flush and main buffers
        _send_buffer_flush.swap(_send_buffer_main);
        _send_buffer_flush_offset = 0;
    }

    // Check if the flush buffer is empty
    if (_send_buffer_flush.empty())
    {
        // Call the empty send buffer handler
        onEmpty();
        return;
    }

    // Async write with the write handler
    _sending = true;
    auto self(this->shared_from_this());
    auto async_write_handler = make_alloc_handler(_send_storage, [this, self](std::error_code ec, std::size_t size)
    {
        _sending = false;

        if (!IsConnected())
            return;

        // Send some data to the client
        if (size > 0)
        {
            // Update statistic
            _bytes_sent += size;
            _server->_bytes_sent += size;

            // Increase the flush buffer offset
            _send_buffer_flush_offset += size;

            // Successfully send the whole flush buffer
            if (_send_buffer_flush_offset == _send_buffer_flush.size())
            {
                // Clear the flush buffer
                _send_buffer_flush.clear();
                _send_buffer_flush_offset = 0;
            }

            // Call the buffer sent handler
            onSent(size, _send_buffer_flush.size() - _send_buffer_flush_offset);
        }

        // Try to send again if the session is valid
        if (!ec)
        {
            TrySend();
        }
        else
        {
            SendError(ec);
            Disconnect(true);
        }
    });
    if (_strand_required)
        asio::async_write(_socket, asio::buffer(_send_buffer_flush.data() + _send_buffer_flush_offset, _send_buffer_flush.size() - _send_buffer_flush_offset), bind_executor(_strand, async_write_handler));
    else
        asio::async_write(_socket, asio::buffer(_send_buffer_flush.data() + _send_buffer_flush_offset, _send_buffer_flush.size() - _send_buffer_flush_offset), async_write_handler);
}

template <class TServer, class TSession>
inline void TCPSession<TServer, TSession>::ClearBuffers()
{
    // Clear send buffers
    {
        std::lock_guard<std::mutex> locker(_send_lock);

        _send_buffer_main.clear();
        _send_buffer_flush.clear();
        _send_buffer_flush_offset = 0;
    }
}

template <class TServer, class TSession>
inline void TCPSession<TServer, TSession>::SendError(std::error_code ec)
{
    // Skip Asio disconnect errors
    if ((ec == asio::error::connection_aborted) ||
        (ec == asio::error::connection_refused) ||
        (ec == asio::error::connection_reset) ||
        (ec == asio::error::eof) ||
        (ec == asio::error::operation_aborted))
        return;

    onError(ec.value(), ec.category().name(), ec.message());
}

} // namespace Asio
} // namespace CppServer
