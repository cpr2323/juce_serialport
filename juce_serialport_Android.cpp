#include "../JuceLibraryCode/JuceHeader.h"

#if JUCE_ANDROID

#include <stdio.h>

#include "juce_serialport.h"

#include <juce_core/native/juce_android_JNIHelpers.h>

/** JNI Cheat-sheet
Example JNI declaration calling android core code:
#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
    DECLARE_JNI_CLASS (UsbDevice, "android/hardware/usb/UsbDevice")
#undef JNI_CLASS_MEMBERS

example JNI declaration calling juce code
#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
    STATICMETHOD (getAndroidBluetoothManager, "getAndroidBluetoothManager", "(Landroid/content/Context;)Lcom/rmsl/juce/JuceMidiSupport$BluetoothManager;")
    DECLARE_JNI_CLASS_WITH_MIN_SDK (AndroidJuceMidiSupport, "com/rmsl/juce/JuceMidiSupport", 23)
#undef JNI_CLASS_MEMBERS

JNI type conversions:
Z	                        boolean
B	                        byte
C	                        char
S	                        short
I	                        int
J	                        long
F	                        float
D	                        double
V                           void (only for return type)
Lfully-qualified-class;	fully-qualified-class
[type                     type[]

When calling JNI methods, you need to use callXXXXMethod with XXXX being the return type.
E.g., for getSerialPortPaths which returns a string, you call it with env->CallObjectMethod()

 jfieldID mapId = (*env)->GetFieldID(env,jCls,"nameOfMapVariable","Ljava/util/Map;");
*/

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
    FIELD (baudRate, "baudRate", "I") \
    FIELD (dataBits, "dataBits", "I") \
    FIELD (stopBits, "stopBits", "I") \
    FIELD (parity, "parity", "I") \
    METHOD (getSerialPortPaths, "getSerialPortPaths", "(Landroid/content/Context;)Ljava/lang/String;") \
    METHOD (connect, "connect", "(I)Z") \
    METHOD (isOpen, "isOpen", "()Z") \
    METHOD (setParameters, "setParameters", "(IIII)Z") \
    METHOD (disconnect, "disconnect", "()V") \
    METHOD (write, "write", "([B)Z") \
    METHOD (read, "read", "([B)I")
    DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialHelper, "com/hoho/android/usbserial/UsbSerialHelper", 23)
#undef JNI_CLASS_MEMBERS

StringPairArray SerialPort::getSerialPortPaths()
{
    StringPairArray serialPortPaths;
    try
    {
        auto env = getEnv();
        //objects need to be saved as GlobalRef's if they are to be passed between JNI calls.
        GlobalRef context(getAppContext());
        jmethodID constructorMethodId = env->GetMethodID(UsbSerialHelper, "<init>", "(Landroid/content/Context;Landroid/app/Activity;)V");
        jobject usbSerialHelper = env->NewObject(UsbSerialHelper, constructorMethodId, context.get(), getMainActivity().get());

        auto portNames = juce::juceString ((jstring) env->CallObjectMethod (usbSerialHelper, UsbSerialHelper.getSerialPortPaths, context.get()));
        auto stringArray = StringArray::fromTokens(portNames, "-", "");
        auto numPorts = stringArray.size();
        if (numPorts > 0)
        {
            //DBG("******************************* portNames = " + portNames + "; numPorts = " + String(numPorts));
            //with the way that StringArray::fromTokens() works, the last token is empty, so skip it
            for (int i = 0; i < numPorts - 1; ++i)
                serialPortPaths.set(String(i), stringArray[i]);
        }
        return serialPortPaths;
    } catch (const std::exception& e) {
        DBG("EXCEPTION IN SerialPort::getSerialPortPaths()" + String(e.what()));
        return serialPortPaths;
    }
}

void SerialPort::close()
{
    auto env = getEnv();
    if (! env->IsSameObject(usbSerialHelper, NULL))
        env->CallVoidMethod (usbSerialHelper, UsbSerialHelper.disconnect);
}

bool SerialPort::exists()
{
    auto env = getEnv();
    return ! env->IsSameObject(usbSerialHelper, NULL) && env->CallBooleanMethod (usbSerialHelper, UsbSerialHelper.isOpen);
}

