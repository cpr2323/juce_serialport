// Android implementation, included by juce_serialport.cpp
//
// this talks to USB serial devices through com/artiphon/juce_serial/UsbSerialHelper.java, which uses the
// usb-serial-for-android library. both have to be added to the app's Java sources, see README.md

/** JNI Cheat-sheet
JNI type conversions:
Z                           boolean
B                           byte
C                           char
S                           short
I                           int
J                           long
F                           float
D                           double
V                           void (only for return type)
Lfully-qualified-class;     fully-qualified-class
[type                       type[]

When calling JNI methods, you need to use callXXXXMethod with XXXX being the return type.
E.g., for getSerialPortPaths which returns an array, you call it with env->CallStaticObjectMethod()
*/

namespace juce::serialport_jni
{
    #define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
        STATICFIELD (STOPBITS_1, "STOPBITS_1", "I") \
        STATICFIELD (STOPBITS_1_5, "STOPBITS_1_5", "I") \
        STATICFIELD (STOPBITS_2, "STOPBITS_2", "I") \
        STATICFIELD (PARITY_NONE, "PARITY_NONE", "I") \
        STATICFIELD (PARITY_ODD, "PARITY_ODD", "I") \
        STATICFIELD (PARITY_EVEN, "PARITY_EVEN", "I") \
        STATICFIELD (PARITY_MARK, "PARITY_MARK", "I") \
        STATICFIELD (PARITY_SPACE, "PARITY_SPACE", "I")
    DECLARE_JNI_CLASS (UsbSerialPort, "com/hoho/android/usbserial/driver/UsbSerialPort")
    #undef JNI_CLASS_MEMBERS

    #define JNI_CLASS_MEMBERS(METHOD, STATICMETHOD, FIELD, STATICFIELD, CALLBACK) \
        METHOD (constructor, "<init>", "(Landroid/content/Context;Landroid/app/Activity;)V") \
        FIELD (baudRate, "baudRate", "I") \
        FIELD (dataBits, "dataBits", "I") \
        FIELD (stopBits, "stopBits", "I") \
        FIELD (parity, "parity", "I") \
        STATICMETHOD (getSerialPortPaths, "getSerialPortPaths", "(Landroid/content/Context;)[Ljava/lang/String;") \
        METHOD (connect, "connect", "(Ljava/lang/String;)Z") \
        METHOD (isOpen, "isOpen", "()Z") \
        METHOD (setParameters, "setParameters", "(IIII)Z") \
        METHOD (disconnect, "disconnect", "()V") \
        METHOD (write, "write", "([BI)Z") \
        METHOD (read, "read", "([BI)I")
    DECLARE_JNI_CLASS (UsbSerialHelper, "com/artiphon/juce_serial/UsbSerialHelper")
    #undef JNI_CLASS_MEMBERS

    // JNI calls do not throw C++ exceptions, a Java exception is left pending, and must be cleared before
    // making any other JNI call
    inline bool exceptionOccurred (JNIEnv* env)
    {
        if (! env->ExceptionCheck ())
            return false;
        env->ExceptionDescribe ();
        env->ExceptionClear ();
        return true;
    }

    inline jint getStaticInt (JNIEnv* env, jfieldID field)
    {
        return env->GetStaticIntField (UsbSerialPort, field);
    }
}

struct SerialPort::Native
{
    juce::GlobalRef helper;
    // only used by the reader thread
    juce::GlobalRefImpl<jbyteArray> readArray;
};

juce::StringPairArray SerialPort::getSerialPortPaths ()
{
    using namespace juce::serialport_jni;

    juce::StringPairArray serialPortPaths;

    auto* env { juce::getEnv () };
    juce::LocalRef<jobjectArray> namesAndPaths { static_cast<jobjectArray> (env->CallStaticObjectMethod (UsbSerialHelper, UsbSerialHelper.getSerialPortPaths,
                                                                                                         juce::getAppContext ().get ())) };
    if (exceptionOccurred (env))
        return serialPortPaths;

    const auto strings { juce::javaStringArrayToJuce (namesAndPaths) };
    for (auto index { 0 }; index + 1 < strings.size (); index += 2)
        serialPortPaths.set (strings[index], strings[index + 1]);

    return serialPortPaths;
}

