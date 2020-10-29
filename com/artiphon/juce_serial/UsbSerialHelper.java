package com.artiphon.juce_serial;

import android.app.Activity;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import com.hoho.android.usbserial.driver.CdcAcmSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import java.io.IOException;
import java.util.ArrayDeque;
import java.util.Deque;
import java.util.concurrent.Executors;

import static android.content.Context.USB_SERVICE;
import java.util.List;
import java.util.concurrent.locks.ReentrantLock;

public class UsbSerialHelper implements SerialInputOutputManager.Listener {

    private enum UsbPermission { Unknown, Requested, Granted, Denied };

    private static final String INTENT_ACTION_GRANT_USB = "MY_APP.GRANT_USB";
    private static final int WRITE_WAIT_MILLIS = 2000;
    private static final int READ_WAIT_MILLIS = 2000;

    private int baudRate = -1;
    private int dataBits = -1;
    private int stopBits = -1;
    private int parity = -1;

    private Handler mainLooper;

    private SerialInputOutputManager usbIoManager;
    private UsbSerialPort usbSerialPort;
    private UsbPermission usbPermission = UsbPermission.Unknown;
    private boolean connected = false;

    Context context;
    Activity mainActivity;

    Deque<Byte> readDeque;
    ReentrantLock readDequeLock;

    public void DBG(String text, Context context) {
//        Toast.makeText(context, text, Toast.LENGTH_SHORT).show();
        Log.d("UsbSerialHelper#DBG", text);
    }

    String getSerialPortPaths(Context contextIn) {
        try {
            UsbManager usbManager = (UsbManager) context.getSystemService (USB_SERVICE);

            for (UsbDevice device : usbManager.getDeviceList().values()) {
                CdcAcmSerialDriver driver = new CdcAcmSerialDriver(device);

                List<UsbSerialPort> ports = driver.getPorts();

                String allPorts = "";
                for (UsbSerialPort s : ports)
                    allPorts += "serialport " + s.getPortNumber() + "-";

                return allPorts;
            }
        } catch (Exception e) {
            e.printStackTrace();
        }

        return "";
    }

    public UsbSerialHelper(Context contextIn, Activity mainActivityIn) {
        context = contextIn;
        mainActivity = mainActivityIn;
        mainLooper = new Handler(Looper.getMainLooper());
        readDeque = new ArrayDeque<>(8192);
        readDequeLock = new ReentrantLock();
    }

    public boolean connect(int portNum) {
        UsbDevice device = null;
        UsbManager usbManager = (UsbManager) context.getSystemService(USB_SERVICE);

        //TODO: deal with the case where there`s more than 1 device
        for (UsbDevice v : usbManager.getDeviceList().values())
            //if (v.getDeviceId() == deviceId)
            device = v;

        if (device == null) {
            DBG("connection failed: device not found", context);
            return false;
        }

        CdcAcmSerialDriver driver = new CdcAcmSerialDriver(device);
        if (driver.getPorts().size() < portNum) {
            DBG("connection failed: not enough ports at device", context);
            return false;
        }

        usbSerialPort = driver.getPorts().get(portNum);

        UsbDeviceConnection usbConnection = usbManager.openDevice(driver.getDevice());
        if (usbConnection == null && usbPermission == UsbPermission.Unknown && !usbManager.hasPermission(driver.getDevice())) {
            usbPermission = UsbPermission.Requested;
            PendingIntent usbPermissionIntent = PendingIntent.getBroadcast(mainActivity, 0, new Intent(INTENT_ACTION_GRANT_USB), 0);
            usbManager.requestPermission(driver.getDevice(), usbPermissionIntent);
            return false;
        }

        if (usbConnection == null) {
            if (! usbManager.hasPermission(driver.getDevice()))
                DBG("connection failed: permission denied", context);
            else
                DBG("connection failed: open failed", context);
            return false;
        }

        try {
            usbSerialPort.open(usbConnection);
            usbIoManager = new SerialInputOutputManager(usbSerialPort, this);
            Executors.newSingleThreadExecutor().submit(usbIoManager);
            connected = true;
            return true;
        } catch (Exception e) {
            e.printStackTrace();
            disconnect();
            return false;
        }
    }

