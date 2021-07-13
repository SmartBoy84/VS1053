#include <Arduino.h>
#include <SPI.h>
#include <patch.h>

//#define vol(i) i << 8 | i
//Macros are scary man

static inline uint16_t vol(uint8_t i)
{
  i = ((100 - i) * 254) / 100;
  return i << 8 | i;
} //ah, much better; type checking, static compilation - the whole shebang!

const uint8_t SCI_MODE = 0x0;
const uint8_t SCI_STATUS = 0x1; //0x48 (analog enabled), 0x40 (analog driver enabled - ready)
const uint8_t SCI_BASS = 0x2;
const uint8_t SCI_CLOCKF = 0x3;
const uint8_t SCI_DECODE_TIME = 0x4; // current decoded time in full seconds
const uint8_t SCI_AUDATA = 0x5;
const uint8_t SCI_WRAM = 0x6;
const uint8_t SCI_WRAMADDR = 0x7;
const uint8_t SCI_HDAT0 = 0x8;
const uint8_t SCI_AIADDR = 0xA;
const uint8_t SCI_VOL = 0xB;
const uint8_t SCI_AICTRL0 = 0xC;
const uint8_t SCI_AICTRL1 = 0xD;

//Bits for SCI_MODE (0x0)
const uint8_t SM_SDINEW = 11; // Bitnumber in SCI_MODE always on
const uint8_t SM_RESET = 2;   // Bitnumber in SCI_MODE soft reset
const uint8_t SM_CANCEL = 3;  // Bitnumber in SCI_MODE cancel song
const uint8_t SM_TESTS = 5;   // Bitnumber in SCI_MODE for tests
const uint8_t SM_LINE1 = 14;  // Bitnumber in SCI_MODE for Line input
//Check out the cool SCI_STATUS bits on pg 40
//Basec ontrol also available
//Clock multipliers (Multiple xtali - external - to create greater clki ) on pg 42

uint8_t cs = 5;     //chip select
uint8_t dcs = 16;   //data chip select
uint8_t dreq = 4;   //data request (data lane)
uint8_t reset = 12; //reset pin

uint32_t sci_speed = (12 * 4.5 * 1000000) / 7; //While spi writes can be done at a higher speed, this is enough for our usecase - 3.5 * XTALI (12)/7
uint32_t sfreq = (12 * 1000000) / 7;

boolean status = false;

uint8_t stamina = 5;     //attempt communication with slave 5 times before giving up
uint8_t chunk_size = 32; //VS module *prefers* data to be sent in chunks of 32 bits (mulitple of it's fifo) and we only need to check dreq after every 32 bits

//old sign test
const uint8_t SINE_ARRAY_SIZE = 8;                                                  //just to make it easier for me
uint8_t sine_start[SINE_ARRAY_SIZE] = {0x53, 0xEF, 0x6E, 0xFF, 0x0, 0x0, 0x0, 0x0}; //on datasheet, pg 66 10.12.1 (sine test)
uint8_t sine_end[SINE_ARRAY_SIZE] = {0x45, 0x78, 0x69, 0x74, 0x0, 0x0, 0x0, 0x0};
uint16_t sine_frequencies[SINE_ARRAY_SIZE] = {44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000};

//Don't mind me, just defining a bunch o' functions
boolean control_mode(boolean state, boolean mode);
boolean write_reg(uint8_t _reg, uint16_t value);
boolean old_sine_test(uint16_t frequency);
//boolean sine_test(uint16_t frequency);

uint16_t read_reg(uint16_t _reg);

void life_support(void *parameter);
void reset_mod(uint8_t attempts);

/********/

void setup()
{
  Serial.begin(115200);
  SPI.begin();

  pinMode(2, OUTPUT);

  pinMode(dreq, INPUT); // DREQ is an input
  pinMode(cs, OUTPUT);  // The SCI and SDI signals
  pinMode(dcs, OUTPUT);
  pinMode(reset, OUTPUT);

  Serial.println("Setting up the VS module\n");
  reset_mod(-1); //should continue infinitely, program shouldn't start if the VS codec isn't even online...

  write_reg(SCI_VOL, vol(75));

  xTaskCreatePinnedToCore(life_support, "Life Support", 1300, NULL, 0, NULL, 0);
}

