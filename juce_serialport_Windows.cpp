//win32_SerialPort.cpp
//Serial Port classes in a Juce stylee, by graffiti
//see SerialPort.h for details
//
// Updated for current Juce API 8/1/13 Marc Lindahl
//

#include "../JuceLibraryCode/JuceHeader.h"

#if JUCE_WINDOWS

#include <windows.h>
#include <stdio.h>

#include "juce_serialport.h"

class CAutoHeapAlloc
{
public:
    //Constructors / Destructors
    CAutoHeapAlloc(HANDLE hHeap = GetProcessHeap(), DWORD dwHeapFreeFlags = 0) : m_pData(NULL),
        m_hHeap(hHeap),
        m_dwHeapFreeFlags(dwHeapFreeFlags)
    {
    }

    BOOL Allocate(SIZE_T dwBytes, DWORD dwFlags = 0)
    {
        //Validate our parameters
        jassert(m_pData == NULL);

        m_pData = HeapAlloc(m_hHeap, dwFlags, dwBytes);
        return (m_pData != NULL);
    }

    ~CAutoHeapAlloc()
    {
        if (m_pData != NULL)
        {
            HeapFree(m_hHeap, m_dwHeapFreeFlags, m_pData);
            m_pData = NULL;
        }
    }

    //Methods

    //Member variables
    LPVOID m_pData;
    HANDLE m_hHeap;
    DWORD  m_dwHeapFreeFlags;
};

StringPairArray SerialPort::getSerialPortPaths()
{
    StringPairArray SerialPortPaths;

    HKEY hSERIALCOMM;
    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_QUERY_VALUE, &hSERIALCOMM) == ERROR_SUCCESS)
    {
        //Get the max value name and max value lengths
        DWORD dwMaxValueNameLen;
        DWORD dwMaxValueLen;
        DWORD dwQueryInfo = RegQueryInfoKey(hSERIALCOMM, NULL, NULL, NULL, NULL, NULL, NULL, NULL, &dwMaxValueNameLen, &dwMaxValueLen, NULL, NULL);
        if (dwQueryInfo == ERROR_SUCCESS)
        {
            DWORD dwMaxValueNameSizeInChars = dwMaxValueNameLen + 1; //Include space for the NULL terminator
            DWORD dwMaxValueNameSizeInBytes = dwMaxValueNameSizeInChars * sizeof(TCHAR);
            DWORD dwMaxValueDataSizeInChars = dwMaxValueLen / sizeof(TCHAR) + 1; //Include space for the NULL terminator
            DWORD dwMaxValueDataSizeInBytes = dwMaxValueDataSizeInChars * sizeof(TCHAR);

            //Allocate some space for the value name and value data			
            CAutoHeapAlloc valueName;
            CAutoHeapAlloc valueData;
            if (valueName.Allocate(dwMaxValueNameSizeInBytes) && valueData.Allocate(dwMaxValueDataSizeInBytes))
            {
                //Enumerate all the values underneath HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM
                DWORD dwIndex = 0;
                DWORD dwType;
                DWORD dwValueNameSize = dwMaxValueNameSizeInChars;
                DWORD dwDataSize = dwMaxValueDataSizeInBytes;
                std::memset(valueName.m_pData, 0, dwMaxValueNameSizeInBytes);
                std::memset(valueData.m_pData, 0, dwMaxValueDataSizeInBytes);
                TCHAR* szValueName = static_cast<TCHAR*>(valueName.m_pData);
                BYTE* byValue = static_cast<BYTE*>(valueData.m_pData);
                LONG nEnum = RegEnumValue(hSERIALCOMM, dwIndex, szValueName, &dwValueNameSize, NULL, &dwType, byValue, &dwDataSize);
                while (nEnum == ERROR_SUCCESS)
                {
                    //If the value is of the correct type, then add it to the array
                    if (dwType == REG_SZ)
                    {
                        TCHAR* szPort = reinterpret_cast<TCHAR*>(byValue);
                        SerialPortPaths.set(szPort, String("\\\\.\\") + String(szPort));
                    }

                    //Prepare for the next time around
                    dwValueNameSize = dwMaxValueNameSizeInChars;
                    dwDataSize = dwMaxValueDataSizeInBytes;
                    std::memset(valueName.m_pData, 0, dwMaxValueNameSizeInBytes);
                    std::memset(valueData.m_pData, 0, dwMaxValueDataSizeInBytes);
                    ++dwIndex;
                    nEnum = RegEnumValue(hSERIALCOMM, dwIndex, szValueName, &dwValueNameSize, NULL, &dwType, byValue, &dwDataSize);
                }
            }
            else
                SetLastError(ERROR_OUTOFMEMORY);
        }

        //Close the registry key now that we are finished with it    
        RegCloseKey(hSERIALCOMM);

        if (dwQueryInfo != ERROR_SUCCESS)
            SetLastError(dwQueryInfo);
    }

    return SerialPortPaths;
}

