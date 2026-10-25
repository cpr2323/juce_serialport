package com.artiphon.juce_serial;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.util.Log;

import com.hoho.android.usbserial.driver.CdcAcmSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.driver.UsbSerialProber;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import java.io.IOException;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.Condition;
import java.util.concurrent.locks.ReentrantLock;

import static android.content.Context.USB_SERVICE;

// Used by juce_serialport_android.cpp. connect, setParameters and disconnect are called by the owner of the port,
// read by the reader thread, write by the writer thread, and the listener callbacks come from the
// SerialInputOutputManager's thread
public class UsbSerialHelper implements SerialInputOutputManager.Listener {

    private static final String TAG = "UsbSerialHelper";
    private static final String INTENT_ACTION_GRANT_USB = "com.artiphon.juce_serial.GRANT_USB";
    private static final String LEGACY_PATH_PREFIX = "serialport ";
    private static final int MAX_BUFFERED_BYTES = 1024 * 1024;

    // devices permission has been asked for, so the user is only asked once, instead of every time open is retried
    private static final Set<String> permissionRequested = new HashSet<>();

    private int baudRate = -1;
    private int dataBits = -1;
    private int stopBits = -1;
    private int parity = -1;

    private final Context context;
    private final Activity mainActivity;

    private SerialInputOutputManager usbIoManager;
    private UsbSerialPort usbSerialPort;
    private volatile boolean connected = false;

    private final ArrayDeque<Byte> readDeque = new ArrayDeque<>(8192);
    private final ReentrantLock readDequeLock = new ReentrantLock();
    private final Condition dataAvailable = readDequeLock.newCondition();

    public UsbSerialHelper(Context contextIn, Activity mainActivityIn) {
        context = contextIn;
        mainActivity = mainActivityIn;
    }

    private static List<UsbSerialDriver> findAllDrivers(UsbManager usbManager) {
        List<UsbSerialDriver> drivers = new ArrayList<>();
        UsbSerialProber prober = UsbSerialProber.getDefaultProber();
        for (UsbDevice device : usbManager.getDeviceList().values()) {
            UsbSerialDriver driver = prober.probeDevice(device);
            // devices which are not in the probe table, eg. many Arduino compatibles, are treated as CDC ACM devices
            if (driver == null)
                driver = new CdcAcmSerialDriver(device);
            drivers.add(driver);
        }
        return drivers;
    }

    private static String getFriendlyName(UsbDevice device, int portNumber, int numPorts) {
        String name = device.getProductName();
        if (name == null || name.isEmpty())
            name = String.format("USB device %04X:%04X", device.getVendorId(), device.getProductId());
        return numPorts > 1 ? name + " (port " + portNumber + ")" : name;
    }

    // returns pairs of friendly name and port path
    public static String[] getSerialPortPaths(Context context) {
        List<String> result = new ArrayList<>();
        try {
            UsbManager usbManager = (UsbManager) context.getSystemService(USB_SERVICE);
            for (UsbSerialDriver driver : findAllDrivers(usbManager)) {
                List<UsbSerialPort> ports = driver.getPorts();
                for (UsbSerialPort port : ports) {
                    result.add(getFriendlyName(driver.getDevice(), port.getPortNumber(), ports.size()));
                    result.add(driver.getDevice().getDeviceName() + ":" + port.getPortNumber());
                }
            }
        } catch (Exception e) {
            Log.e(TAG, "getSerialPortPaths failed", e);
        }
        return result.toArray(new String[0]);
    }

