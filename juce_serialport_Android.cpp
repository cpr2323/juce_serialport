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
    STATICFIELD (STOPBITS_1, "STOPBITS_1", "I") \
    STATICFIELD (STOPBITS_1_5, "STOPBITS_1_5", "I") \
    STATICFIELD (STOPBITS_2, "STOPBITS_2", "I") \
    STATICFIELD (PARITY_NONE, "PARITY_NONE", "I") \
    STATICFIELD (PARITY_ODD, "PARITY_ODD", "I") \
    STATICFIELD (PARITY_EVEN, "PARITY_EVEN", "I") \
    STATICFIELD (PARITY_MARK, "PARITY_MARK", "I") \
    STATICFIELD (PARITY_SPACE, "PARITY_SPACE", "I")
    DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialPort, "com/hoho/android/usbserial/driver/UsbSerialPort", 23)
#undef JNI_CLASS_MEMBERS

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
    DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialHelper, "com/artiphon/juce_serial/UsbSerialHelper", 23)
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
            //DBG ("******************************* portNames = " + portNames + "; numPorts = " + String(numPorts));
            //with the way that StringArray::fromTokens() works, the last token is empty, so skip it
            for (int i = 0; i < numPorts - 1; ++i)
                serialPortPaths.set(String(i), stringArray[i]);
        }
        return serialPortPaths;
    } catch (const std::exception& e) {
        DBG ("EXCEPTION IN SerialPort::getSerialPortPaths()" + String(e.what()));
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
        DebugLog ("SerialPort::open", "EXCEPTION: " + String(e.what()));

        portPath = "";
        portDescriptor = -1;
    }

    return result;
}

void SerialPort::cancel ()
{
}

bool SerialPort::setConfig(const SerialPortConfig & config)
{
    //flow control isn't supported/used by UsbSerialPort
    if (config.flowcontrol != SerialPortConfig::FLOWCONTROL_NONE)
        return false;

    auto env = getEnv();
    auto stopBits { -1 };
    switch (config.stopbits){
        case SerialPortConfig::SerialPortStopBits::STOPBITS_1:
            stopBits = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.STOPBITS_1);
            break;
        case SerialPortConfig::SerialPortStopBits::STOPBITS_1ANDHALF:
            stopBits = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.STOPBITS_1_5);
            break;
        case SerialPortConfig::SerialPortStopBits::STOPBITS_2:
            stopBits = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.STOPBITS_2);
            break;
        default:
            jassertfalse;
            return false;
    }

    auto parity { -1 };
    switch (config.parity){
        case SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_NONE:
            parity = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_NONE);
            break;
        case SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_ODD:
            parity = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_ODD);
            break;
        case SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_EVEN:
            parity = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_EVEN);
            break;
        case SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_SPACE:
            parity = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_SPACE);
            break;
        case SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_MARK:
            parity = (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_MARK);
            break;
        default:
            jassertfalse;
            return false;
    }

    return env->CallBooleanMethod(usbSerialHelper, UsbSerialHelper.setParameters, config.bps, config.databits, stopBits, parity);
}

bool SerialPort::getConfig(SerialPortConfig & config)
{
    if (! portHandle)
        return false;

    auto env = getEnv();

    //set the integer/easy values
    const auto baudRate { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.baudRate) };
    const auto dataBits { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.dataBits) };
    config.bps = baudRate;
    config.databits = dataBits;
    config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;

    //convert the enum values
    const auto stopBits { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.stopBits) };
    if (stopBits == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.STOPBITS_1))
        config.stopbits = SerialPortConfig::SerialPortStopBits::STOPBITS_1;
    else if (stopBits == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.STOPBITS_1_5))
        config.stopbits = SerialPortConfig::SerialPortStopBits::STOPBITS_1ANDHALF;
    else if (stopBits == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.STOPBITS_2))
        config.stopbits = SerialPortConfig::SerialPortStopBits::STOPBITS_2;
    else
        return false;

    const auto parity { (jint) env->GetIntField(usbSerialHelper, UsbSerialHelper.parity) };
    if (parity == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_NONE))
        config.parity = SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_NONE;
    else if (parity == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_ODD))
        config.parity = SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_ODD;
    else if (parity == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_EVEN))
        config.parity = SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_EVEN;
    else if (parity == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_SPACE))
        config.parity = SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_SPACE;
    else if (parity == (jint) env->GetStaticIntField(UsbSerialPort, UsbSerialPort.PARITY_MARK))
        config.parity = SerialPortConfig::SerialPortParity::SERIALPORT_PARITY_MARK;
    else
        return false;

    //DebugLog("********************************* SerialPort::getConfig() baudrate: " + String(baudRate) + " dataBits: " + String(dataBits) + " stopBits: " + String(stopBits) + " parity: " + String(parity));
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

                    //String msg;
                    for (int i = 0; i < bytesRead; ++i)
                    {
                        buffer[bufferedbytes++] = jbuffer[i];
                        //msg += String (std::bitset<8>(static_cast<char>(buffer[bufferedbytes])).to_string()) + " ";
                    }
                    //port->DebugLog("*************** SerialPortInputStream::run(): " + msg);
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
        port->DebugLog ("SerialPortInputStream::run", "EXCEPTION: " + String(e.what()));
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
    port->DebugLog("SerialPortOutputStream::run", "this function is called but doesn't do anything and exists immediately");
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

        //String msg;
        for (int i = 0; i < howManyBytes; ++i)
        {
            cSignedCharArray[i] = static_cast<const signed char*>(dataToWrite)[i];
            //msg += String (std::bitset<8>(cSignedCharArray[i]).to_string()) + " ";
        }
        //port->DebugLog("*************** SerialPortOutputStream::write (): " + msg);

        env->SetByteArrayRegion(jByteArray, 0, howManyBytes, cSignedCharArray);
        result = (jboolean) env->CallBooleanMethod(port->usbSerialHelper, UsbSerialHelper.write, jByteArray);
        env->DeleteLocalRef(jByteArray);
    } catch (const std::exception& e) {
        port->DebugLog ("SerialPortOutputStream::write", "EXCEPTION: " + String(e.what()));
        return false;
    }

    return result;
}
#endif // JUCE_ANDROID
