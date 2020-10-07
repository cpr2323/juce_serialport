/*
 * Ported to usb-serial-for-android by Felix Hädicke <felixhaedicke@web.de>
 *
 * Based on the pyprolific driver written by Emmanuel Blot <emmanuel.blot@free.fr>
 * See https://github.com/eblot/pyftdi
 *
 * Project home page: https://github.com/mik3y/usb-serial-for-android
 */

package com.hoho.android.usbserial.driver;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.util.Log;

//import com.hoho.android.usbserial.BuildConfig;

import java.io.IOException;
import java.util.Arrays;
import java.util.Collections;
import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

public class ProlificSerialDriver implements UsbSerialDriver {

    private final String TAG = ProlificSerialDriver.class.getSimpleName();

    private final static int[] standardBaudRates = {
            75, 150, 300, 600, 1200, 1800, 2400, 3600, 4800, 7200, 9600, 14400, 19200,
            28800, 38400, 57600, 115200, 128000, 134400, 161280, 201600, 230400, 268800,
            403200, 460800, 614400, 806400, 921600, 1228800, 2457600, 3000000, 6000000
    };
    private enum DeviceType { DEVICE_TYPE_01, DEVICE_TYPE_HX};

    private final UsbDevice mDevice;
    private final UsbSerialPort mPort;

    public ProlificSerialDriver(UsbDevice device) {
        mDevice = device;
        mPort = new ProlificSerialPort(mDevice, 0);
    }

    @Override
    public List<UsbSerialPort> getPorts() {
        return Collections.singletonList(mPort);
    }

    @Override
    public UsbDevice getDevice() {
        return mDevice;
    }

    class ProlificSerialPort extends CommonUsbSerialPort {

        private static final int USB_READ_TIMEOUT_MILLIS = 1000;
        private static final int USB_WRITE_TIMEOUT_MILLIS = 5000;

        private static final int USB_RECIP_INTERFACE = 0x01;

        private static final int PROLIFIC_VENDOR_READ_REQUEST = 0x01;
        private static final int PROLIFIC_VENDOR_WRITE_REQUEST = 0x01;

        private static final int PROLIFIC_VENDOR_OUT_REQTYPE = UsbConstants.USB_DIR_OUT | UsbConstants.USB_TYPE_VENDOR;
        private static final int PROLIFIC_VENDOR_IN_REQTYPE = UsbConstants.USB_DIR_IN | UsbConstants.USB_TYPE_VENDOR;
        private static final int PROLIFIC_CTRL_OUT_REQTYPE = UsbConstants.USB_DIR_OUT | UsbConstants.USB_TYPE_CLASS | USB_RECIP_INTERFACE;

        private static final int WRITE_ENDPOINT = 0x02;
        private static final int READ_ENDPOINT = 0x83;
        private static final int INTERRUPT_ENDPOINT = 0x81;

        private static final int FLUSH_RX_REQUEST = 0x08; // RX @ Prolific device = write @ usb-serial-for-android library
        private static final int FLUSH_TX_REQUEST = 0x09;

        private static final int SET_LINE_REQUEST = 0x20;
        private static final int SET_CONTROL_REQUEST = 0x22;

        private static final int CONTROL_DTR = 0x01;
        private static final int CONTROL_RTS = 0x02;

        private static final int STATUS_FLAG_CD = 0x01;
        private static final int STATUS_FLAG_DSR = 0x02;
        private static final int STATUS_FLAG_RI = 0x08;
        private static final int STATUS_FLAG_CTS = 0x80;

        private static final int STATUS_BUFFER_SIZE = 10;
        private static final int STATUS_BYTE_IDX = 8;

        private DeviceType mDeviceType = DeviceType.DEVICE_TYPE_HX;
        private UsbEndpoint mInterruptEndpoint;
        private int mControlLinesValue = 0;
        private int mBaudRate = -1, mDataBits = -1, mStopBits = -1, mParity = -1;

        private int mStatus = 0;
        private volatile Thread mReadStatusThread = null;
        private final Object mReadStatusThreadLock = new Object();
        boolean mStopReadStatusThread = false;
        private IOException mReadStatusException = null;


        public ProlificSerialPort(UsbDevice device, int portNumber) {
            super(device, portNumber);
        }

        @Override
        public UsbSerialDriver getDriver() {
            return ProlificSerialDriver.this;
        }

        private final byte[] inControlTransfer(int requestType, int request,
                int value, int index, int length) throws IOException {
            byte[] buffer = new byte[length];
            int result = mConnection.controlTransfer(requestType, request, value,
                    index, buffer, length, USB_READ_TIMEOUT_MILLIS);
            if (result != length) {
                throw new IOException(
                        String.format("ControlTransfer with value 0x%x failed: %d",
                                value, result));
            }
            return buffer;
        }