    public boolean setParameters(int baudRateIn, int dataBitsIn, int stopBitsIn, int parityIn) {
        try {
            usbSerialPort.setParameters(baudRateIn, dataBitsIn, stopBitsIn, parityIn);
            baudRate = baudRateIn;
            dataBits = dataBitsIn;
            stopBits = stopBitsIn;
            parity = parityIn;
//            DBG("********************************* UsbSerialHelper#setParameters() baudrate: " + baudRate + " dataBits: " + dataBits + " stopBits: " + stopBits + " parity: " + parity, context);
            return true;
        } catch (IOException e) {
            e.printStackTrace();
            baudRate = -1;
            dataBits = -1;
            stopBits = -1;
            parity = -1;
            return false;
        }
    }

    public boolean isOpen() {
        return usbSerialPort != null && usbSerialPort.isOpen();
    }

    private void disconnect() {
        connected = false;

        if (usbIoManager != null)
            usbIoManager.stop();
        usbIoManager = null;

        try {
            if (usbSerialPort != null)
                usbSerialPort.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
        usbSerialPort = null;
    }

    public boolean write (byte[] data) {
        if (! connected) {
            DBG("not connected", context);
            return false;
        }

        //StringBuilder dbg = new StringBuilder("*************** UsbSerialHelper#write ():");
        //for (byte b1: data)
        //    dbg.append(String.format("%8s", Integer.toBinaryString(b1 & 0xFF)).replace(' ', '0') + " ");
        //Log.d("UsbSerialHelper#write", dbg.toString());

        try {
            usbSerialPort.write(data, WRITE_WAIT_MILLIS);
        } catch (Exception e) {
            onRunError(e);
            return false;
        }

        return true;
    }

    @Override
    public void onRunError(Exception e) {
        mainLooper.post(() -> {
            DBG("connection lost: " + e.getMessage(), context);
            disconnect();
        });
    }

    //TODO: should this be coalesced in another thread, maybe using the mainLooper, in case we get another call to this callback while we're copying bytes?
    @Override
    public void onNewData(byte[] data) {
        try {
            readDequeLock.lock();

            //StringBuilder dbg = new StringBuilder("*************** UsbSerialHelper#onNewData ():");
            //for (byte b1 : data)
            //    dbg.append(String.format("%8s", Integer.toBinaryString(b1 & 0xFF)).replace(' ', '0') + " ");
            //Log.d("UsbSerialHelper#onNewDa", dbg.toString());

            for (byte b : data)
                readDeque.add(b);

        } catch (Exception e) {
            e.printStackTrace();
        } finally {
            readDequeLock.unlock();
        }
    }

    public int read(byte[] buffer) {
        if (! connected) {
            //DBG("not connected", context);
            return 0;
        }

        int ret = 0;

        try {
            readDequeLock.lock();
            int bufferedSize = readDeque.size();

            if (bufferedSize > 0) {
                //StringBuilder dbg = new StringBuilder("*************** UsbSerialHelper#read ():");
                for (int i = 0; i < bufferedSize; ++i) {
                    buffer[i] = readDeque.pop().byteValue();
                    //dbg.append(String.format("%8s", Integer.toBinaryString(buffer[i] & 0xFF)).replace(' ', '0') + " ");
                }
                //Log.d("UsbSerialHelper#read", dbg.toString());

                ret = bufferedSize;
            }
        } catch (Exception e) {
            DBG("**************** issue reading de_que: " + e.getMessage(), context);
        } finally {
            readDequeLock.unlock();
        }

        return ret;
    }
}
