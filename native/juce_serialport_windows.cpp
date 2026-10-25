// Windows implementation, included by juce_serialport.cpp
//
// the handle is opened for overlapped I/O, so the reader and writer threads can use it at the same time.
// with a non-overlapped handle Windows serialises the calls, and a pending read would block every write

struct SerialPort::Native
{
    Native ()
        : readEvent { CreateEventW (nullptr, TRUE, FALSE, nullptr) },
          writeEvent { CreateEventW (nullptr, TRUE, FALSE, nullptr) }
    {
    }

    ~Native ()
    {
        if (readEvent != nullptr)
            CloseHandle (readEvent);
        if (writeEvent != nullptr)
            CloseHandle (writeEvent);
    }

    HANDLE handle { INVALID_HANDLE_VALUE };
    // one event per direction, as reads and writes happen on different threads
    HANDLE readEvent { nullptr };
    HANDLE writeEvent { nullptr };
};

namespace
{
    // waits for an overlapped operation to finish, cancelling it if the timeout expires.
    // returns the number of bytes transferred, or -1 if the operation failed
    int finishOverlappedIo (HANDLE handle, OVERLAPPED& overlapped, int timeoutMs)
    {
        if (WaitForSingleObject (overlapped.hEvent, static_cast<DWORD> (timeoutMs)) != WAIT_OBJECT_0)
            CancelIoEx (handle, &overlapped);

        // always wait for completion, so the kernel is no longer using the buffer or the OVERLAPPED when this returns
        DWORD bytesTransferred { 0 };
        if (GetOverlappedResult (handle, &overlapped, &bytesTransferred, TRUE))
            return static_cast<int> (bytesTransferred);

        // a timed out, or cancelled, operation may still have transferred some data
        if (GetLastError () == ERROR_OPERATION_ABORTED)
            return static_cast<int> (bytesTransferred);

        return -1;
    }
}