        private final void outControlTransfer(int requestType, int request,
                int value, int index, byte[] data) throws IOException {
            int length = (data == null) ? 0 : data.length;
            int result = mConnection.controlTransfer(requestType, request, value,
                    index, data, length, USB_WRITE_TIMEOUT_MILLIS);
            if (result != length) {
                throw new IOException(
                        String.format("ControlTransfer with value 0x%x failed: %d",
                                value, result));
            }
        }

        private final byte[] vendorIn(int value, int index, int length)
                throws IOException {
            return inControlTransfer(PROLIFIC_VENDOR_IN_REQTYPE,
                    PROLIFIC_VENDOR_READ_REQUEST, value, index, length);
        }

        private final void vendorOut(int value, int index, byte[] data)
                throws IOException {
            outControlTransfer(PROLIFIC_VENDOR_OUT_REQTYPE,
                    PROLIFIC_VENDOR_WRITE_REQUEST, value, index, data);
        }

        private void resetDevice() throws IOException {
            purgeHwBuffers(true, true);
        }

        private final void ctrlOut(int request, int value, int index, byte[] data)
                throws IOException {
            outControlTransfer(PROLIFIC_CTRL_OUT_REQTYPE, request, value, index,
                    data);
        }

        private void doBlackMagic() throws IOException {
            vendorIn(0x8484, 0, 1);
            vendorOut(0x0404, 0, null);
            vendorIn(0x8484, 0, 1);
            vendorIn(0x8383, 0, 1);
            vendorIn(0x8484, 0, 1);
            vendorOut(0x0404, 1, null);
            vendorIn(0x8484, 0, 1);
            vendorIn(0x8383, 0, 1);
            vendorOut(0, 1, null);
            vendorOut(1, 0, null);
            vendorOut(2, (mDeviceType == DeviceType.DEVICE_TYPE_HX) ? 0x44 : 0x24, null);
        }

        private void setControlLines(int newControlLinesValue) throws IOException {
            ctrlOut(SET_CONTROL_REQUEST, newControlLinesValue, 0, null);
            mControlLinesValue = newControlLinesValue;
        }

        private final void readStatusThreadFunction() {
            try {
                while (!mStopReadStatusThread) {
                    byte[] buffer = new byte[STATUS_BUFFER_SIZE];
                    long endTime = System.currentTimeMillis() + 500;
                    int readBytesCount = mConnection.bulkTransfer(mInterruptEndpoint, buffer, STATUS_BUFFER_SIZE, 500);
                    if(readBytesCount == -1 && System.currentTimeMillis() < endTime)
                        testConnection();
                    if (readBytesCount > 0) {
                        if (readBytesCount == STATUS_BUFFER_SIZE) {
                            mStatus = buffer[STATUS_BYTE_IDX] & 0xff;
                        } else {
                            throw new IOException(
                                    String.format("Invalid CTS / DSR / CD / RI status buffer received, expected %d bytes, but received %d",
                                            STATUS_BUFFER_SIZE,
                                            readBytesCount));
                        }
                    }
                }
            } catch (IOException e) {
                mReadStatusException = e;
            }
        }

        private final int getStatus() throws IOException {
            if ((mReadStatusThread == null) && (mReadStatusException == null)) {
                synchronized (mReadStatusThreadLock) {
                    if (mReadStatusThread == null) {
                        byte[] buffer = new byte[STATUS_BUFFER_SIZE];
                        int readBytes = mConnection.bulkTransfer(mInterruptEndpoint,
                                buffer,
                                STATUS_BUFFER_SIZE,
                                100);
                        if (readBytes != STATUS_BUFFER_SIZE) {
                            Log.w(TAG, "Could not read initial CTS / DSR / CD / RI status");
                        } else {
                            mStatus = buffer[STATUS_BYTE_IDX] & 0xff;
                        }

                        mReadStatusThread = new Thread(new Runnable() {
                            @Override
                            public void run() {
                                readStatusThreadFunction();
                            }
                        });
                        mReadStatusThread.setDaemon(true);
                        mReadStatusThread.start();
                    }
                }
            }

            /* throw and clear an exception which occured in the status read thread */
            IOException readStatusException = mReadStatusException;
            if (mReadStatusException != null) {
                mReadStatusException = null;
                throw readStatusException;
            }

            return mStatus;
        }

        private final boolean testStatusFlag(int flag) throws IOException {
            return ((getStatus() & flag) == flag);
        }

