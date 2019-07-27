/***********************************************************************************
 BEGIN_JUCE_MODULE_DECLARATION

  ID:               juce_serialport
  vendor:           om ohm productions
  version:          2.1.4
  name:             JUCE Serial Port classes
  description:      Access hardware serial ports or USB virtual serial ports.
  website:          
  license:          

  dependencies:     juce_core
  OSXFrameworks:
  iOSFrameworks:
  linuxLibs:
  mingwLibs:

 END_JUCE_MODULE_DECLARATION
***********************************************************************************/
/*Serial port class for accessing serial ports in an asyncronous buffered manner

contributed by graffiti
 
Updated for current Juce API 8/1/12 Marc Lindahl



a typical serialport scenario may be:
{
	//get a list of serial ports installed on the system, as a StringPairArray containing a friendly name and the port path
	StringPairArray portlist = SerialPort::getSerialPortPaths();
	if(portlist.size())
	{
		//open the first port on the system
		SerialPort * pSP = new SerialPort(portlist.getAllValues()[0], SerialPortConfig(9600, 8, SerialPortConfig::SERIALPORT_PARITY_NONE, SerialPortConfig::STOPBITS_1, SerialPortConfig::FLOWCONTROL_NONE));
		if(pSP->exists())
		{
			//create streams for reading/writing
			SerialPortOutputStream * pOutputStream = new SerialPortOutputStream(pSP);
			SerialPortInputStream * pInputStream = new SerialPortInputStream(pSP);

			pOutputStream->write("hello world via serial", 22); //write some bytes

			//read chars one at a time:
			char c;
			while(!pInputStream->isExhausted())
				pInputStream->read(&c, 1);

			//or, read line by line:
			String s;
			while(pInputStream->canReadLine())
				s = pInputStream->readNextLine();

			//or ask to be notified when a new line is available:
			pInputStreams->addChangeListener(this); //we must be a ChangeListener to receive notifications
			pInputStream->setNotify(SerialPortInputStream::NOTIFY_ON_CHAR, '\n');

			//or ask to be notified whenever any character is received
			//NOTE - use with care at high baud rates!!!!
			pInputStream->setNotify(SerialPortInputStream::NOTIFY_ALWAYS);

			//please see class definitions for other features/functions etc		
		}
	}
}
*/


#ifndef _SERIALPORT_H_
#define _SERIALPORT_H_

#include <stdint.h>


class JUCE_API SerialPortConfig
{
public:
	SerialPortConfig(){};
	~SerialPortConfig(){};
	enum SerialPortStopBits{STOPBITS_1, STOPBITS_1ANDHALF, STOPBITS_2};
	enum SerialPortFlowControl{FLOWCONTROL_NONE, FLOWCONTROL_HARDWARE, FLOWCONTROL_XONXOFF};
	enum SerialPortParity{SERIALPORT_PARITY_NONE, SERIALPORT_PARITY_ODD, SERIALPORT_PARITY_EVEN, SERIALPORT_PARITY_SPACE, SERIALPORT_PARITY_MARK};
	SerialPortConfig(uint32_t bps, uint32_t databits, SerialPortParity parity, SerialPortStopBits stopbits, SerialPortFlowControl flowcontrol)
		:bps(bps), databits(databits), parity(parity), stopbits(stopbits), flowcontrol(flowcontrol)
	{}
	uint32_t bps;
	uint32_t databits;
	SerialPortParity parity;
	SerialPortStopBits stopbits;
	SerialPortFlowControl flowcontrol;
};

