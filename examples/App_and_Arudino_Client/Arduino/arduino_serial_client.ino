enum ParseState
{
  waitingForStartByte1,
  waitingForStartByte2,
  waitingForCommand,
  waitingForCommandDataSize,
  waitingForCommandData
};
ParseState parseState = waitingForStartByte1;
const int kStartByte1 = '*';
const int kStartByte2 = '~';
enum Command
{
    none,
    lightColor,
    tempo,
    chargingAlarmLevel,
    endOfList
};
const int kMaxPayloadSize = 100;
uint8_t gSerialDataBuffer[kMaxPayloadSize];

const auto kNumberOfDecimalPlaces { 4 };

const int kMaxCommandDataBytes = 4;
uint8_t gCommandData[kMaxCommandDataBytes];
uint8_t gCommand = Command::none;
uint8_t gCommandDataSize = 0;
uint8_t gCommandDataCount = 0;

float gTempo { 60.0f };
uint16_t gLightColor { 0 };
uint8_t gAlarmLevels [2] { 0, 0 };

void handleTempoCommand (uint8_t* commandData, const int commandDataSize)
{
  if (commandDataSize != 4)
    return;
  const auto tempoAsInt { commandData [0] + (static_cast<uint32_t>(commandData [1]) << 8) +
                          (static_cast<uint32_t>(commandData [2]) << 16) + (static_cast<uint32_t>(commandData [3]) << 24) };
  gTempo = static_cast<float>(tempoAsInt / pow (10, kNumberOfDecimalPlaces));
  Serial.write (Command::tempo);
}

void handleLightColorCommand (uint8_t* commandData, const int commandDataSize)
{
  if (commandDataSize != 2)
    return;
  gLightColor = commandData [0] + (static_cast<uint16_t>(commandData [1]) << 8);
  Serial.write (Command::lightColor);
}

void handleChargingAlarmLevelCommand (uint8_t* commandData, const int commandDataSize)
{
  if (commandDataSize != 2)
    return;

  const auto alarmIndex { commandData [0] };
  if (alarmIndex >= 2)
    return;

  gAlarmLevels [alarmIndex] = commandData [1];
  Serial.write (Command::chargingAlarmLevel);
}

void processCommand (uint8_t command, uint8_t* commandData, const int commandDataSize)
{
  switch (command)
  {
    case Command::none               : break;
    case Command::lightColor         : handleLightColorCommand (commandData, commandDataSize); break;
    case Command::tempo              : handleTempoCommand (commandData, commandDataSize); break;
    case Command::chargingAlarmLevel : handleChargingAlarmLevelCommand (commandData, commandDataSize); break;
    default                          : break;
  }

}

// TODO - is it possible that so much data is coming in, that we spend too much time in here, and
//        end up blocking the output updates
void parseInputData (const uint8_t*data, const int dataSize)
{
  auto resetParseState = [] ()
  {
    gCommand = Command::none;
    gCommandDataSize = 0;
    gCommandDataCount = 0;
    parseState = ParseState::waitingForStartByte1;
  };

  for (int dataIndex = 0; dataIndex < dataSize; ++dataIndex)
  {
    const uint8_t dataByte { data [dataIndex] };
    //Serial.write (dataByte);
    switch(parseState)
    {
      case ParseState::waitingForStartByte1:
      {
        if (dataByte == kStartByte1)
          parseState = ParseState::waitingForStartByte2;
      }
      break;
      case ParseState::waitingForStartByte2:
      {
        if (dataByte == kStartByte2)
          parseState = ParseState::waitingForCommand;
        else
          resetParseState ();
      }
      break;
      case ParseState::waitingForCommand:
      {
        if (dataByte >= Command::none && dataByte < Command::endOfList)
        {
          gCommand = dataByte;
          parseState = ParseState::waitingForCommandDataSize;
        }
        else
        {
          resetParseState ();
        }
      }
      break;
      case ParseState::waitingForCommandDataSize:
      {
        if (dataByte >= 0 && dataByte <= kMaxCommandDataBytes)
        {
          gCommandDataSize = dataByte;
          parseState = ParseState::waitingForCommandData;
        }
        else
        {
          resetParseState ();
        }
      }
      break;
      case ParseState::waitingForCommandData:
      {
        if (gCommandDataSize != 0)
        {
          gCommandData[gCommandDataCount] = dataByte;
          ++gCommandDataCount;
        }
        if (gCommandDataCount == gCommandDataSize)
        {
          processCommand (gCommand, gCommandData, gCommandDataSize);

          resetParseState ();
        }
      }
      break;

      default:
      {
        // unknown parse state, broken
        resetParseState ();
      }
    }
  }
}

void checkCommandInput ()
{
  if (Serial.available() == 0)
    return;

  uint8_t bytesToRead = min(Serial.available(), kMaxPayloadSize);
  Serial.readBytes(gSerialDataBuffer, bytesToRead);
  parseInputData (gSerialDataBuffer, bytesToRead);
}

// the setup function runs once when you press reset or power the board
void setup()
 {
  //Serial.begin(115200);
  Serial.begin(9600);

  // initialize digital pin LED_BUILTIN as an output.
  pinMode(LED_BUILTIN, OUTPUT);

}

enum class LoopState
{
  checkCommand,
  updateOutputs
};
LoopState gLoopState { LoopState::updateOutputs };

void doStuff ()
{
}

void loop()
 {
  switch (gLoopState)
  {
    case LoopState::checkCommand:
    {
      checkCommandInput ();

      gLoopState = LoopState::updateOutputs;
    }
    break;
    case LoopState::updateOutputs:
    {
      doStuff ();

      gLoopState = LoopState::checkCommand;
    }
    break;
  }
}
