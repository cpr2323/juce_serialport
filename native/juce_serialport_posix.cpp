// macOS and Linux implementation, included by juce_serialport.cpp
//
// the descriptor is non-blocking. the stream threads wait for it to become ready with a timeout, so they can always
// exit, and a stalled device (flow control, etc) can not block a thread forever

struct SerialPort::Native
{
    int fd { -1 };
   #if JUCE_MAC
    // the rate set with IOSSIOSPEED, which termios does not report
    uint32_t customBps { 0 };
   #endif
};

namespace
{
    enum class WaitResult { ready, timedOut, failed };

    WaitResult waitForDescriptor (int fd, bool forWriting, int timeoutMs)
    {
       #if JUCE_MAC
        // poll () does not support devices on macOS, so select () is used. it can only handle descriptors below
        // FD_SETSIZE, which a host with a lot of open files can exceed, so then fall back to trying every few ms
        if (fd >= FD_SETSIZE)
        {
            juce::Thread::sleep (juce::jmin (timeoutMs, 2));
            return WaitResult::ready;
        }

        fd_set fds;
        FD_ZERO (&fds);
        FD_SET (fd, &fds);
        timeval timeout { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
        const auto result { select (fd + 1, forWriting ? nullptr : &fds, forWriting ? &fds : nullptr, nullptr, &timeout) };
        if (result < 0)
            return errno == EINTR ? WaitResult::timedOut : WaitResult::failed;
        return result == 0 ? WaitResult::timedOut : WaitResult::ready;
       #else
        pollfd pfd { fd, static_cast<short> (forWriting ? POLLOUT : POLLIN), 0 };
        const auto result { poll (&pfd, 1, timeoutMs) };
        if (result < 0)
            return errno == EINTR ? WaitResult::timedOut : WaitResult::failed;
        if (result == 0)
            return WaitResult::timedOut;
        // POLLHUP is reported when a USB serial device is unplugged
        if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            return WaitResult::failed;
        return WaitResult::ready;
       #endif
    }

    bool isTransientError (int error)
    {
        return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
    }

   #if JUCE_LINUX
    // termios can only set the standard rates on Linux, other rates are set with termios2. it is declared here, as
    // <asm/termbits.h> conflicts with <termios.h>. the layout, and the BOTHER/CBAUD values, are the same on x86,
    // ARM and RISC-V, other architectures get the standard rates only
    #if defined (__x86_64__) || defined (__i386__) || defined (__aarch64__) || defined (__arm__) || defined (__riscv)
     #define JUCE_SERIALPORT_HAS_TERMIOS2 1
    struct SerialPortTermios2
    {
        tcflag_t c_iflag;
        tcflag_t c_oflag;
        tcflag_t c_cflag;
        tcflag_t c_lflag;
        cc_t c_line;
        cc_t c_cc[19];
        speed_t c_ispeed;
        speed_t c_ospeed;
    };
    constexpr tcflag_t kSerialPortCBaud { 0010017 };
    constexpr tcflag_t kSerialPortBOther { 0010000 };
    #endif

    struct StandardSpeed
    {
        uint32_t bps;
        speed_t speed;
    };

    constexpr StandardSpeed kStandardSpeeds[] {
        { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 }, { 200, B200 }, { 300, B300 }, { 600, B600 },
        { 1200, B1200 }, { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 }, { 19200, B19200 }, { 38400, B38400 },
        { 57600, B57600 }, { 115200, B115200 }, { 230400, B230400 }, { 460800, B460800 }, { 500000, B500000 }, { 576000, B576000 },
        { 921600, B921600 }, { 1000000, B1000000 }, { 1152000, B1152000 }, { 1500000, B1500000 }, { 2000000, B2000000 }
    };

    // returns B0 if the rate is not a standard one
    speed_t getStandardSpeed (uint32_t bps)
    {
        for (const auto& standardSpeed : kStandardSpeeds)
            if (standardSpeed.bps == bps)
                return standardSpeed.speed;
        return B0;
    }

    uint32_t getSpeedValue (speed_t speed)
    {
        for (const auto& standardSpeed : kStandardSpeeds)
            if (standardSpeed.speed == speed)
                return standardSpeed.bps;
        return 0;
    }
   #endif
}

#if JUCE_MAC
juce::StringPairArray SerialPort::getSerialPortPaths ()
{
    juce::StringPairArray serialPortPaths;

    auto matchingDictionary { IOServiceMatching (kIOSerialBSDServiceValue) };
    if (matchingDictionary == nullptr)
        return serialPortPaths;
    CFDictionarySetValue (matchingDictionary, CFSTR (kIOSerialBSDTypeKey), CFSTR (kIOSerialBSDAllTypes));

    // IOServiceGetMatchingServices consumes the matching dictionary. MACH_PORT_NULL selects the default main port,
    // and avoids kIOMasterPortDefault (deprecated) and kIOMainPortDefault (macOS 12 and above)
    io_iterator_t serviceIterator { 0 };
    if (IOServiceGetMatchingServices (MACH_PORT_NULL, matchingDictionary, &serviceIterator) != KERN_SUCCESS)
        return serialPortPaths;

    auto getStringProperty = [] (io_object_t service, CFStringRef key)
    {
        juce::String value;
        if (auto property { IORegistryEntryCreateCFProperty (service, key, kCFAllocatorDefault, 0) })
        {
            if (CFGetTypeID (property) == CFStringGetTypeID ())
            {
                char buffer [1024];
                if (CFStringGetCString (static_cast<CFStringRef> (property), buffer, sizeof (buffer), kCFStringEncodingUTF8))
                    value = juce::String::fromUTF8 (buffer);
            }
            CFRelease (property);
        }
        return value;
    };

    while (auto service { IOIteratorNext (serviceIterator) })
    {
        const auto devicePath { getStringProperty (service, CFSTR (kIODialinDeviceKey)) };
        auto friendlyName { getStringProperty (service, CFSTR (kIOTTYDeviceKey)) };
        if (friendlyName.isEmpty ())
            friendlyName = devicePath;
        if (devicePath.isNotEmpty ())
            serialPortPaths.set (friendlyName, devicePath);
        IOObjectRelease (service);
    }
    IOObjectRelease (serviceIterator);

    return serialPortPaths;
}
#elif JUCE_LINUX
juce::StringPairArray SerialPort::getSerialPortPaths ()
{
    juce::StringPairArray serialPortPaths;

    // every tty has an entry here, the ones with a device link are backed by hardware
    for (const auto& entry : juce::RangedDirectoryIterator (juce::File ("/sys/class/tty"), false, "*", juce::File::findFilesAndDirectories))
    {
        const auto ttyDir { entry.getFile () };
        const auto deviceDir { ttyDir.getChildFile ("device") };
        if (! deviceDir.exists ())
            continue;

        // the legacy 8250 driver creates ttyS0-ttyS31 whether there is hardware or not, a type of 0 means there is none
        const auto typeFile { ttyDir.getChildFile ("type") };
        if (typeFile.existsAsFile () && typeFile.loadFileAsString ().trim () == "0")
            continue;

        const auto devicePath { "/dev/" + ttyDir.getFileName () };
        if (! juce::File (devicePath).exists ())
            continue;

        // for USB devices, the product name is in a parent of the device directory
        juce::String productName;
        auto parentDir { deviceDir.getLinkedTarget () };
        for (auto level { 0 }; level < 4 && productName.isEmpty () && parentDir.exists (); ++level)
        {
            const auto productFile { parentDir.getChildFile ("product") };
            if (productFile.existsAsFile ())
                productName = productFile.loadFileAsString ().trim ();
            parentDir = parentDir.getParentDirectory ();
        }

        const auto friendlyName { productName.isNotEmpty () ? ttyDir.getFileName () + " (" + productName + ")" : ttyDir.getFileName () };
        serialPortPaths.set (friendlyName, devicePath);
    }

    return serialPortPaths;
}
#else
juce::StringPairArray SerialPort::getSerialPortPaths ()
{
    // not implemented, but ports can still be opened by path
    return {};
}
#endif

bool SerialPort::isOpen () const
{
    return native->fd != -1;
}

void SerialPort::close ()
{
    // the streams use the descriptor, so they must be deleted before the port is closed
    jassert (numAttachedStreams == 0);

    if (native->fd != -1)
    {
        DebugLog ("SerialPort::close", "closing port:" + portPath);
        ::close (native->fd);
        native->fd = -1;
    }
}

bool SerialPort::open (const juce::String& newPortPath)
{
    close ();

    portPath = newPortPath;
    connectionLost = false;
   #if JUCE_MAC
    native->customBps = 0;
   #endif
    DebugLog ("SerialPort::open", "opening port:" + portPath);

    // O_NONBLOCK also stops the open from waiting for carrier detect on ports which need it
    native->fd = ::open (portPath.toRawUTF8 (), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (native->fd == -1)
    {
        DebugLog ("SerialPort::open", "open () failed, errno: " + juce::String (errno));
        return false;
    }

    // don't allow multiple opens
    if (ioctl (native->fd, TIOCEXCL) == -1)
        DebugLog ("SerialPort::open", "TIOCEXCL failed, non critical");

    termios options {};
    if (tcgetattr (native->fd, &options) == -1)
    {
        DebugLog ("SerialPort::open", "can't get port settings, errno: " + juce::String (errno));
        close ();
        return false;
    }

    // raw mode. with O_NONBLOCK and VMIN 1, a read with no data available fails with EAGAIN, so a read
    // returning 0 means end of file, ie. the device has gone away
    cfmakeraw (&options);
    options.c_cflag |= CREAD | CLOCAL;
    options.c_cc[VMIN] = 1;
    options.c_cc[VTIME] = 0;
    if (tcsetattr (native->fd, TCSANOW, &options) == -1)
    {
        DebugLog ("SerialPort::open", "can't set port settings, errno: " + juce::String (errno));
        close ();
        return false;
    }

    tcflush (native->fd, TCIOFLUSH);
    return true;
}

void SerialPort::cancel ()
{
    // nothing to do, the stream threads never block for longer than their timeouts
}

bool SerialPort::setConfig (const SerialPortConfig& config)
{
    if (! isOpen ())
        return false;

    termios options {};
    if (tcgetattr (native->fd, &options) == -1)
    {
        DebugLog ("SerialPort::setConfig", "can't get port settings, errno: " + juce::String (errno));
        return false;
    }

    cfmakeraw (&options);
    options.c_cflag |= CREAD | CLOCAL; // enable the receiver, and don't monitor the modem control lines
    options.c_cc[VMIN] = 1;
    options.c_cc[VTIME] = 0;

    options.c_cflag &= ~static_cast<tcflag_t> (CSIZE);
    switch (config.databits)
    {
        case 5: options.c_cflag |= CS5; break;
        case 6: options.c_cflag |= CS6; break;
        case 7: options.c_cflag |= CS7; break;
        case 8: options.c_cflag |= CS8; break;
        default:
            DebugLog ("SerialPort::setConfig", "unsupported number of data bits: " + juce::String (config.databits));
            return false;
    }

    options.c_cflag &= ~static_cast<tcflag_t> (PARENB | PARODD);
    switch (config.parity)
    {
        case SerialPortConfig::SERIALPORT_PARITY_ODD:
            options.c_cflag |= PARENB | PARODD;
            break;
        case SerialPortConfig::SERIALPORT_PARITY_EVEN:
            options.c_cflag |= PARENB;
            break;
        case SerialPortConfig::SERIALPORT_PARITY_MARK:
        case SerialPortConfig::SERIALPORT_PARITY_SPACE:
           #if JUCE_LINUX && defined (CMSPAR)
            options.c_cflag |= PARENB | CMSPAR | (config.parity == SerialPortConfig::SERIALPORT_PARITY_MARK ? PARODD : 0);
            break;
           #else
            DebugLog ("SerialPort::setConfig", "SERIALPORT_PARITY_MARK and SERIALPORT_PARITY_SPACE are not supported on this platform");
            return false;
           #endif
        case SerialPortConfig::SERIALPORT_PARITY_NONE:
        default:
            break;
    }

    if (config.stopbits == SerialPortConfig::STOPBITS_1ANDHALF)
    {
        DebugLog ("SerialPort::setConfig", "STOPBITS_1ANDHALF is not supported on this platform");
        return false;
    }
    if (config.stopbits == SerialPortConfig::STOPBITS_2)
        options.c_cflag |= CSTOPB;
    else
        options.c_cflag &= ~static_cast<tcflag_t> (CSTOPB);

    options.c_iflag &= ~static_cast<tcflag_t> (IXON | IXOFF | IXANY);
   #if JUCE_MAC
    options.c_cflag &= ~static_cast<tcflag_t> (CCTS_OFLOW | CRTS_IFLOW);
   #else
    options.c_cflag &= ~static_cast<tcflag_t> (CRTSCTS);
   #endif
    switch (config.flowcontrol)
    {
        case SerialPortConfig::FLOWCONTROL_XONXOFF:
            options.c_iflag |= IXON | IXOFF;
            break;
        case SerialPortConfig::FLOWCONTROL_HARDWARE:
           #if JUCE_MAC
            options.c_cflag |= CCTS_OFLOW | CRTS_IFLOW;
           #else
            options.c_cflag |= CRTSCTS;
           #endif
            break;
        case SerialPortConfig::FLOWCONTROL_NONE:
        default:
            break;
    }

   #if JUCE_MAC
    // IOSSIOSPEED, below, sets the actual rate, which can be any value. tcsetattr needs a valid one, so use 9600
    cfsetspeed (&options, B9600);
   #elif JUCE_LINUX
    const auto standardSpeed { getStandardSpeed (config.bps) };
    #if ! JUCE_SERIALPORT_HAS_TERMIOS2
    if (standardSpeed == B0)
    {
        DebugLog ("SerialPort::setConfig", "unsupported baud rate: " + juce::String (config.bps));
        return false;
    }
    #endif
    // non-standard rates are set with termios2 below
    cfsetspeed (&options, standardSpeed != B0 ? standardSpeed : B9600);
   #else
    if (cfsetspeed (&options, static_cast<speed_t> (config.bps)) == -1)
    {
        DebugLog ("SerialPort::setConfig", "unsupported baud rate: " + juce::String (config.bps));
        return false;
    }
   #endif

    if (tcsetattr (native->fd, TCSANOW, &options) == -1)
    {
        DebugLog ("SerialPort::setConfig", "can't set port settings, errno: " + juce::String (errno));
        return false;
    }

   #if JUCE_MAC
    speed_t speed { static_cast<speed_t> (config.bps) };
    if (ioctl (native->fd, IOSSIOSPEED, &speed) == -1)
    {
        DebugLog ("SerialPort::setConfig", "can't set baud rate: " + juce::String (config.bps) + ", errno: " + juce::String (errno));
        return false;
    }
    native->customBps = config.bps;
   #elif JUCE_LINUX && JUCE_SERIALPORT_HAS_TERMIOS2
    if (standardSpeed == B0)
    {
        SerialPortTermios2 options2 {};
        if (ioctl (native->fd, _IOR ('T', 0x2A, SerialPortTermios2), &options2) == -1)
        {
            DebugLog ("SerialPort::setConfig", "can't get port settings (termios2), errno: " + juce::String (errno));
            return false;
        }
        options2.c_cflag &= ~kSerialPortCBaud;
        options2.c_cflag |= kSerialPortBOther;
        options2.c_ispeed = config.bps;
        options2.c_ospeed = config.bps;
        if (ioctl (native->fd, _IOW ('T', 0x2B, SerialPortTermios2), &options2) == -1)
        {
            DebugLog ("SerialPort::setConfig", "can't set baud rate: " + juce::String (config.bps) + ", errno: " + juce::String (errno));
            return false;
        }
    }
   #endif

    return true;
}

bool SerialPort::getConfig (SerialPortConfig& config)
{
    if (! isOpen ())
        return false;

    termios options {};
    if (tcgetattr (native->fd, &options) == -1)
    {
        DebugLog ("SerialPort::getConfig", "can't get port settings, errno: " + juce::String (errno));
        return false;
    }

   #if JUCE_LINUX
    config.bps = getSpeedValue (cfgetospeed (&options));
    #if JUCE_SERIALPORT_HAS_TERMIOS2
    SerialPortTermios2 options2 {};
    if (ioctl (native->fd, _IOR ('T', 0x2A, SerialPortTermios2), &options2) == 0 && (options2.c_cflag & kSerialPortCBaud) == kSerialPortBOther)
        config.bps = options2.c_ospeed;
    #endif
   #else
    // speed_t is the actual rate on macOS and the BSDs
    config.bps = static_cast<uint32_t> (juce::jmax (cfgetispeed (&options), cfgetospeed (&options)));
    #if JUCE_MAC
    if (native->customBps != 0)
        config.bps = native->customBps;
    #endif
   #endif

    switch (options.c_cflag & CSIZE)
    {
        case CS5: config.databits = 5; break;
        case CS6: config.databits = 6; break;
        case CS7: config.databits = 7; break;
        case CS8:
        default:
            config.databits = 8;
            break;
    }

    config.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
    if ((options.c_cflag & PARENB) != 0)
    {
       #if JUCE_LINUX && defined (CMSPAR)
        if ((options.c_cflag & CMSPAR) != 0)
            config.parity = (options.c_cflag & PARODD) != 0 ? SerialPortConfig::SERIALPORT_PARITY_MARK : SerialPortConfig::SERIALPORT_PARITY_SPACE;
        else
       #endif
        config.parity = (options.c_cflag & PARODD) != 0 ? SerialPortConfig::SERIALPORT_PARITY_ODD : SerialPortConfig::SERIALPORT_PARITY_EVEN;
    }

    config.stopbits = (options.c_cflag & CSTOPB) != 0 ? SerialPortConfig::STOPBITS_2 : SerialPortConfig::STOPBITS_1;

    config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
    if ((options.c_iflag & (IXON | IXOFF)) != 0)
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_XONXOFF;
   #if JUCE_MAC
    else if ((options.c_cflag & (CCTS_OFLOW | CRTS_IFLOW)) != 0)
   #else
    else if ((options.c_cflag & CRTSCTS) != 0)
   #endif
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_HARDWARE;

    return true;
}

int SerialPort::readBytes (void* destBuffer, int maxBytesToRead, int timeoutMs)
{
    const auto waitResult { waitForDescriptor (native->fd, false, timeoutMs) };
    if (waitResult == WaitResult::failed)
        return -1;
    if (waitResult == WaitResult::timedOut)
        return 0;

    const auto bytesRead { ::read (native->fd, destBuffer, static_cast<size_t> (maxBytesToRead)) };
    if (bytesRead > 0)
        return static_cast<int> (bytesRead);
    // end of file, the device has been unplugged
    if (bytesRead == 0)
        return -1;
    return isTransientError (errno) ? 0 : -1;
}

int SerialPort::writeBytes (const void* sourceBuffer, int numBytesToWrite, int timeoutMs)
{
    const auto waitResult { waitForDescriptor (native->fd, true, timeoutMs) };
    if (waitResult == WaitResult::failed)
        return -1;
    if (waitResult == WaitResult::timedOut)
        return 0;

    const auto bytesWritten { ::write (native->fd, sourceBuffer, static_cast<size_t> (numBytesToWrite)) };
    if (bytesWritten >= 0)
        return static_cast<int> (bytesWritten);
    return isTransientError (errno) ? 0 : -1;
}