void loop()
{
  Serial.println(read_reg(SCI_HDAT0));

  delay(50);
}

void life_support(void *parameter)
{
  for (;;)
  {
    if (!read_reg(SCI_STATUS)) //this will trip if ANY wire essential to SPI communication is not connected properly (every wire apart from dcxs - chip select for SDI mode)
    {
      if (status)
      {
        Serial.println("It's gone offline, doing CPR!");
        reset_mod(stamina);
      }
      else
        reset_mod(1); //try to get back every loop
    }

    delay(1);
  }
}

void reset_mod(uint8_t attempts)
{
  digitalWrite(2, status = false);

  for (int i = 0; i != attempts; i++)
  {
    Serial.print("Attempt number: ");
    Serial.println(i);

    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(sfreq, MSBFIRST, SPI_MODE0));

    digitalWrite(cs, HIGH);  // Set inflow for both SDI and SCI to off
    digitalWrite(dcs, HIGH); // Note: States are inversed

    //Completely reset the module
    digitalWrite(reset, LOW);
    delay(500);
    digitalWrite(reset, HIGH);
    delay(750);

    //at this point, dreq should be high
    if (!digitalRead(dreq))
    {
      Serial.println("DREQ isn't high? Let's try that again...");
      continue;
    }

    //Set proper clock to enable max sample rate
    if (!write_reg(SCI_CLOCKF, 0xC000)) //SC_MULT=4.5x, SC_FREQ is 0 meaning I have not attached an external XTALI clock (default one is being used) ,SC_ADD=0.0x the module, realistically, shouldn't go higher than god damn 54mhz
      continue;

    SPI.endTransaction();
    SPI.beginTransaction(SPISettings(sci_speed, MSBFIRST, SPI_MODE0)); //set mc speed to 'fast'

    Serial.print("Testing fast r/w...\n");
    //Serial.println((float) 2.0 + ((((read_reg(SCI_CLOCKF) & 0xE000)/8192)-1)*0.5)); //clock frequency is bit 10:0, the 0xE000 mask has the high 3 bits set to 1, this should return 0x8000 (32768 in base 10)

    for (int i = 0; i != 100; i++) //just a simple test to see if we're reading correctly (volume increases in chunks of 0.5 btw)
    {
      if (write_reg(SCI_VOL, vol(i)))
        Serial.print(".");
      else
        continue;
    }

    digitalWrite(2, status = true); //SUCCESS!
    break;
  }

  Serial.println(write_reg(SCI_MODE, read_reg(SCI_MODE) | _BV(SM_TESTS)) ? "\nSet the SCI_MODE register" : "\nFailed to set the SCI_MODE register"); //_BV() is the macro => bit(x) = 1 << x
  Serial.println(status ? "Ready to rumble!\n" : "Hmmm, can't seem to talk to the old bugger.\n");
}

boolean control_mode(boolean state, boolean sdi)
{
  if (!digitalRead(sdi ? cs : dcs)) //the codec does NOT like if cs and dcs are on at the same time
    return false;                   //if the other guy is doing something then let him be and DND!

  if (state)
  {
    for (int i = 0;; i++) //max wait time is extremely small (100 CLKI = 100/6000000 seconds = 1/60000 = 0.167 millisecond) so let's just wait 2 milliseconds before panicking
    {
      if (digitalRead(dreq))
        break;

      if (i == 5)
        return false;

      delay(1); //ps all of this is probably more than 2 milliseconds but eh ¯\_(ツ)_/¯
    }
  }

  digitalWrite(sdi ? dcs : cs, !state); //this damned exclamation mark cost me two hours, I forgot the state is inverse

  return true;
}