void SerialPort::close()
{
    if (portHandle)
    {
        CloseHandle(portHandle);
        portHandle = 0;
    }
}
bool SerialPort::exists()
{
    return portHandle ? true : false;
}
bool SerialPort::open(const String & newPortPath)
{
    portPath = newPortPath;
    portHandle = CreateFile((const char*)portPath.toUTF8(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (portHandle == INVALID_HANDLE_VALUE)
    {
        //DBG_PRINTF((T("(SerialPort::open) CreateFile failed with error %d.\n"), GetLastError()));
        portHandle = 0;
        return false;
    }
    COMMTIMEOUTS commTimeout;
    if (GetCommTimeouts(portHandle, &commTimeout))
    {
        commTimeout.ReadIntervalTimeout = MAXDWORD;
        commTimeout.ReadTotalTimeoutConstant = 0;
        commTimeout.ReadTotalTimeoutMultiplier = 0;
        commTimeout.WriteTotalTimeoutConstant = 0;
        commTimeout.WriteTotalTimeoutMultiplier = 0;
    }
    else
        DBG("GetCommTimeouts error");
    if (!SetCommTimeouts(portHandle, &commTimeout))
        DBG("SetCommTimeouts error");

    if (!SetCommMask(portHandle, EV_RXCHAR))
        DBG("SetCommMask error");

    return true;
}
bool SerialPort::setConfig(const SerialPortConfig & config)
{
    if (!portHandle)return false;
    DCB dcb;
    memset(&dcb, 0, sizeof(DCB));
    dcb.DCBlength = sizeof(DCB);
    dcb.fBinary = 1;
    dcb.XonLim = 2048;
    dcb.XoffLim = 512;
    dcb.BaudRate = config.bps;
    dcb.ByteSize = (BYTE)config.databits;
    dcb.fParity = true;
    switch (config.parity)
    {
    case SerialPortConfig::SERIALPORT_PARITY_ODD:
        dcb.Parity = ODDPARITY;
        break;
    case SerialPortConfig::SERIALPORT_PARITY_EVEN:
        dcb.Parity = EVENPARITY;
        break;
    case SerialPortConfig::SERIALPORT_PARITY_MARK:
        dcb.Parity = MARKPARITY;
        break;
    case SerialPortConfig::SERIALPORT_PARITY_SPACE:
        dcb.Parity = SPACEPARITY;
        break;
    case SerialPortConfig::SERIALPORT_PARITY_NONE:
    default:
        dcb.Parity = NOPARITY;
        dcb.fParity = false;
        break;
    }
    switch (config.stopbits)
    {
    case SerialPortConfig::STOPBITS_1:
    default:
        dcb.StopBits = ONESTOPBIT;
        break;
    case SerialPortConfig::STOPBITS_1ANDHALF:
        dcb.StopBits = ONE5STOPBITS;
        break;
    case SerialPortConfig::STOPBITS_2:
        dcb.StopBits = TWOSTOPBITS;
        break;
    }
    switch (config.flowcontrol)
    {
    case SerialPortConfig::FLOWCONTROL_XONXOFF:
        dcb.fOutxCtsFlow = 0;
        dcb.fOutxDsrFlow = 0;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fOutX = 1;
        dcb.fInX = 1;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        break;
    case SerialPortConfig::FLOWCONTROL_HARDWARE:
        dcb.fOutxCtsFlow = 1;
        dcb.fOutxDsrFlow = 1;
        dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;
        dcb.fOutX = 0;
        dcb.fInX = 0;
        dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
        break;
    case SerialPortConfig::FLOWCONTROL_NONE:
    default:
        dcb.fOutxCtsFlow = 0;
        dcb.fOutxDsrFlow = 0;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fOutX = 0;
        dcb.fInX = 0;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        break;
    }
    return (SetCommState(portHandle, &dcb) ? true : false);
}
bool SerialPort::getConfig(SerialPortConfig & config)
{
    if (!portHandle)return false;
    DCB dcb;
    if (!GetCommState(portHandle, &dcb))
        return false;
    config.bps = dcb.BaudRate;
    config.databits = dcb.ByteSize;
    switch (dcb.Parity)
    {
    case ODDPARITY:
        config.parity = SerialPortConfig::SERIALPORT_PARITY_ODD;
        break;
    case EVENPARITY:
        config.parity = SerialPortConfig::SERIALPORT_PARITY_EVEN;
        break;
    case MARKPARITY:
        config.parity = SerialPortConfig::SERIALPORT_PARITY_MARK;
        break;
    case SPACEPARITY:
        config.parity = SerialPortConfig::SERIALPORT_PARITY_SPACE;
        break;
    case NOPARITY:
    default:
        config.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
        break;
    }
    switch (dcb.StopBits)
    {
    case ONESTOPBIT:
    default:
        config.stopbits = SerialPortConfig::STOPBITS_1;
        break;
    case ONE5STOPBITS:
        config.stopbits = SerialPortConfig::STOPBITS_1ANDHALF;
        break;
    case TWOSTOPBITS:
        config.stopbits = SerialPortConfig::STOPBITS_2;
        break;
    }
    if (dcb.fOutX && dcb.fInX)
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_XONXOFF;
    else if ((dcb.fDtrControl == DTR_CONTROL_HANDSHAKE) && (dcb.fRtsControl == RTS_CONTROL_HANDSHAKE))
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_HARDWARE;
    else
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
    return true;
}
/////////////////////////////////
// SerialPortInputStream
/////////////////////////////////
void SerialPortInputStream::run()
{
    DWORD dwEventMask = 0;
    //overlapped structure for the wait
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(0, true, 0, 0);
    //overlapped structure for the read
    OVERLAPPED ovRead;
    memset(&ovRead, 0, sizeof(ovRead));
    ovRead.hEvent = CreateEvent(0, true, 0, 0);
    while (port && port->portHandle && !threadShouldExit())
    {
        unsigned char c;
        DWORD bytesread = 0;
        const auto wceReturn = WaitCommEvent(port->portHandle, &dwEventMask, &ov);
        if (wceReturn == 0 && GetLastError () != ERROR_IO_PENDING)
        {
            port->close ();
            break;
        }

        if (WAIT_OBJECT_0 == WaitForSingleObject(ov.hEvent, 100))
        {
            DWORD dwMask;
            if (GetCommMask(port->portHandle, &dwMask))
            {
                if (dwMask & EV_RXCHAR)
                {
                    do
                    {
                        ResetEvent(ovRead.hEvent);
                        ReadFile(port->portHandle, &c, 1, &bytesread, &ovRead);
                        if (bytesread == 1)
                        {
                            const ScopedLock l(bufferCriticalSection);
                            buffer.ensureSize(bufferedbytes + 1);
                            buffer[bufferedbytes] = c;
                            bufferedbytes++;
                            if (notify == NOTIFY_ALWAYS || ((notify == NOTIFY_ON_CHAR) && (c == notifyChar)))
                                sendChangeMessage();
                        }
                    } while (bytesread);
                }
            }
            ResetEvent(ov.hEvent);
        }
    }
    CloseHandle(ovRead.hEvent);
    CloseHandle(ov.hEvent);
}
int SerialPortInputStream::read(void *destBuffer, int maxBytesToRead)
{
    if (!port || port->portHandle == 0)
        return -1;

    const ScopedLock l (bufferCriticalSection);
    if (maxBytesToRead > bufferedbytes)maxBytesToRead = bufferedbytes;
    memcpy (destBuffer, buffer.getData (), maxBytesToRead);
    buffer.removeSection (0, maxBytesToRead);
    bufferedbytes -= maxBytesToRead;
    return maxBytesToRead;
}
/////////////////////////////////
// SerialPortOutputStream
/////////////////////////////////
void SerialPortOutputStream::run()
{
    unsigned char tempbuffer[writeBufferSize];
    OVERLAPPED ov;
    memset(&ov, 0, sizeof(ov));
    ov.hEvent = CreateEvent(0, true, 0, 0);
    while (port && port->portHandle && !threadShouldExit())
    {
        if (!bufferedbytes)
            triggerWrite.wait(100);
        if (bufferedbytes)
        {
            DWORD byteswritten = 0;
            bufferCriticalSection.enter();
            DWORD bytestowrite = bufferedbytes > writeBufferSize ? writeBufferSize : bufferedbytes;
            memcpy(tempbuffer, buffer.getData(), bytestowrite);
            bufferCriticalSection.exit();
            ResetEvent(ov.hEvent);
            int iRet = WriteFile(port->portHandle, tempbuffer, bytestowrite, &byteswritten, &ov);
            if (iRet == 0)
            {
                WaitForSingleObject(ov.hEvent, INFINITE);
            }
            GetOverlappedResult(port->portHandle, &ov, &byteswritten, TRUE);
            if (byteswritten)
            {
                const ScopedLock l(bufferCriticalSection);
                buffer.removeSection(0, byteswritten);
                bufferedbytes -= byteswritten;
            }
        }
    }
    CloseHandle(ov.hEvent);
}
bool SerialPortOutputStream::write(const void *dataToWrite, size_t howManyBytes)
{
    if (! port || port->portHandle == 0)
        return false;

    bufferCriticalSection.enter();
    buffer.append(dataToWrite, howManyBytes);
    bufferedbytes += (int)howManyBytes;
    bufferCriticalSection.exit();
    triggerWrite.signal();
    return true;
}

#endif // JUCE_WIN
