#include "../JuceLibraryCode/JuceHeader.h"

#if JUCE_ANDROID

#include <stdio.h>

#include "juce_serialport.h"

#include <juce_core/native/juce_android_JNIHelpers.h>

////JNI to call android core stuff
//#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
//    DECLARE_JNI_CLASS (UsbDevice, "android/hardware/usb/UsbDevice")
//#undef JNI_CLASS_MEMBERS
//
////JNI to call juce stuff
//#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
//    STATICMETHOD (getAndroidBluetoothManager, "getAndroidBluetoothManager", "(Landroid/content/Context;)Lcom/rmsl/juce/JuceMidiSupport$BluetoothManager;")
//    DECLARE_JNI_CLASS_WITH_MIN_SDK (AndroidJuceMidiSupport, "com/rmsl/juce/JuceMidiSupport", 23)
//#undef JNI_CLASS_MEMBERS

//#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
////    METHOD (refresh, "refresh", "(Landroid/content/Context;)[Ljava/lang/String;")
//      METHOD (refresh, "refresh", "(Landroid/content/Context;)Ljava/lang/String;")
//    DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialHelper, "com/hoho/android/usbserial/UsbSerialHelper", 23)
//#undef JNI_CLASS_MEMBERS

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
    METHOD (getSerialPortPaths, "getSerialPortPaths", "(Landroid/content/Context;)Ljava/lang/String;")
DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialHelper, "com/hoho/android/usbserial/UsbSerialHelper", 23)
#undef JNI_CLASS_MEMBERS

StringPairArray SerialPort::getSerialPortPaths()
{
    StringPairArray serialPortPaths;

    try
    {
        auto env = getEnv();
        jmethodID constructorMethodId = env->GetMethodID(UsbSerialHelper, "<init>", "(Landroid/content/Context;)V");
        if (constructorMethodId == nullptr) {
            DBG("******************************* NULL METHOD ID");
            return serialPortPaths;
        }

        //objects need to be saved as GlobalRef's if they are to be passed between JNI calls.
        //not sure what the impact of this has overall though -- is it like creating a ton of different global objects?
        GlobalRef context(getAppContext());
        jobject usbSerialHelper = env->NewObject(UsbSerialHelper, constructorMethodId, context.get());

        auto portNames = juce::juceString ((jstring) env->CallObjectMethod (usbSerialHelper, UsbSerialHelper.getSerialPortPaths, context.get()));

        auto stringArray = StringArray::fromTokens(portNames, "-", "");
        auto numPorts = stringArray.size();
        if (numPorts > 0)
        {
            DBG("******************************* portNames = " + portNames + "; numPorts = " + String(numPorts));
            //unclear why, but there's an empty token at the end of the stringArray, so skipping that one
            for (int i = 0; i < numPorts - 1; ++i)
                serialPortPaths.set(String(i++), stringArray[i]);
        }
        return serialPortPaths;
    } catch (const std::exception& e) {
        DBG("EXCEPTION IN SerialPort::getSerialPortPaths()" + String(e.what()));
    }

    return serialPortPaths;
}

void SerialPort::close()
{

}

bool SerialPort::exists()
{
    return false;
}

bool SerialPort::open(const String & newPortPath)
{
    DBG("************************ SerialPort::open()");
    portPath = newPortPath;
    return false;
}

void SerialPort::cancel ()
{

}
bool SerialPort::setConfig(const SerialPortConfig & config)
{
    return false;
}

bool SerialPort::getConfig(SerialPortConfig & config)
{
    return false;
}

/////////////////////////////////
// SerialPortInputStream
/////////////////////////////////
void SerialPortInputStream::run()
{
}

void SerialPortInputStream::cancel ()
{
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

}

void SerialPortOutputStream::cancel ()
{
    if (! port || port->portHandle == 0)
        return;

    port->cancel ();
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

#endif // JUCE_ANDROID