boolean write_reg(uint8_t _reg, uint16_t value)
{
  control_mode(true, false);

  SPI.transfer(2);       //this is the 8 bit opcode for write
  SPI.transfer(_reg);    //send the address
  SPI.transfer16(value); //Send the 16 bit instruction

  control_mode(false, false);
  return _reg == SCI_AIADDR ? true : read_reg(_reg) == value; //return the status of the write process. Uhhh, apparently reading SCI_AIADDR can cause samplerate to drop (pg 46)
}

uint16_t read_reg(uint16_t _reg)
{
  control_mode(true, false);

  SPI.transfer(3);    //read opcode
  SPI.transfer(_reg); //register address
  //uint16_t value = (SPI.transfer(0xFF) << 8) | (SPI.transfer(0xFF)); //Since we are concatenating two 8 bit results, we shift first one down 8 bits and then do an OR operation on the second byte
  //Note: the OR operation will be perfomed on the later "half" of the 16 bit value (right 8 bits), effectively copying the digit into it - the zeroes will only be set to 1 if they are one.
  uint16_t value = SPI.transfer16(0xFFFF); //Huh, this exists

  control_mode(false, false);
  return value;
}

boolean old_sine_test(uint16_t frequency) //'n' parameters are on the datasheet pg 66 (there is a newer sine test avaiable now)
{
  if (frequency > 12000 || !control_mode(true, true)) //max frequency is 12000 (check post note)
    return false;

  if (frequency > 0)
  {
    uint8_t n = 0;
    uint8_t freq = SINE_ARRAY_SIZE + 1;
    uint8_t skip_speed;

    for (int i = SINE_ARRAY_SIZE - 1; i >= 0; i--)
    {
      if (frequency < sine_frequencies[i] / 4 && (freq > SINE_ARRAY_SIZE || sine_frequencies[i] < sine_frequencies[freq])) //Sorry for the hacky solution
        freq = i;
    }

    skip_speed = (frequency * 128) / sine_frequencies[freq]; //Note: the compiler automatically truncates (rounds towards 0/down) integer divisions, in my case that's not that noticeable

    n |= (freq << 5) | skip_speed; //'copy' the freq index to bits 7:5 [datasheet]

    sine_start[3] = n;
    SPI.writeBytes(sine_start, SINE_ARRAY_SIZE);
  }
  else
    SPI.writeBytes(sine_end, SINE_ARRAY_SIZE);

  control_mode(false, true);
  return true;
}

boolean sine_test(uint16_t frequency)
{
  //This doesn't work properly as I never figured out how to stop the sine_test without a hard/soft reset
  //+ when this is activated it just beeps
  if (frequency > 0)
  {
    uint16_t packet = (frequency * 65536) / 48000; //max frequency is 24KHz (24000 hz) - check my notes

    if (packet <= 0x8000 && write_reg(SCI_AUDATA, 0xBB80)) //set max sample rate frequency (check my notes) and confirm it's below max frequency (24000), max value is 0x8000
    {
      Serial.print("Sample rate: ");
      Serial.println(read_reg(SCI_AUDATA));

      write_reg(SCI_AICTRL0, packet);
      write_reg(SCI_AICTRL1, packet);
      write_reg(SCI_AIADDR, 0x4020);
    }
    else
      return false;
  }
  else
    write_reg(SCI_AIADDR, 0x0); //reset application start address

  return true;
}


void LoadUserCode(void) {
  int i = 0;

  while (i<sizeof(plugin)/sizeof(plugin[0])) {
    unsigned short addr, n, val;
    addr = plugin[i++];
    n = plugin[i++];
    if (n & 0x8000U) { /* RLE run, replicate n samples */
      n &= 0x7FFF;
      val = plugin[i++];
      while (n--) {
        write_reg(addr, val);
      }
    } else {           /* Copy run, copy n samples */
      while (n--) {
        val = plugin[i++];
        write_reg(addr, val);
      }
    }
  }
}
