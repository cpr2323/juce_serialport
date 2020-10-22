#include "../JuceLibraryCode/JuceHeader.h"

#if JUCE_ANDROID

#include <stdio.h>

#include "juce_serialport.h"

#include <juce_core/native/juce_android_JNIHelpers.h>

////JNI to call android core stuff
//#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
//    DECLARE_JNI_CLASS (UsbDevice, "android/hardware/usb/UsbDevice")
//#undef JNI_CLASS_MEMBERS

////JNI to call juce stuff
//#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
//    STATICMETHOD (getAndroidBluetoothManager, "getAndroidBluetoothManager", "(Landroid/content/Context;)Lcom/rmsl/juce/JuceMidiSupport$BluetoothManager;")
//    DECLARE_JNI_CLASS_WITH_MIN_SDK (AndroidJuceMidiSupport, "com/rmsl/juce/JuceMidiSupport", 23)
//#undef JNI_CLASS_MEMBERS

////JNI type conversions
//Z	                        boolean
//B	                        byte
//C	                        char
//S	                        short
//I	                        int
//J	                        long
//F	                        float
//D	                        double
//Lfully-qualified-class;	fully-qualified-class
//[type                   type[]

//when calling those methods, you have to use callXXXXMethod with XXXX being the return type
//e.g., for getSerialPortPaths which returns a string, you call it with CallObjectMethod

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
    METHOD (getSerialPortPaths, "getSerialPortPaths", "(Landroid/content/Context;)Ljava/lang/String;") \
    METHOD (send, "send", "([B)Z") \
    METHOD (read, "read", "([B)I") \
    METHOD (connect, "connect", "(I)Z")
DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialHelper, "com/hoho/android/usbserial/UsbSerialHelper", 23)
#undef JNI_CLASS_MEMBERS

StringPairArray SerialPort::getSerialPortPaths()
{
    StringPairArray serialPortPaths;
    try
    {
        auto env = getEnv();
        jmethodID constructorMethodId = env->GetMethodID(UsbSerialHelper, "<init>", "(Landroid/content/Context;Landroid/app/Activity;)V");

        //objects need to be saved as GlobalRef's if they are to be passed between JNI calls.
        //not sure what the impact of this has overall though -- is it like creating a ton of different global objects?
        GlobalRef context(getAppContext());
        GlobalRef mainActivity(getMainActivity());
        jobject usbSerialHelper = env->NewObject(UsbSerialHelper, constructorMethodId, context.get(), mainActivity.get());

        auto portNames = juce::juceString ((jstring) env->CallObjectMethod (usbSerialHelper, UsbSerialHelper.getSerialPortPaths, context.get()));

        auto stringArray = StringArray::fromTokens(portNames, "-", "");
        auto numPorts = stringArray.size();
        if (numPorts > 0)
        {
            DBG("******************************* portNames = " + portNames + "; numPorts = " + String(numPorts));
            //unclear why, but there's an empty token at the end of the stringArray, so skipping that one
            for (int i = 0; i < numPorts - 1; ++i)
                serialPortPaths.set(String(i), stringArray[i]);
        }
        return serialPortPaths;
    } catch (const std::exception& e) {
        DBG("EXCEPTION IN SerialPort::getSerialPortPaths()" + String(e.what()));
    }

    return serialPortPaths;
}

void SerialPort::close()
{
    DBG("SerialPort::close ()");
}

bool SerialPort::exists()
{
    return (portDescriptor != -1);
}

bool SerialPort::open(const String & newPortPath)
{
    DBG("************************ SerialPort::open()");
    portPath = newPortPath;
    portDescriptor = newPortPath.getIntValue();

    bool result = false;
    try
    {
        GlobalRef context(getAppContext());
        auto env = getEnv();
        GlobalRef mainActivity(getMainActivity());

        jmethodID constructorMethodId = env->GetMethodID(UsbSerialHelper, "<init>", "(Landroid/content/Context;Landroid/app/Activity;)V");
        jobject temp = env->NewObject(UsbSerialHelper, constructorMethodId, context.get(), mainActivity.get());
        usbSerialHelper = (jobject) env->NewGlobalRef(temp);

        portHandle = &usbSerialHelper;
        result = (jboolean) env->CallBooleanMethod (usbSerialHelper, UsbSerialHelper.connect, newPortPath.getIntValue());
    } catch (const std::exception& e) {
        DBG("********************* EXCEPTION IN SerialPort::open()" + String(e.what()));
    }

    if (result)
        DBG("********************* SerialPort::open() succeded");
    else
        DBG("********************* SerialPort::open() FAILED");
    return result;
}