bool SerialPort::open(const String & newPortPath)
{
    portPath = newPortPath;
    portDescriptor = newPortPath.getIntValue();

    bool result = false;
    try
    {
        auto env = getEnv();

        //construct the usbSerialHelper
        jmethodID constructorMethodId = env->GetMethodID(UsbSerialHelper, "<init>", "(Landroid/content/Context;Landroid/app/Activity;)V");
        jobject tempUsbSerialHelper = env->NewObject(UsbSerialHelper, constructorMethodId, getAppContext().get(), getMainActivity().get());
        usbSerialHelper = (jobject) env->NewGlobalRef(tempUsbSerialHelper);
        portHandle = &usbSerialHelper;

        //attempt to connect to the newPortPath
        result = (jboolean) env->CallBooleanMethod (usbSerialHelper, UsbSerialHelper.connect, newPortPath.getIntValue());
    } catch (const std::exception& e) {
        DBG("********************* EXCEPTION IN SerialPort::open()" + String(e.what()));

        portPath = "";
        portDescriptor = -1;
    }

    return result;
}

void SerialPort::cancel ()
{
    DBG("SerialPort::cancel ()");
}

bool SerialPort::setConfig(const SerialPortConfig & config)
{
    if (true)
        return true;

    auto env = getEnv();
    return env->CallBooleanMethod(usbSerialHelper, UsbSerialHelper.setParameters, config.bps, config.databits, config.stopbits, config.parity);

//    return config.bps == 115200
//        && config.databits == 8
//        && config.stopbits == SerialPortConfig::STOPBITS_1
//        && config.parity == SerialPortConfig::SERIALPORT_PARITY_NONE
//        && config.flowcontrol == SerialPortConfig::FLOWCONTROL_NONE;
}

bool SerialPort::getConfig(SerialPortConfig & config)
{
    if (! portHandle)
        return false;

    if (false)
    {
        //this is what is hardcoded in UsbSerialHelper.java
        //public void setParameters(int baudRate, int dataBits, int stopBits, int parity) throws IOException;
        //usbSerialPort.setParameters(baudRate, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE);
        auto env = getEnv();
        const auto baudRate { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.baudRate) };
        const auto dataBits { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.dataBits) };
        const auto stopBits { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.stopBits) };
        const auto parity { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.parity) };
        DBG("********************************* BAUDRATE: " + String(baudRate) + " dataBits: " + String(dataBits) + " stopBits: " + String(stopBits) + " parity: " + String(parity));

        config.bps = baudRate;
        config.databits = dataBits;
        //this will need some proper conversion
        config.stopbits = (SerialPortConfig::SerialPortStopBits) stopBits;
        config.parity = (SerialPortConfig::SerialPortParity) parity;
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
    }
    else
    {
        config.bps = 115200;
        config.databits = 8;
        config.stopbits = SerialPortConfig::STOPBITS_1;
        config.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
        config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;
    }

    return true;
}

/////////////////////////////////
// SerialPortInputStream
/////////////////////////////////
void SerialPortInputStream::run()
{
    try
    {
    while (port && port->portDescriptor != -1 && ! threadShouldExit())
    {
        auto env = getEnv();
        jbyteArray result = env->NewByteArray (8192);
        const int bytesRead = (jint) env->CallIntMethod (port->usbSerialHelper, UsbSerialHelper.read, result);
        if (bytesRead > 0)
        {
            jbyte* jbuffer = env->GetByteArrayElements (result, nullptr);
            {
                const ScopedLock lock(bufferCriticalSection);
                buffer.ensureSize(bufferedbytes + bytesRead);

                //String dbg;
                for (int i = 0; i < bytesRead; ++i)
                {
                    buffer[bufferedbytes++] = jbuffer[i];
                    //dbg += String (std::bitset<8>(static_cast<char>(buffer[bufferedbytes])).to_string()) + " ";
                }
                //DBG("*************** SerialPortInputStream::run(): " + dbg);
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
    } catch (const std::exception& e) {
        DBG("********************* EXCEPTION IN SerialPortInputStream::run()" + String(e.what()));
    }
}

void SerialPortInputStream::cancel ()
{
}

int SerialPortInputStream::read(void *destBuffer, int maxBytesToRead)
{
    if (! port || port->portHandle == 0)
        return -1;

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
        jbyteArray jByteArray = env->NewByteArray(howManyBytes);
        signed char cSignedCharArray[howManyBytes];

        //String dbg;
        for (int i = 0; i < howManyBytes; ++i)
        {
            cSignedCharArray[i] = static_cast<const signed char*>(dataToWrite)[i];
            //dbg += String (std::bitset<8>(cSignedCharArray[i]).to_string()) + " ";
        }
        //DBG("*************** SerialPortOutputStream::write (): " + dbg);

        env->SetByteArrayRegion(jByteArray, 0, howManyBytes, cSignedCharArray);
        result = (jboolean) env->CallBooleanMethod(port->usbSerialHelper, UsbSerialHelper.write, jByteArray);
        env->DeleteLocalRef(jByteArray);
    } catch (const std::exception& e) {
        DBG("********************* EXCEPTION IN SerialPortOutputStream::write()" + String(e.what()));
        return false;
    }

    return result;
}
#endif // JUCE_ANDROID