class JUCE_API SerialPort
{
public:
	SerialPort()
	{
		portHandle = 0;
		portDescriptor = -1;
	}
	SerialPort(const juce::String & portPath)
	{
		SerialPort();
		open(portPath);
	}
	SerialPort(const juce::String & portPath, const SerialPortConfig & config)
	{
		SerialPort();
		open(portPath);
		setConfig(config);
	}
	~SerialPort()
	{
		close();
	}
	bool open(const juce::String & portPath);
	void close();
	bool setConfig(const SerialPortConfig & config);
	bool getConfig(SerialPortConfig & config);
	juce::String getPortPath(){return portPath;}
	static juce::StringPairArray getSerialPortPaths();
	bool exists();
	juce_UseDebuggingNewOperator
private:
	friend class SerialPortInputStream;
	friend class SerialPortOutputStream;
	void * portHandle;
	int portDescriptor;
	juce::String portPath;
};

class JUCE_API SerialPortInputStream : public juce::InputStream, public juce::ChangeBroadcaster, private juce::Thread
{
public:
    SerialPortInputStream(SerialPort * port)
    :Thread("SerialInThread"), port(port),bufferedbytes(0), notify(NOTIFY_OFF), notifyChar(0)
	{
		startThread();
	}
	virtual ~SerialPortInputStream()
	{
		signalThreadShouldExit();
		waitForThreadToExit(500);
	}
	enum notifyflag{NOTIFY_OFF=0, NOTIFY_ON_CHAR, NOTIFY_ALWAYS};
	void setNotify(notifyflag _notify=NOTIFY_ON_CHAR, char c=0)
	{
		notifyChar = c;
		this->notify = _notify;
	}
	bool canReadString()
	{
		const juce::ScopedLock l(bufferCriticalSection);
		int iNdx=0;
		while(iNdx<bufferedbytes)
			if(buffer[iNdx++]==0)return true;
		return false;
	}
	bool canReadLine()
	{
		const juce::ScopedLock l(bufferCriticalSection);
		int iNdx=0;
		while(iNdx<bufferedbytes)
			if( buffer[iNdx++]=='\n' /*|| (buffer[iNdx++]=='\r')*/)
				return true;
		return false;
	}
	virtual void run();
	virtual int read(void *destBuffer, int maxBytesToRead);
	virtual juce::String readNextLine() //have to override this, because InputStream::readNextLine isn't compatible with SerialPorts (uses setPos)
	{
		juce::String s;
		char c;
		s.preallocateBytes(32);
		while(read(&c, 1) && (c!='\n'))
			s.append(juce::String::charToString(c), 1);
		s = s.trim();
		return s;
	}
	virtual juce::int64 getTotalLength()
	{
		const juce::ScopedLock l(bufferCriticalSection);
		return bufferedbytes;
	};
	virtual bool isExhausted()
	{
		const juce::ScopedLock l(bufferCriticalSection);
		return bufferedbytes?false:true;
	};
	virtual juce::int64 getPosition(){return -1;}
	virtual bool setPosition(juce::int64 /*newPosition*/){return false;}
    SerialPort* getPort() { return port; }
private:
	SerialPort * port;
	int bufferedbytes;
	juce::MemoryBlock buffer;
	juce::CriticalSection bufferCriticalSection;
	notifyflag notify;
	char notifyChar;
};

class JUCE_API SerialPortOutputStream : public juce::OutputStream, private juce::Thread
{
public:
    SerialPortOutputStream(SerialPort * port)
    :Thread("SerialOutThread"), port(port),bufferedbytes(0)
	{
		startThread();
	}
	virtual ~SerialPortOutputStream()
	{
		signalThreadShouldExit();
		waitForThreadToExit(500);
	}
	virtual void run();
	virtual void flush(){}
	virtual bool setPosition(juce::int64 /*newPosition*/){return false;}
	virtual juce::int64 getPosition(){return -1;}
	virtual bool write(const void *dataToWrite, size_t howManyBytes);
    SerialPort* getPort() { return port; }
private:
	SerialPort * port;
	int bufferedbytes;
	juce::MemoryBlock buffer;
	juce::CriticalSection bufferCriticalSection;
	juce::WaitableEvent triggerWrite;
	static const uint32_t writeBufferSize=128;
};
#endif //_SERIALPORT_H_
