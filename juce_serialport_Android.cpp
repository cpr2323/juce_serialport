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

#define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
    METHOD (refresh, "refresh", "(Landroid/content/Context;)Ljava/lang/String;")
DECLARE_JNI_CLASS_WITH_MIN_SDK (UsbSerialHelper, "com/hoho/android/usbserial/UsbSerialHelper", 23)
#undef JNI_CLASS_MEMBERS

StringPairArray SerialPort::getSerialPortPaths()
{
    StringPairArray SerialPortPaths;

    try
    {
        auto env = getEnv();
        jobject usbSerialHelper = env->AllocObject(UsbSerialHelper);

        auto found = juce::juceString ((jstring) env->CallObjectMethod (usbSerialHelper, UsbSerialHelper.refresh, getAppContext().get()));
        DBG(found);

    } catch (const std::exception& e) { // reference to the base of a polymorphic object
        DBG(e.what());
    }

    return SerialPortPaths;
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