        @Override
        public void openInt(UsbDeviceConnection connection) throws IOException {
            UsbInterface usbInterface = mDevice.getInterface(0);

            if (!connection.claimInterface(usbInterface, true)) {
                throw new IOException("Error claiming Prolific interface 0");
            }

            for (int i = 0; i < usbInterface.getEndpointCount(); ++i) {
                UsbEndpoint currentEndpoint = usbInterface.getEndpoint(i);

                switch (currentEndpoint.getAddress()) {
                case READ_ENDPOINT:
                    mReadEndpoint = currentEndpoint;
                    break;

                case WRITE_ENDPOINT:
                    mWriteEndpoint = currentEndpoint;
                    break;

                case INTERRUPT_ENDPOINT:
                    mInterruptEndpoint = currentEndpoint;
                    break;
                }
            }

            if (mDevice.getDeviceClass() == 0x02) {
                mDeviceType = DeviceType.DEVICE_TYPE_01;
            } else {
                byte[] rawDescriptors = connection.getRawDescriptors();
                if(rawDescriptors == null || rawDescriptors.length <8) {
                    Log.w(TAG, "Could not get device descriptors, Assuming that it is a HX device");
                    mDeviceType = DeviceType.DEVICE_TYPE_HX;
                } else {
                    byte maxPacketSize0 = rawDescriptors[7];
                    if (maxPacketSize0 == 64) {
                        mDeviceType = DeviceType.DEVICE_TYPE_HX;
                    } else if ((mDevice.getDeviceClass() == 0x00)
                            || (mDevice.getDeviceClass() == 0xff)) {
                        mDeviceType = DeviceType.DEVICE_TYPE_01;
                    } else {
                        Log.w(TAG, "Could not detect PL2303 subtype, Assuming that it is a HX device");
                        mDeviceType = DeviceType.DEVICE_TYPE_HX;
                    }
                }
            }
            setControlLines(mControlLinesValue);
            resetDevice();
            doBlackMagic();
        }

        @Override
        public void closeInt() {
            try {
                mStopReadStatusThread = true;
                synchronized (mReadStatusThreadLock) {
                    if (mReadStatusThread != null) {
                        try {
                            mReadStatusThread.join();
                        } catch (Exception e) {
                            Log.w(TAG, "An error occured while waiting for status read thread", e);
                        }
                    }
                }
                resetDevice();
            } catch(Exception ignored) {}
            try {
                mConnection.releaseInterface(mDevice.getInterface(0));
            } catch(Exception ignored) {}
        }

        private int filterBaudRate(int baudRate) {
            if(/*BuildConfig.DEBUG &&*/ (baudRate & (3<<29)) == (1<<29)) {
                return baudRate & ~(1<<29); // for testing purposes accept without further checks
            }
            if (baudRate <= 0) {
                throw new IllegalArgumentException("Invalid baud rate: " + baudRate);
            }
            for(int br : standardBaudRates) {
                if (br == baudRate) {
                    return baudRate;
                }
            }
            /*
             * Formula taken from Linux + FreeBSD.
             *   baudrate = baseline / (mantissa * 4^exponent)
             * where
             *   mantissa = buf[8:0]
             *   exponent = buf[11:9]
             *
             * Note: The formula does not work for all PL2303 variants.
             *       Ok for PL2303HX. Not ok for PL2303TA. Other variants unknown.
             */
            int baseline, mantissa, exponent;
            baseline = 12000000 * 32;
            mantissa = baseline / baudRate;
            if (mantissa == 0) { // > unrealistic 384 MBaud
                throw new UnsupportedOperationException("Baud rate to high");
            }
            exponent = 0;
            while (mantissa >= 512) {
                if (exponent < 7) {
                    mantissa >>= 2;	/* divide by 4 */
                    exponent++;
                } else { // < 45.8 baud
                    throw new UnsupportedOperationException("Baud rate to low");
                }
            }
            int effectiveBaudRate = (baseline / mantissa) >> (exponent << 1);
            double baudRateError = Math.abs(1.0 - (effectiveBaudRate / (double)baudRate));
            if(baudRateError >= 0.031) // > unrealistic 11.6 Mbaud
                throw new UnsupportedOperationException(String.format("Baud rate deviation %.1f%% is higher than allowed 3%%", baudRateError*100));
            int buf = mantissa + (exponent<<9) + (1<<31);

            Log.d(TAG, String.format("baud rate=%d, effective=%d, error=%.1f%%, value=0x%08x, mantissa=%d, exponent=%d",
                    baudRate, effectiveBaudRate, baudRateError*100, buf, mantissa, exponent));
            return buf;
        }