void SerialPort::cancel ()
{
    DBG("SerialPort::cancel ()");
}

bool SerialPort::setConfig(const SerialPortConfig & config)
{
    return config.bps == 115200
        && config.databits == 8
        && config.stopbits == SerialPortConfig::STOPBITS_1
        && config.parity == SerialPortConfig::SERIALPORT_PARITY_NONE
        && config.flowcontrol == SerialPortConfig::FLOWCONTROL_NONE;
}

bool SerialPort::getConfig(SerialPortConfig & config)
{
    if (! portHandle)
        return false;

    //this is what is hardcoded in UsbSerialHelper.java
    //public void setParameters(int baudRate, int dataBits, int stopBits, int parity) throws IOException;
    //usbSerialPort.setParameters(baudRate, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE);

    config.bps = 115200;
    config.databits = 8;
    config.stopbits = SerialPortConfig::STOPBITS_1;
    config.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
    config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;

    return true;
}

/////////////////////////////////
// SerialPortInputStream
/////////////////////////////////
void SerialPortInputStream::run()
{
    while (port && port->portDescriptor != -1 && ! threadShouldExit())
    {
        auto env = getEnv();
        jbyteArray result = env->NewByteArray (8192);
        const int bytesRead = (jint)  env->CallIntMethod (port->usbSerialHelper, UsbSerialHelper.read, result);
        if (bytesRead > 0)
        {
            jbyte* jbuffer = env->GetByteArrayElements (result, nullptr);
            {
                const ScopedLock lock(bufferCriticalSection);
                buffer.ensureSize(bufferedbytes + bytesRead);
                for (int i = 0; i < bytesRead; ++i)
                    buffer[bufferedbytes++] = jbuffer[i];
            }
            env->ReleaseByteArrayElements(result, jbuffer, 0);

            if (notify == NOTIFY_ALWAYS)
                sendChangeMessage();
        }
        else if (bytesRead == -1)
        {
            port->close ();
            break;
        }

        env->DeleteLocalRef(result);
    }
}

void SerialPortInputStream::cancel ()
{
    DBG("SerialPortInputStream::cancel()");
}

int SerialPortInputStream::read(void *destBuffer, int maxBytesToRead)
{
    if (! port || port->portHandle == 0)
        return -1;

    //taken from OSX code
    const ScopedLock l (bufferCriticalSection);

    if (maxBytesToRead > bufferedbytes)
        maxBytesToRead = bufferedbytes;

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
    //TODO if this is not used, can we stop it from running?
    DBG("SerialPortOutputStream::run()");
}

void SerialPortOutputStream::cancel ()
{
    DBG("SerialPortOutputStream::cancel()");

    if (! port || port->portHandle == 0)
        return;

    port->cancel ();
}

bool SerialPortOutputStream::write(const void *dataToWrite, size_t howManyBytes)
{
    auto result = false;
    if (! port || port->portHandle == 0)
        return result;

    try {
        auto env = getEnv();
        jbyteArray data = env->NewByteArray (howManyBytes);
        jbyte* jdata = env->GetByteArrayElements (data, nullptr);

        for (int i = 0; i < howManyBytes; ++i)
            jdata[i] = *static_cast<const unsigned char*>(dataToWrite);

        result = (jboolean)  env->CallBooleanMethod (port->usbSerialHelper, UsbSerialHelper.send, data);
        env->DeleteLocalRef (data);
    } catch (const std::exception& e) {
        DBG("********************* EXCEPTION IN SerialPortOutputStream::write()" + String(e.what()));
        return false;
    }

    return result;
}
#endif // JUCE_ANDROID