juce::StringPairArray SerialPort::getSerialPortPaths ()
{
    juce::StringPairArray serialPortPaths;

    HKEY serialCommKey { nullptr };
    if (RegOpenKeyExW (HKEY_LOCAL_MACHINE, L"HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &serialCommKey) != ERROR_SUCCESS)
        return serialPortPaths;

    DWORD maxValueNameLen { 0 }; // in characters, not including the terminator
    DWORD maxValueDataLen { 0 }; // in bytes
    if (RegQueryInfoKeyW (serialCommKey, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, &maxValueNameLen, &maxValueDataLen, nullptr, nullptr) == ERROR_SUCCESS)
    {
        juce::HeapBlock<WCHAR> valueName { maxValueNameLen + 1, true };
        // room for a terminator, as REG_SZ data is not guaranteed to have one
        const auto valueDataChars { maxValueDataLen / sizeof (WCHAR) + 1 };
        juce::HeapBlock<WCHAR> valueData { valueDataChars, true };

        for (DWORD index { 0 };; ++index)
        {
            auto valueNameLen { maxValueNameLen + 1 };
            auto valueDataLen { static_cast<DWORD> ((valueDataChars - 1) * sizeof (WCHAR)) };
            DWORD valueType { 0 };
            std::fill (valueData.get (), valueData.get () + valueDataChars, WCHAR { 0 });
            const auto result { RegEnumValueW (serialCommKey, index, valueName.get (), &valueNameLen, nullptr, &valueType,
                                               reinterpret_cast<BYTE*> (valueData.get ()), &valueDataLen) };
            if (result != ERROR_SUCCESS)
                break;

            if (valueType == REG_SZ)
            {
                const juce::String portName { valueData.get () };
                if (portName.isNotEmpty ())
                    serialPortPaths.set (portName, "\\\\.\\" + portName);
            }
        }
    }

    RegCloseKey (serialCommKey);
    return serialPortPaths;
}

bool SerialPort::isOpen () const
{
    return native->handle != INVALID_HANDLE_VALUE;
}

void SerialPort::close ()
{
    // the streams use the handle, so they must be deleted before the port is closed
    jassert (numAttachedStreams == 0);

    if (native->handle != INVALID_HANDLE_VALUE)
    {
        DebugLog ("SerialPort::close", "closing port:" + portPath);
        CloseHandle (native->handle);
        native->handle = INVALID_HANDLE_VALUE;
    }
}

bool SerialPort::open (const juce::String& newPortPath)
{
    close ();

    portPath = newPortPath;
    connectionLost = false;

    if (native->readEvent == nullptr || native->writeEvent == nullptr)
    {
        DebugLog ("SerialPort::open", "CreateEvent failed");
        return false;
    }

    // COM10 and above can only be opened with the device namespace prefix
    const auto devicePath { portPath.startsWith ("\\\\.\\") ? portPath : "\\\\.\\" + portPath };
    native->handle = CreateFileW (devicePath.toWideCharPointer (), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (native->handle == INVALID_HANDLE_VALUE)
    {
        DebugLog ("SerialPort::open", "CreateFile failed, error: " + juce::String (GetLastError ()));
        return false;
    }

    // XonLim and XoffLim, set in setConfig, are relative to the input buffer size
    SetupComm (native->handle, 4096, 4096);

    // a read returns as soon as any data is available, or after ReadTotalTimeoutConstant if there is none.
    // writes do not time out here, readBytes and writeBytes cancel them instead
    COMMTIMEOUTS commTimeouts {};
    commTimeouts.ReadIntervalTimeout = MAXDWORD;
    commTimeouts.ReadTotalTimeoutMultiplier = MAXDWORD;
    commTimeouts.ReadTotalTimeoutConstant = 100;
    commTimeouts.WriteTotalTimeoutMultiplier = 0;
    commTimeouts.WriteTotalTimeoutConstant = 0;
    if (! SetCommTimeouts (native->handle, &commTimeouts))
    {
        DebugLog ("SerialPort::open", "SetCommTimeouts failed, error: " + juce::String (GetLastError ()));
        close ();
        return false;
    }

    DWORD commErrors { 0 };
    ClearCommError (native->handle, &commErrors, nullptr);

    return true;
}

void SerialPort::cancel ()
{
    if (native->handle != INVALID_HANDLE_VALUE)
        CancelIoEx (native->handle, nullptr);
}

bool SerialPort::setConfig (const SerialPortConfig& config)
{
    if (! isOpen ())
        return false;

    // start from the current state, so the fields not set here keep the driver's values
    DCB dcb {};
    dcb.DCBlength = sizeof (DCB);
    if (! GetCommState (native->handle, &dcb))
        return false;

    dcb.BaudRate = config.bps;
    dcb.ByteSize = static_cast<BYTE> (config.databits);
    dcb.fBinary = TRUE;
    dcb.fParity = TRUE;
    dcb.fErrorChar = FALSE;
    dcb.fNull = FALSE;
    dcb.fAbortOnError = FALSE;
    dcb.fDsrSensitivity = FALSE;
    dcb.fTXContinueOnXoff = TRUE;
    dcb.XonLim = 2048;
    dcb.XoffLim = 512;
    dcb.XonChar = 0x11;
    dcb.XoffChar = 0x13;
    switch (config.parity)
    {
        case SerialPortConfig::SERIALPORT_PARITY_ODD:   dcb.Parity = ODDPARITY; break;
        case SerialPortConfig::SERIALPORT_PARITY_EVEN:  dcb.Parity = EVENPARITY; break;
        case SerialPortConfig::SERIALPORT_PARITY_MARK:  dcb.Parity = MARKPARITY; break;
        case SerialPortConfig::SERIALPORT_PARITY_SPACE: dcb.Parity = SPACEPARITY; break;
        case SerialPortConfig::SERIALPORT_PARITY_NONE:
        default:
            dcb.Parity = NOPARITY;
            dcb.fParity = FALSE;
            break;
    }
    switch (config.stopbits)
    {
        case SerialPortConfig::STOPBITS_1ANDHALF: dcb.StopBits = ONE5STOPBITS; break;
        case SerialPortConfig::STOPBITS_2:        dcb.StopBits = TWOSTOPBITS; break;
        case SerialPortConfig::STOPBITS_1:
        default:
            dcb.StopBits = ONESTOPBIT;
            break;
    }
    // DTR and RTS are asserted unless they are used for handshaking, as many devices (Arduinos, etc) wait for DTR
    dcb.fOutxDsrFlow = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    switch (config.flowcontrol)
    {
        case SerialPortConfig::FLOWCONTROL_XONXOFF:
            dcb.fOutxCtsFlow = FALSE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
            dcb.fOutX = TRUE;
            dcb.fInX = TRUE;
            break;
        case SerialPortConfig::FLOWCONTROL_HARDWARE:
            // RTS/CTS, which is what the other platforms mean by hardware flow control
            dcb.fOutxCtsFlow = TRUE;
            dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
            dcb.fOutX = FALSE;
            dcb.fInX = FALSE;
            break;
        case SerialPortConfig::FLOWCONTROL_NONE:
        default:
            dcb.fOutxCtsFlow = FALSE;
            dcb.fRtsControl = RTS_CONTROL_ENABLE;
            dcb.fOutX = FALSE;
            dcb.fInX = FALSE;
            break;
    }

    if (! SetCommState (native->handle, &dcb))
    {
        DebugLog ("SerialPort::setConfig", "SetCommState failed, error: " + juce::String (GetLastError ()));
        return false;
    }
    return true;
}

bool SerialPort::getConfig (SerialPortConfig& config)
{
    if (! isOpen ())
        return false;

    DCB dcb {};
    dcb.DCBlength = sizeof (DCB);
    if (! GetCommState (native->handle, &dcb))
        return false;

    config.bps = dcb.BaudRate;
    config.databits = dcb.ByteSize;
    switch (dcb.Parity)
    {
        case ODDPARITY:   config.parity = SerialPortConfig::SERIALPORT_PARITY_ODD; break;
        case EVENPARITY:  config.parity = SerialPortConfig::SERIALPORT_PARITY_EVEN; break;
        case MARKPARITY:  config.parity = SerialPortConfig::SERIALPORT_PARITY_MARK; break;
        case SPACEPARITY: config.parity = SerialPortConfig::SERIALPORT_PARITY_SPACE; break;
        case NOPARITY:
        default:
            config.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
            break;
    }
    switch (dcb.StopBits)
    {
        case ONE5STOPBITS: config.stopbits = SerialPortConfig::STOPBITS_1ANDHALF; break;
        case TWOSTOPBITS:  config.stopbits = SerialPortConfig::STOPBITS_2; break;
        case ONESTOPBIT:
        default:
            config.stopbits = SerialPortConfig::STOPBITS_1;
            break;
    }
    if (dcb.fOutX && dcb.fInX)
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_XONXOFF;
    else if (dcb.fOutxCtsFlow || dcb.fRtsControl == RTS_CONTROL_HANDSHAKE)
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_HARDWARE;
    else
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
    return true;
}

int SerialPort::readBytes (void* destBuffer, int maxBytesToRead, int timeoutMs)
{
    OVERLAPPED overlapped {};
    overlapped.hEvent = native->readEvent;
    ResetEvent (overlapped.hEvent);

    // the byte count must come from GetOverlappedResult, ReadFile's is not reliable for overlapped I/O
    if (! ReadFile (native->handle, destBuffer, static_cast<DWORD> (maxBytesToRead), nullptr, &overlapped) && GetLastError () != ERROR_IO_PENDING)
        return -1;

    // the read completes after ReadTotalTimeoutConstant, the extra time is for drivers which do not honour that
    const auto bytesRead { finishOverlappedIo (native->handle, overlapped, timeoutMs + 100) };
    if (bytesRead != 0)
        return bytesRead;

    // some USB serial drivers never fail the read when the device is unplugged, but GetCommState does fail
    DCB dcb {};
    dcb.DCBlength = sizeof (DCB);
    return GetCommState (native->handle, &dcb) ? 0 : -1;
}

int SerialPort::writeBytes (const void* sourceBuffer, int numBytesToWrite, int timeoutMs)
{
    OVERLAPPED overlapped {};
    overlapped.hEvent = native->writeEvent;
    ResetEvent (overlapped.hEvent);

    if (! WriteFile (native->handle, sourceBuffer, static_cast<DWORD> (numBytesToWrite), nullptr, &overlapped) && GetLastError () != ERROR_IO_PENDING)
        return -1;

    return finishOverlappedIo (native->handle, overlapped, timeoutMs);
}
