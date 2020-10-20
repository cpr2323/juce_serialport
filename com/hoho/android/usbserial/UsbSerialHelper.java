package com.hoho.android.usbserial;


import android.app.Activity;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbManager;
import android.os.Handler;
import android.os.Looper;
import android.text.SpannableStringBuilder;
import android.widget.TextView;
import android.widget.Toast;

import com.artiphon.orba.BuildConfig;
import com.hoho.android.usbserial.driver.CdcAcmSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
import com.hoho.android.usbserial.util.HexDump;
import com.hoho.android.usbserial.util.SerialInputOutputManager;

import java.io.IOException;
import java.util.Arrays;
import java.util.concurrent.Executors;

import static android.content.Context.USB_SERVICE;
import java.util.List;

public class UsbSerialHelper implements SerialInputOutputManager.Listener {

    private enum UsbPermission { Unknown, Requested, Granted, Denied };

    private static final String INTENT_ACTION_GRANT_USB = BuildConfig.APPLICATION_ID + ".GRANT_USB";
    private static final int WRITE_WAIT_MILLIS = 2000;
    private static final int READ_WAIT_MILLIS = 2000;

    private BroadcastReceiver broadcastReceiver;
    private Handler mainLooper;

    private SerialInputOutputManager usbIoManager;
    private UsbSerialPort usbSerialPort;
    private UsbPermission usbPermission = UsbPermission.Unknown;
    private boolean connected = false;

    //This value is taken from ArtiCommSerialPort::internalOpen()
    private int baudRate = 115200;

    Context context;
    Activity mainActivity;

    public void DBG(String text, Context context) {
        Toast.makeText(context, text, Toast.LENGTH_SHORT).show();
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

                DBG(allPorts, context);
                return allPorts;
            }
        } catch (Exception e) {
            DBG("****************************** exception!!! ", context);
            e.printStackTrace();
        }

        return "";
    }

    public UsbSerialHelper(Context contextIn, Activity mainActivityIn) {
        context = contextIn;
        mainActivity = mainActivityIn;
        broadcastReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                if (intent.getAction().equals(INTENT_ACTION_GRANT_USB)) {
                    usbPermission = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false) ? UsbPermission.Granted : UsbPermission.Denied;
//                    connect();
                }
            }
        };
        mainLooper = new Handler(Looper.getMainLooper());
    }

    @Override
    public void onNewData(byte[] data) {
        mainLooper.post(() -> {
            receive(data);
        });
    }

    @Override
    public void onRunError(Exception e) {
        mainLooper.post(() -> {
            DBG("connection lost: " + e.getMessage(), context);
            disconnect();
        });
    }

    public boolean connect(int portNum) {
        UsbDevice device = null;
        UsbManager usbManager = (UsbManager) context.getSystemService(USB_SERVICE);

        //TODO: deal with the case where there`s more than 1 device
        for (UsbDevice v : usbManager.getDeviceList().values())
//            if (v.getDeviceId() == deviceId)
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
            usbSerialPort.setParameters(baudRate, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE);

            usbIoManager = new SerialInputOutputManager(usbSerialPort, this);
            Executors.newSingleThreadExecutor().submit(usbIoManager);

            connected = true;
        } catch (Exception e) {
            DBG("connection failed: " + e.getMessage(), context);
            disconnect();
            return false;
        }

        return true;
    }

    private void disconnect() {
        connected = false;

        if (usbIoManager != null)
            usbIoManager.stop();
        usbIoManager = null;

        try {
            usbSerialPort.close();
        } catch (IOException ignored) {}
        usbSerialPort = null;
    }

    public void send () {
        if (! connected) {
            DBG("not connected", context);
            return;
        }

        try {
            String str = "0";
            byte[] data = (str + '\n').getBytes();
            usbSerialPort.write(data, WRITE_WAIT_MILLIS);
        } catch (Exception e) {
            onRunError(e);
        }
    }

    private void read() {
        if (! connected) {
            DBG("not connected", context);
            return;
        }

        try {
            byte[] buffer = new byte[8192];
            int len = usbSerialPort.read(buffer, READ_WAIT_MILLIS);
            //TODO: presumably here we'd call the equivalent of receive in c++ code
            receive(Arrays.copyOf(buffer, len));
        } catch (IOException e) {
            // when using read with timeout, USB bulkTransfer returns -1 on timeout _and_ errors
            // like connection loss, so there is typically no exception thrown here on error
            DBG("connection lost: " + e.getMessage(), context);
            disconnect();
        }
    }

    private void receive(byte[] data) {
        SpannableStringBuilder spn = new SpannableStringBuilder();
        spn.append("receive " + data.length + " bytes\n");
        if(data.length > 0)
            spn.append(HexDump.dumpHexString(data)+"\n");

        DBG(spn.toString(), context);
    }
}