        @Override
        public void setParameters(int baudRate, int dataBits, int stopBits, int parity) throws IOException {
            baudRate = filterBaudRate(baudRate);
            if ((mBaudRate == baudRate) && (mDataBits == dataBits)
                    && (mStopBits == stopBits) && (mParity == parity)) {
                // Make sure no action is performed if there is nothing to change
                return;
            }

            byte[] lineRequestData = new byte[7];
            lineRequestData[0] = (byte) (baudRate & 0xff);
            lineRequestData[1] = (byte) ((baudRate >> 8) & 0xff);
            lineRequestData[2] = (byte) ((baudRate >> 16) & 0xff);
            lineRequestData[3] = (byte) ((baudRate >> 24) & 0xff);

            switch (stopBits) {
            case STOPBITS_1:
                lineRequestData[4] = 0;
                break;
            case STOPBITS_1_5:
                lineRequestData[4] = 1;
                break;
            case STOPBITS_2:
                lineRequestData[4] = 2;
                break;
            default:
                throw new IllegalArgumentException("Invalid stop bits: " + stopBits);
            }

            switch (parity) {
            case PARITY_NONE:
                lineRequestData[5] = 0;
                break;
            case PARITY_ODD:
                lineRequestData[5] = 1;
                break;
            case PARITY_EVEN:
                lineRequestData[5] = 2;
                break;
            case PARITY_MARK:
                lineRequestData[5] = 3;
                break;
            case PARITY_SPACE:
                lineRequestData[5] = 4;
                break;
            default:
                throw new IllegalArgumentException("Invalid parity: " + parity);
            }

            if(dataBits < DATABITS_5 || dataBits > DATABITS_8) {
                throw new IllegalArgumentException("Invalid data bits: " + dataBits);
            }
            lineRequestData[6] = (byte) dataBits;

            ctrlOut(SET_LINE_REQUEST, 0, 0, lineRequestData);

            resetDevice();

            mBaudRate = baudRate;
            mDataBits = dataBits;
            mStopBits = stopBits;
            mParity = parity;
        }

        @Override
        public boolean getCD() throws IOException {
            return testStatusFlag(STATUS_FLAG_CD);
        }

        @Override
        public boolean getCTS() throws IOException {
            return testStatusFlag(STATUS_FLAG_CTS);
        }

        @Override
        public boolean getDSR() throws IOException {
            return testStatusFlag(STATUS_FLAG_DSR);
        }

        @Override
        public boolean getDTR() throws IOException {
            return (mControlLinesValue & CONTROL_DTR) != 0;
        }

        @Override
        public void setDTR(boolean value) throws IOException {
            int newControlLinesValue;
            if (value) {
                newControlLinesValue = mControlLinesValue | CONTROL_DTR;
            } else {
                newControlLinesValue = mControlLinesValue & ~CONTROL_DTR;
            }
            setControlLines(newControlLinesValue);
        }

        @Override
        public boolean getRI() throws IOException {
            return testStatusFlag(STATUS_FLAG_RI);
        }

        @Override
        public boolean getRTS() throws IOException {
            return (mControlLinesValue & CONTROL_RTS) != 0;
        }

        @Override
        public void setRTS(boolean value) throws IOException {
            int newControlLinesValue;
            if (value) {
                newControlLinesValue = mControlLinesValue | CONTROL_RTS;
            } else {
                newControlLinesValue = mControlLinesValue & ~CONTROL_RTS;
            }
            setControlLines(newControlLinesValue);
        }


        @Override
        public EnumSet<ControlLine> getControlLines() throws IOException {
            int status = getStatus();
            EnumSet<ControlLine> set = EnumSet.noneOf(ControlLine.class);
            if((mControlLinesValue & CONTROL_RTS) != 0) set.add(ControlLine.RTS);
            if((status & STATUS_FLAG_CTS) != 0) set.add(ControlLine.CTS);
            if((mControlLinesValue & CONTROL_DTR) != 0) set.add(ControlLine.DTR);
            if((status & STATUS_FLAG_DSR) != 0) set.add(ControlLine.DSR);
            if((status & STATUS_FLAG_CD) != 0) set.add(ControlLine.CD);
            if((status & STATUS_FLAG_RI) != 0) set.add(ControlLine.RI);
            return set;
        }

        @Override
        public EnumSet<ControlLine> getSupportedControlLines() throws IOException {
            return EnumSet.allOf(ControlLine.class);
        }

        @Override
        public void purgeHwBuffers(boolean purgeWriteBuffers, boolean purgeReadBuffers) throws IOException {
            if (purgeWriteBuffers) {
                vendorOut(FLUSH_RX_REQUEST, 0, null);
            }

            if (purgeReadBuffers) {
                vendorOut(FLUSH_TX_REQUEST, 0, null);
            }
        }
    }

    public static Map<Integer, int[]> getSupportedDevices() {
        final Map<Integer, int[]> supportedDevices = new LinkedHashMap<Integer, int[]>();
        supportedDevices.put(UsbId.VENDOR_PROLIFIC,
                new int[] { UsbId.PROLIFIC_PL2303, });
        return supportedDevices;
    }
}
