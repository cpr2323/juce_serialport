package com.hoho.android.usbserial;

import android.content.Context;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Bundle;
import androidx.fragment.app.ListFragment;
import android.view.Menu;
import android.view.MenuInflater;
import android.view.MenuItem;
import android.view.View;
import android.view.ViewGroup;
import android.widget.ArrayAdapter;
import android.widget.ListView;
import android.widget.TextView;
import android.widget.Toast;

import com.hoho.android.usbserial.driver.CdcAcmSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialDriver;
import com.hoho.android.usbserial.driver.UsbSerialPort;
//import com.rmsl.juce.JuceMidiSupport;

import java.util.ArrayList;
import java.util.List;


import static android.content.Context.USB_SERVICE;

public class UsbSerialHelper extends ListFragment {

    public void DBG(String text, Context context) {
        Toast.makeText(context, text, Toast.LENGTH_SHORT).show();
    }

    class ListItem {
        UsbDevice device;
        int port;
        UsbSerialDriver driver;

        ListItem(UsbDevice device, int port, UsbSerialDriver driver) {
            this.device = device;
            this.port = port;
            this.driver = driver;
        }
    }

    private ArrayList<ListItem> listItems = new ArrayList<>();
    private ArrayAdapter<ListItem> listAdapter;
    private int baudRate = 19200;
    private boolean withIoManager = true;

    @Override
    public void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setHasOptionsMenu(true);
        listAdapter = new ArrayAdapter<ListItem>(getActivity(), 0, listItems) {
            @Override
            public View getView(int position, View view, ViewGroup parent) {
                ListItem item = listItems.get(position);

//                if (view == null)
//                    view = getActivity().getLayoutInflater().inflate(R.layout.device_list_item, parent, false);
//
//                TextView text1 = view.findViewById(R.id.text1);
//                if (item.driver == null)
//                    text1.setText("<no driver>");
//                else if(item.driver.getPorts().size() == 1)
//                    text1.setText(item.driver.getClass().getSimpleName().replace("SerialDriver",""));
//                else
//                    text1.setText(item.driver.getClass().getSimpleName().replace("SerialDriver","")+", Port "+item.port);
//
//                TextView text2 = view.findViewById(R.id.text2);
//                text2.setText(String.format(Locale.US, "Vendor %04X, Product %04X", item.device.getVendorId(), item.device.getProductId()));
                return view;
            }
        };
    }

    @Override
    public void onActivityCreated(Bundle savedInstanceState) {
        super.onActivityCreated(savedInstanceState);
        setListAdapter(null);
//        View header = getActivity().getLayoutInflater().inflate(R.layout.device_list_header, null, false);
//        getListView().addHeaderView(header, null, false);
        setEmptyText("<no USB devices found>");
        ((TextView) getListView().getEmptyView()).setTextSize(18);
        setListAdapter(listAdapter);
    }

    @Override
    public void onCreateOptionsMenu(Menu menu, MenuInflater inflater) {
//        inflater.inflate(R.menu.menu_devices, menu);
    }

    @Override
    public void onResume() {
        super.onResume();
//        refresh();
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
//        int id = item.getItemId();
//        if(id == R.id.refresh) {
//            refresh();
//            return true;
//        } else if (id ==R.id.baud_rate) {
//            final String[] values = getResources().getStringArray(R.array.baud_rates);
//            int pos = java.util.Arrays.asList(values).indexOf(String.valueOf(baudRate));
//            AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
//            builder.setTitle("Baud rate");
//            builder.setSingleChoiceItems(values, pos, (dialog, which) -> {
//                baudRate = Integer.parseInt(values[which]);
//                dialog.dismiss();
//            });
//            builder.create().show();
//            return true;
//        } else if (id ==R.id.read_mode) {
//            final String[] values = getResources().getStringArray(R.array.read_modes);
//            int pos = withIoManager ? 0 : 1; // read_modes[0]=event/io-manager, read_modes[1]=direct
//            AlertDialog.Builder builder = new AlertDialog.Builder(getActivity());
//            builder.setTitle("Read mode");
//            builder.setSingleChoiceItems(values, pos, (dialog, which) -> {
//                withIoManager = (which == 0);
//                dialog.dismiss();
//            });
//            builder.create().show();
//            return true;
//        } else {
//            return super.onOptionsItemSelected(item);
//        }
        return true;
    }

    String refresh(Context context) {
        try {
            UsbManager usbManager = (UsbManager) context.getSystemService (USB_SERVICE);

            for (UsbDevice device : usbManager.getDeviceList().values()) {
                CdcAcmSerialDriver driver = new CdcAcmSerialDriver(device);

                List<UsbSerialPort> ports = driver.getPorts();

                String allPorts = "";
                for (UsbSerialPort s : ports)
                    allPorts += "serialport " + s.getPortNumber() + "\n";

                DBG(allPorts, context);
                return allPorts;

            }
        } catch (Exception e) {
            DBG("****************************** exception!!! ", context);
            e.printStackTrace();
        }

        return "";
    }

    @Override
    public void onListItemClick(ListView l, View v, int position, long id) {
        ListItem item = listItems.get(position-1);
        if(item.driver == null) {
            Toast.makeText(getActivity(), "no driver", Toast.LENGTH_SHORT).show();
        } else {
            Bundle args = new Bundle();
            args.putInt("device", item.device.getDeviceId());
            args.putInt("port", item.port);
            args.putInt("baud", baudRate);
            args.putBoolean("withIoManager", withIoManager);
//            Fragment fragment = new TerminalFragment();
//            fragment.setArguments(args);
//            assert getFragmentManager() != null;
//            getFragmentManager().beginTransaction().replace(R.id.fragment, fragment, "terminal").addToBackStack(null).commit();
        }
    }
}