    public synchronized boolean connect(String portPath) {
        disconnect();

        UsbManager usbManager = (UsbManager) context.getSystemService(USB_SERVICE);
        List<UsbSerialDriver> drivers = findAllDrivers(usbManager);

        UsbSerialDriver driver = null;
        int portNumber;
        if (portPath.startsWith(LEGACY_PATH_PREFIX)) {
            // paths from version 2 of the module were "serialport <port number>" on the first device
            try {
                portNumber = Integer.parseInt(portPath.substring(LEGACY_PATH_PREFIX.length()).trim());
            } catch (NumberFormatException e) {
                portNumber = 0;
            }
            if (!drivers.isEmpty())
                driver = drivers.get(0);
        } else {
            int separator = portPath.lastIndexOf(':');
            if (separator < 0) {
                Log.d(TAG, "connection failed: invalid port path: " + portPath);
                return false;
            }
            String deviceName = portPath.substring(0, separator);
            try {
                portNumber = Integer.parseInt(portPath.substring(separator + 1));
            } catch (NumberFormatException e) {
                Log.d(TAG, "connection failed: invalid port path: " + portPath);
                return false;
            }
            for (UsbSerialDriver d : drivers)
                if (d.getDevice().getDeviceName().equals(deviceName))
                    driver = d;
        }

        if (driver == null) {
            Log.d(TAG, "connection failed: device not found");
            return false;
        }
        if (portNumber < 0 || portNumber >= driver.getPorts().size()) {
            Log.d(TAG, "connection failed: not enough ports at device");
            return false;
        }

        UsbDevice device = driver.getDevice();
        if (!usbManager.hasPermission(device)) {
            synchronized (permissionRequested) {
                if (!permissionRequested.contains(device.getDeviceName())) {
                    permissionRequested.add(device.getDeviceName());
                    // the result is not listened for, the owner retries opening the port
                    Intent intent = new Intent(INTENT_ACTION_GRANT_USB);
                    intent.setPackage(context.getPackageName());
                    PendingIntent usbPermissionIntent = PendingIntent.getBroadcast(mainActivity != null ? mainActivity : context, 0, intent, PendingIntent.FLAG_IMMUTABLE);
                    usbManager.requestPermission(device, usbPermissionIntent);
                }
            }
            Log.d(TAG, "connection failed: permission not granted");
            return false;
        }

        UsbDeviceConnection usbConnection = usbManager.openDevice(device);
        if (usbConnection == null) {
            Log.d(TAG, "connection failed: open failed");
            return false;
        }

        try {
            readDequeLock.lock();
            try {
                readDeque.clear();
            } finally {
                readDequeLock.unlock();
            }

            usbSerialPort = driver.getPorts().get(portNumber);
            usbSerialPort.open(usbConnection);
            usbIoManager = new SerialInputOutputManager(usbSerialPort, this);
            usbIoManager.start();
            connected = true;
            return true;
        } catch (Exception e) {
            Log.e(TAG, "connection failed", e);
            disconnect();
            return false;
        }
    }

    public synchronized boolean setParameters(int baudRateIn, int dataBitsIn, int stopBitsIn, int parityIn) {
        if (usbSerialPort == null)
            return false;
        try {
            usbSerialPort.setParameters(baudRateIn, dataBitsIn, stopBitsIn, parityIn);
            baudRate = baudRateIn;
            dataBits = dataBitsIn;
            stopBits = stopBitsIn;
            parity = parityIn;
            return true;
        } catch (Exception e) {
            Log.e(TAG, "setParameters failed", e);
            baudRate = -1;
            dataBits = -1;
            stopBits = -1;
            parity = -1;
            return false;
        }
    }

    public boolean isOpen() {
        return connected;
    }

    public synchronized void disconnect() {
        connected = false;

        if (usbIoManager != null) {
            usbIoManager.setListener(null);
            usbIoManager.stop();
        }
        usbIoManager = null;

        try {
            // this also interrupts the SerialInputOutputManager's blocking read
            if (usbSerialPort != null)
                usbSerialPort.close();
        } catch (IOException e) {
            Log.e(TAG, "close failed", e);
        }
        usbSerialPort = null;

        // wake up a waiting read, so it sees the port has gone
        readDequeLock.lock();
        try {
            dataAvailable.signalAll();
        } finally {
            readDequeLock.unlock();
        }
    }

    public boolean write(byte[] data, int timeoutMs) {
        UsbSerialPort port = usbSerialPort;
        if (!connected || port == null)
            return false;

        try {
            port.write(data, timeoutMs);
        } catch (Exception e) {
            onRunError(e);
            return false;
        }
        return true;
    }

    @Override
    public void onRunError(Exception e) {
        Log.d(TAG, "connection lost: " + e.getMessage());
        // the owner sees isOpen () return false, and closes the port, which calls disconnect
        connected = false;
        readDequeLock.lock();
        try {
            dataAvailable.signalAll();
        } finally {
            readDequeLock.unlock();
        }
    }

    @Override
    public void onNewData(byte[] data) {
        readDequeLock.lock();
        try {
            for (byte b : data) {
                // nobody is reading, drop the oldest data rather than using unbounded memory
                if (readDeque.size() >= MAX_BUFFERED_BYTES)
                    readDeque.poll();
                readDeque.add(b);
            }
            dataAvailable.signalAll();
        } finally {
            readDequeLock.unlock();
        }
    }

    // waits up to timeoutMs for data. returns the number of bytes read, 0 on timeout, or -1 if the port has gone
    public int read(byte[] buffer, int timeoutMs) {
        readDequeLock.lock();
        try {
            if (readDeque.isEmpty() && connected)
                dataAvailable.await(timeoutMs, TimeUnit.MILLISECONDS);

            if (readDeque.isEmpty())
                return connected ? 0 : -1;

            int numBytes = Math.min(buffer.length, readDeque.size());
            for (int i = 0; i < numBytes; ++i)
                buffer[i] = readDeque.poll();
            return numBytes;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return 0;
        } finally {
            readDequeLock.unlock();
        }
    }
}