bool SerialPort::isOpen () const
{
    if (native->helper == nullptr)
        return false;

    // the helper notices the device going away, even when no reader thread is running
    auto* env { juce::getEnv () };
    const auto open { env->CallBooleanMethod (native->helper.get (), juce::serialport_jni::UsbSerialHelper.isOpen) };
    return ! juce::serialport_jni::exceptionOccurred (env) && open;
}

void SerialPort::close ()
{
    // the streams use the helper, so they must be deleted before the port is closed
    jassert (numAttachedStreams == 0);

    if (native->helper == nullptr)
        return;

    auto* env { juce::getEnv () };
    env->CallVoidMethod (native->helper.get (), juce::serialport_jni::UsbSerialHelper.disconnect);
    juce::serialport_jni::exceptionOccurred (env);
    native->helper.clear ();
}

bool SerialPort::open (const juce::String& newPortPath)
{
    using namespace juce::serialport_jni;

    close ();

    portPath = newPortPath;
    connectionLost = false;
    configuredBps = 0;

    auto* env { juce::getEnv () };
    juce::LocalRef<jobject> helper { env->NewObject (UsbSerialHelper, UsbSerialHelper.constructor, juce::getAppContext ().get (), juce::getMainActivity ().get ()) };
    if (exceptionOccurred (env) || helper == nullptr)
    {
        DebugLog ("SerialPort::open", "could not create UsbSerialHelper");
        return false;
    }

    const auto connected { env->CallBooleanMethod (helper.get (), UsbSerialHelper.connect, juce::javaString (newPortPath).get ()) };
    if (exceptionOccurred (env) || ! connected)
    {
        DebugLog ("SerialPort::open", "could not connect to: " + newPortPath);
        return false;
    }

    native->helper = juce::GlobalRef { helper };
    return true;
}

void SerialPort::cancel ()
{
    // nothing to do, the stream threads never block for longer than their timeouts
}

bool SerialPort::setConfig (const SerialPortConfig& config)
{
    using namespace juce::serialport_jni;

    if (! isOpen ())
        return false;

    // flow control isn't supported by UsbSerialPort
    if (config.flowcontrol != SerialPortConfig::FLOWCONTROL_NONE)
        return false;

    auto* env { juce::getEnv () };
    jint stopBits { 0 };
    switch (config.stopbits)
    {
        case SerialPortConfig::STOPBITS_1:        stopBits = getStaticInt (env, UsbSerialPort.STOPBITS_1); break;
        case SerialPortConfig::STOPBITS_1ANDHALF: stopBits = getStaticInt (env, UsbSerialPort.STOPBITS_1_5); break;
        case SerialPortConfig::STOPBITS_2:        stopBits = getStaticInt (env, UsbSerialPort.STOPBITS_2); break;
        default:
            jassertfalse;
            return false;
    }

    jint parity { 0 };
    switch (config.parity)
    {
        case SerialPortConfig::SERIALPORT_PARITY_NONE:  parity = getStaticInt (env, UsbSerialPort.PARITY_NONE); break;
        case SerialPortConfig::SERIALPORT_PARITY_ODD:   parity = getStaticInt (env, UsbSerialPort.PARITY_ODD); break;
        case SerialPortConfig::SERIALPORT_PARITY_EVEN:  parity = getStaticInt (env, UsbSerialPort.PARITY_EVEN); break;
        case SerialPortConfig::SERIALPORT_PARITY_SPACE: parity = getStaticInt (env, UsbSerialPort.PARITY_SPACE); break;
        case SerialPortConfig::SERIALPORT_PARITY_MARK:  parity = getStaticInt (env, UsbSerialPort.PARITY_MARK); break;
        default:
            jassertfalse;
            return false;
    }

    const auto result { env->CallBooleanMethod (native->helper.get (), UsbSerialHelper.setParameters,
                                                static_cast<jint> (config.bps), static_cast<jint> (config.databits), stopBits, parity) };
    if (exceptionOccurred (env) || ! result)
        return false;
    configuredBps = config.bps;
    return true;
}

