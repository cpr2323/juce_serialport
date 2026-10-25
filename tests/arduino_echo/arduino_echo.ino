// echoes everything it receives, for SerialPortTests --echo and the SerialTerminal echo test.
// the rate must match the --baud given to the tests (boards with native USB, eg. Leonardo, Micro, Due's native
// port, ignore the rate)
const unsigned long kBaudRate = 115200;

void setup ()
{
    Serial.begin (kBaudRate);
}

void loop ()
{
    uint8_t buffer [64];
    int available = Serial.available ();
    if (available <= 0)
        return;
    if (available > (int) sizeof (buffer))
        available = sizeof (buffer);
    const int numRead = Serial.readBytes (buffer, available);
    Serial.write (buffer, numRead);
}