bool SerialPort::getConfig (SerialPortConfig& config)
{
    using namespace juce::serialport_jni;

    if (! isOpen ())
        return false;

    auto* env { juce::getEnv () };
    const auto helper { native->helper.get () };

    const auto baudRate { env->GetIntField (helper, UsbSerialHelper.baudRate) };
    const auto dataBits { env->GetIntField (helper, UsbSerialHelper.dataBits) };
    // the parameters have not been set yet
    if (baudRate < 0 || dataBits < 0)
        return false;

    config.bps = static_cast<uint32_t> (baudRate);
    config.databits = static_cast<uint32_t> (dataBits);
    config.flowcontrol = SerialPortConfig::FLOWCONTROL_NONE;

    const auto stopBits { env->GetIntField (helper, UsbSerialHelper.stopBits) };
    if (stopBits == getStaticInt (env, UsbSerialPort.STOPBITS_1))
        config.stopbits = SerialPortConfig::STOPBITS_1;
    else if (stopBits == getStaticInt (env, UsbSerialPort.STOPBITS_1_5))
        config.stopbits = SerialPortConfig::STOPBITS_1ANDHALF;
    else if (stopBits == getStaticInt (env, UsbSerialPort.STOPBITS_2))
        config.stopbits = SerialPortConfig::STOPBITS_2;
    else
        return false;

    const auto parity { env->GetIntField (helper, UsbSerialHelper.parity) };
    if (parity == getStaticInt (env, UsbSerialPort.PARITY_NONE))
        config.parity = SerialPortConfig::SERIALPORT_PARITY_NONE;
    else if (parity == getStaticInt (env, UsbSerialPort.PARITY_ODD))
        config.parity = SerialPortConfig::SERIALPORT_PARITY_ODD;
    else if (parity == getStaticInt (env, UsbSerialPort.PARITY_EVEN))
        config.parity = SerialPortConfig::SERIALPORT_PARITY_EVEN;
    else if (parity == getStaticInt (env, UsbSerialPort.PARITY_SPACE))
        config.parity = SerialPortConfig::SERIALPORT_PARITY_SPACE;
    else if (parity == getStaticInt (env, UsbSerialPort.PARITY_MARK))
        config.parity = SerialPortConfig::SERIALPORT_PARITY_MARK;
    else
        return false;

    return true;
}

int SerialPort::readBytes (void* destBuffer, int maxBytesToRead, int timeoutMs)
{
    using namespace juce::serialport_jni;

    auto* env { juce::getEnv () };
    if (native->readArray == nullptr || env->GetArrayLength (native->readArray.get ()) < maxBytesToRead)
        native->readArray = juce::GlobalRefImpl<jbyteArray> { juce::LocalRef<jbyteArray> { env->NewByteArray (maxBytesToRead) } };

    // the Java read waits for data, so this does not spin
    const auto bytesRead { env->CallIntMethod (native->helper.get (), UsbSerialHelper.read, native->readArray.get (), static_cast<jint> (timeoutMs)) };
    if (exceptionOccurred (env))
        return -1;
    if (bytesRead > 0)
        env->GetByteArrayRegion (native->readArray.get (), 0, juce::jmin (bytesRead, maxBytesToRead), static_cast<jbyte*> (destBuffer));
    return juce::jmin (bytesRead, maxBytesToRead);
}

int SerialPort::writeBytes (const void* sourceBuffer, int numBytesToWrite, int timeoutMs)
{
    using namespace juce::serialport_jni;

    auto* env { juce::getEnv () };
    juce::LocalRef<jbyteArray> data { env->NewByteArray (numBytesToWrite) };
    env->SetByteArrayRegion (data.get (), 0, numBytesToWrite, static_cast<const jbyte*> (sourceBuffer));

    const auto written { env->CallBooleanMethod (native->helper.get (), UsbSerialHelper.write, data.get (), static_cast<jint> (timeoutMs)) };
    if (exceptionOccurred (env) || ! written)
        return -1;
    return numBytesToWrite;
}
