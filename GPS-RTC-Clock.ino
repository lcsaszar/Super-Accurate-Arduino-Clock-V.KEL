// Update:  modified for V.KEL VK2828U7G5LF GPS module and Philips RC5 IR transmitter
// Commit Date: 03-June-2025
// RTC always has UTC time and date
// You have to set the GPS serial speed to 57600 with the u-blox u-center software,
// or modify the speed to 9600 in the setup() section

#include <Wire.h> //for RTC comms over I2C
#include <SPI.h>  //for LED output to Max7219 module
#include <RC5.h>  // add library from https://github.com/guyc/RC5

//Nano pins: D10/SS(CS), D11/DIN, D13/CLK = SPI for MAX7219;  D17/A3(SQW), D18/A4(SDA), D19/A5(SCL) = I2C to DS3231 RTC.
//           D0/RX, D1/TX(not used), D2(PPS) = VK2828U7G5FL GPS. D3 = IR receiver interrupt.

//STATE MACHINE SETUP//
  enum { DEBUG, BOOTUP, REG_OPS, TOGGLE_DISPLAY, COUNTER, CHECK_PPS, GPS_INIT, GPS_PPS_SYNC, GPS_NMEA_SYNC} StateMachine;

//PIN DEFS & ADDRESSES//
  const byte RTC_SQW_Pin = 17;  //same as A3/D17 pin (using analog pin as digital for RTC SQW)
  const byte GPS_PPS_Pin = 2;   // for receiving the GPS PPS signal
  const byte irReceivePin = 3;    //Interrupt pin for IR receiver data pin.
  const byte ChipSelectPin = 10;  // For Max7219. We set the SPI Chip Select/Slave Select Pin here. D10 for nano.
  const int RTC_I2C_ADDRESS = 0x68;  // Must be data type [int] to keep wire.h happy. Sets RTC DS3231 i2C address. 

RC5 rc5 = RC5(irReceivePin);

//TIMERS AND EDGE DETECTORS//
  unsigned long GPS_INIT_t0, t0, t1, t2; //for timers
  
  byte RTC_SQW_Current = LOW;   
  byte RTC_SQW_Prev = LOW;
  byte GPS_PPS_Current = HIGH;
  byte GPS_PPS_Prev = HIGH;

//ISR HANDLERS//
  volatile unsigned int pulseChangeTime;  //long or int?
  volatile byte pulseFlag = 0;


//UTC offset handlers//  //ie, Time Zones and Daylight Savings (summer) Time //
  bool UTC_offset_enable = true; // False for UTC time. True for local time.
  
  const char offsetStandardHr = 1; 
  const char offsetStandardMin = 0;
  const char offsetDSTHr = 2; 
  const char offsetDSTMin = 0;

  const byte startDST[4] = {5,0,3,2}; // {nth,day of week,month,hh} use [0..6] for [Sunday...Saturday]
  const byte startStandard[4] = {5,0,10,3}; // {nth,day of week,month,hh} [0..6] for [Sunday...Saturday]
                                 // set n=5 for 5th or last. Assume DST starts/stops at 2am local time
 
  const byte days[] = {0,31,28,31,30,31,30,31,31,30,31,30,31}; // mapping days of the 12 months

  const int currentCentury = 2000;
  
  /* globals that store our offset values*/
  int  offYYYY;
  byte offMO;
  byte offDD;
  byte offHH;
  byte offMM;
  byte offSS;

//RTC date+time holders//
  byte ssRTC, mmRTC, hhRTC, dowRTC, ddRTC, moRTC, ctyRTC, yyRTC; 
  byte countSS, countMM, countHH;

//GPS UTC date+time handlers//
  byte hhGPS, mmGPS, ssGPS, ddGPS, moGPS;
  int yyGPS = currentCentury; 
  
  bool newGPS_dateAvail = false;
  bool newGPS_timeAvail = false;
  bool NMEA_processFlag = false;
//  bool GGA_msg = false;
  bool RMC_msg = false;
  bool msgStart = false;
  
  bool GPS_sec_primed = false;
  bool PPS_done = false;
         
  byte byteIndex = 0;
  byte comma = 1;
  byte dateIndex = 0;

bool counter_enable = true;

/* ----------------------- MAX STUFF ALL HERE ---------------------------vvvvvv
ICSP BLOCK PINOUT
5 3 1
6 4 2

===MAPPING SPI HARDWARE AND ARDUINO'S TO MAX7219 MODULES===

7219 MODULE:  DIN          LOAD(CS)     CLK         (*n/a*)
SPI:          MOSI         SS           SCK          MISO
NANO:         D11          D10          D13          D12
* 
[CS(SS) can actually be any unused PIN on the uController, but these
are the typical conventions.]
*/


// Max7219 digit bitmaps for digit registers 0x01 to 0x08
  
  const byte dp     = 0b10000000; // Decimal Point. Do bitwise OR to combine with any digit to add decimal point

  const byte blank  = 0b00000000;
  const byte hyphen = 0b00000001;
  const byte excl   = 0b10100000;  //exclamation point!
  
    //           0b0abcdefg
  const byte A = 0b01110111;
  const byte B = 0b00011111;
  const byte C = 0b01001110;
  const byte D = 0b00111101;
  const byte E = 0b01001111;
  const byte F = 0b01000111;
  const byte G = 0b01111011;
  const byte H = 0b00110111;
  const byte I = 0b00110000;
  const byte J = 0b00111100;
  const byte L = 0b00001110;
  const byte N = 0b00010101;
  const byte O = 0b00011101;
  const byte P = 0b01100111;
  const byte Q = 0b01110011;  
  const byte R = 0b00000101;
  const byte S = 0b01011011;
  const byte T = 0b00001111;
  const byte U = 0b00011100;
  const byte M = 0b00000000;
  const byte K = 0b00000000;
  const byte V = 0b00000000;
  const byte W = 0b00000000;
  const byte X = 0b00000000;
  const byte Y = 0b00111011;
  const byte Z = 0b01101101;


  byte char_library[28] = {blank,A,B,C,D,E,F,G,H,I,J,K,L,M,N,O,P,Q,R,S,T,U,V,W,X,Y,Z,hyphen};

  //array of digits 0-9 with corresponding segment bit maps
    byte digit[10] = {0b01111110,  //0
                      0b00110000,  //1
                      0b01101101,  //2
                      0b01111001,  //3
                      0b00110011,  //4
                      0b01011011,  //5
                      0b01011111,  //6
                      0b01110000,  //7
                      0b01111111,  //8
                      0b01110011}; //9 

        //            0b0abcdefg
        //      -- a
        //  f  |  |  b
        //    g --
        //   e |  | c
        //    d --
        //0b0abcdefg

// Max7219 Address Registers 
    const byte reg_nonop = 0x00;  // generally not used
    const byte reg_d1 = 0x01; // "Digit0" in the datasheet
    const byte reg_d2 = 0x02; // "Digit1" in the datasheet
    const byte reg_d3 = 0x03; // "Digit2" in the datasheet
    const byte reg_d4 = 0x04; // "Digit3" in the datasheet
    const byte reg_d5 = 0x05; // "Digit4" in the datasheet
    const byte reg_d6 = 0x06; // "Digit5" in the datasheet
    const byte reg_d7 = 0x07; // "Digit6" in the datasheet
    const byte reg_d8 = 0x08; // "Digit7" in the datasheet
    const byte reg_decode = 0x09; // 0x00 no decode, to 0xFF decode all; use a bit map to toggle, ie 0b00000010
    const byte reg_intensity = 0x0A; // min 0x00, max 0x0F... 16 duty-cycle options. 0x07 middle.
    const byte reg_scanlimit = 0x0B; // from 0x00 to 0x07 (sets number of digits being scanned)
    const byte reg_shutdown = 0x0C; // 0x00 shutdown mode, 0x01 normal ops
    const byte reg_displaytest = 0x0F; // 0x00 normal ops, 0x01 display test mode
    
void SPIwrite (byte reg_address, byte regdata) {  
    //Writes 2 bytes to SPI. This is optimized for Max7219 Comms.
    
    digitalWrite(ChipSelectPin,LOW);  // take the CS/SS pin low to select the chip
    SPI.transfer(reg_address);
    SPI.transfer(regdata);
    digitalWrite(ChipSelectPin,HIGH); // take the CS/SS pin high to deselect the chip
  
} //end of SPIwrite

void setAllDigitsTo (byte set_digit) { 
  //this sets all the digits to set_digit
  
  for (byte i=1;i<=8;i++) {
      SPIwrite(i,set_digit);
  } //end for
  
} //end of setAllDigitsTo()


void initializeMax7219() {

    //reset the Max by activating shutdown mode   
    SPIwrite(reg_shutdown,0x00); // 0x00 shutdown, 0x01 normal ops
    //initialize each digit with known values
    setAllDigitsTo(hyphen);
    //set intensity
    SPIwrite(reg_intensity, 0x03); // min 0x00, max 0x0F... 16 duty-cycle options. 0x07 middle.
    //set scan limit
    SPIwrite(reg_scanlimit, 0x07); // from 0x00 to 0x07 (sets number of digits being scanned)
    //set decode
    SPIwrite(reg_decode, 0b00000000); // built-in decode, from 0x00 [all off] to 0xFF [all on] (bit map toggles digits 1-8).
    //flash a display test
    SPIwrite(reg_displaytest, 0x01); // 0x00 normal ops, 0x01 display test mode
    //end the test
    SPIwrite(reg_displaytest, 0x00); // 0x00 normal ops, 0x01 display test mode
    //exit shutdown mode. resume normal ops
    SPIwrite(reg_shutdown,0x01); // 0x00 shutdown, 0x01 normal ops
    
} //end of initializeMax7219

void maxDisplay(byte a, byte b, byte c, byte d, byte e, byte f, byte g, byte h) {
  SPIwrite(8,a);
  SPIwrite(7,b);
  SPIwrite(6,c);
  SPIwrite(5,d);
  SPIwrite(4,e);
  SPIwrite(3,f);
  SPIwrite(2,g);
  SPIwrite(1,h);
}


//**** ISR ROUTINE ****//

void ISR_pulse_detected() {
  pulseChangeTime = micros();
  pulseFlag = 1;
} //end of ISR_pulse_detected()


//**** Philips/Marantz REMOTE ROUTINES  ****//

void processIR(int code) {  // IR code processors
  
  static byte brightness = 0x07;
  
  if (code == 0x0B) {  // Display - displays the date
    displayRTCDate();
    StateMachine = TOGGLE_DISPLAY;
    t1 = millis(); //sets t1 for date display in TOGGLE_DISPLAY state machine
  } //end if

  if (code == 0x2D) {  // Open/Close - toggles UTC vs local time
    UTC_offset_enable = !UTC_offset_enable;
    if (UTC_offset_enable) {
      maxDisplay(L,O,C,A,L,blank,blank,blank);
    } //end if
    else {
      maxDisplay(blank,U,T,C,blank,blank,blank,blank);
    } //end else
    StateMachine = TOGGLE_DISPLAY;
    t1 = millis(); //sets t1 for date display in TOGGLE_DISPLAY state machine  
  } //end if

  if (code == 0x35) {  // Play - starts counter mode  
    countSS = 0;  //we reset all the counter registers to 0
    countMM = 0;
    countHH = 0;
    displayRTC_timeOnMax(countHH,countMM,countSS);
    StateMachine = COUNTER;
  } //end if

  if (code == 0x30) {  // Pause - turns counter timer on and off
    counter_enable = !counter_enable;
  } //end if

  if (code == 0x36) {  // Stop - goes into REG_OPS (used to exit counter mode)
    StateMachine = REG_OPS;
  } //end if

  if (code == 0x2B) {  // AMS - does a 4 second check displaying whether PPS signal is active
    StateMachine = CHECK_PPS;
    maxDisplay(P,P,S,blank,O,F,F,blank); // set this as default display. will be overwritten if PPS is active
    t1 = millis(); //sets t1 for date display in CHECK_PPS state machine
  } //end if

  if ((code == 0x10) && (brightness < 0x0F)) {  // Vol Up - toggles through brightness levels
    delay(250); // IR transmits 3 signals w/ each button press. This delay avoids repeat commands here.    
    brightness++;
    SPIwrite(reg_intensity, brightness); // min 0x00, max 0x0F... 16 duty-cycle options. 0x07 middle.
  }  // end if

  if ((code == 0x11) && (brightness > 0x01)) {  // Vol Up - toggles through brightness levels
    delay(250); // IR transmits 3 signals w/ each button press. This delay avoids repeat commands here.    
    brightness--;
    SPIwrite(reg_intensity, brightness); // min 0x00, max 0x0F... 16 duty-cycle options. 0x07 middle.
  }  // end if
 
  if (code == 0x0F) {   // Recall - display status of local or UTC time
    if (UTC_offset_enable) {
      maxDisplay(L,O,C,A,L,blank,blank,blank);
    } //end if
    else {
      maxDisplay(blank,U,T,C,blank,blank,blank,blank);
    } //end else
    StateMachine = TOGGLE_DISPLAY;
    t1 = millis(); //sets t1 for date display in TOGGLE_DISPLAY state machine  
  } //end if
} //end processIR()



//**** UNIVERSAL FUNCTIONS ****//

byte bcd2dec(byte n) {  
  // Converts binary coded decimal to normal decimal numbers
  return ((n/16 * 10) + (n % 16));
} //end of bcd2dec()

byte dec2bcd(byte n) {  //n must be in range [0..99] incl
  // Converts normal decimal to binary coded decimal. Speed optimized.
  // return ((n / 10 * 16) + (n % 10));  <----- slower method.
  // see https://forum.arduino.cc/index.php?topic=185235.msg1372439#msg1372439
  
  uint16_t a = n;
  byte b = (a*103) >> 10;
  return  n + b*6;
  
} //end of dec2bcd()
    

void clearSerialInputBuffer() {
  while (Serial.available() > 0) {
    Serial.read();
  }//end while
} //end of clearSerialInputBuffer()

 
//****  DISPLAY HANDLERS **** //

void displayRTC_timeOnMax(byte rtc_h, byte rtc_m, byte rtc_s) { //receiving [0-99] decimals from caller

  maxDisplay(digit[(rtc_h/10)%10],digit[rtc_h%10],hyphen,
             digit[(rtc_m/10)%10],digit[rtc_m%10],hyphen,
             digit[(rtc_s/10)%10],digit[rtc_s%10]);
  
} //end of displayRTC_timeOnMax()


//**** GPS HANDLERS **** //

bool PPS_detect() {   // top of the second for ublox 6 GPS (default setting) is rising edge of PPS time pulse
    GPS_PPS_Prev = GPS_PPS_Current;
    GPS_PPS_Current = digitalRead(GPS_PPS_Pin);
    return (GPS_PPS_Prev == LOW && GPS_PPS_Current == HIGH); //returns true if PPS has gone high!
} // end of PPS_detect()

void processNMEA() {
  byte inByte;
  if (Serial.available() > 0) {  //do all of this only if byte ready to read
    inByte = Serial.read();
    byteIndex++;    // we only increment index if we read a byte

// sample RMC message (no fix): $GPRMC,163213.80,V,,,,,,,150625,,*16
// sample RMC message (position fix): $GPRMC,163320.40,A,4731.59662,N,01906.19440,E,1.645,,150625,,*16

    switch (byteIndex) {
      case 1 ... 3: //  $GP characters ignored
        break;
      case 4:
         RMC_msg = (inByte == 'R');
         break;
      case 5:
         RMC_msg = (RMC_msg && (inByte == 'M'));
         break;
      case 6:
         RMC_msg = (RMC_msg && (inByte == 'C'));
         NMEA_processFlag = RMC_msg; // if we have RMC, we keep processing. We don't process GGA message.
         if(NMEA_processFlag)
           {
            comma = 0;   // start counting commas
           }
         break;
      case 7:  // first comma in RMC
        break;
      case 8:   // hour tens
         hhGPS = (inByte - '0')*10;
         break;
      case 9:   //hour units
         hhGPS += (inByte - '0');
         break;
      case 10:  // min tens
         mmGPS = (inByte - '0')*10;
         break;
      case 11:  //min units
         mmGPS += (inByte - '0');
         break;
      case 12:  // sec tens
         ssGPS = (inByte - '0')*10;
         break;
      case 13:  //sec units
         ssGPS += (inByte - '0');  
         //**AT THIS POINT WE HAVE GPS TIME**// CAN ACTUALLY UPDATE RTC!
         newGPS_timeAvail = true;
         break;
    default:
        // comma count for RMC message
        if (comma < 8) 
          {
            if (inByte == ',') comma++;
            break;  
          } else {
            dateIndex++;
          }
        switch (dateIndex){
          case 1:
            if (inByte == ',') // if the first byte after the 8th comma is again comma, we stop processing
              { 
                NMEA_processFlag = false;
                break;
              } else {             // ddmmyy follows in the next 6 bytes
            ddGPS = (inByte - '0')*10;  //day tens
            break;
              }
          case 2:
            ddGPS += (inByte - '0');   //day units
            break;
          case 3:
            moGPS = (inByte - '0')*10;   //mo tens
            break;
          case 4:
            moGPS += (inByte - '0');   //mo units
            break;
          case 5:
            yyGPS = (inByte - '0')*10;   //yy tens, add to currentCentury
            break;
          case 6:
            yyGPS += (inByte - '0');       //yy units
            dateIndex = 0;
            comma = 1;
            newGPS_dateAvail = true;   //all date fields now read
            RMC_msg = false;
            NMEA_processFlag = false;  //and no need to process further
            break;
          default:
            break;  // default dateIndex
        }  // end switch dateIndex
      break;  // default byteIndex
    }//end switch byteIndex
  } //end if Serial available
} //end of processNMEA



//**** UTC TIMEZONE OFFSET AND DST HANDLERS ****//

void offsetAdj(int y, byte mo, byte d, byte h, byte m, char offsetHr, char offsetMin) {
  offMM = (m + offsetMin + 120) % 60;
  if (m + offsetMin > 59) {
    offsetHr ++;
  }
  if (m + offsetMin < 0) {
    offsetHr --;
  }
  offHH = (h + offsetHr + 24) % 24;
  offDD = d;
  offYYYY = y;
  offMO = mo;
  if (offsetHr + h < 0) {
     //Do a decrement
     if (d > 1) {
      offDD--;
     }
     else { //rollover
      offDD = days[((mo+11-1)%12+1)] + (mo==2 && isLeap(y));
      offMO--;
      if (mo == 1) {
        offMO = 12;
        offYYYY = y - 1;
      }//end if
     }
  } //end if for decrement day
  if (offsetHr + h >= 24) {
   // Do an increment
    if (d == (days[mo] + (mo == 2 && isLeap(y)))) { //ie, if d is last day of month
      offDD = 1;
      offMO++;
      if (mo == 12) {
        offMO = 1;
        offYYYY = y + 1;
      }
    }//end if
    else {
      offDD = d+1;
    }
  } //end if increment day
} //end offsetAdj

bool isLeap (byte y) {
  return ((y%4==0) || (!(y%100==0) &&  (y%400==0)));
} //end isLeap()

byte dowDate(int y, byte n, byte dow_target, byte m) { // returns date number in month of nth day of week in month
  char temp = 1; // char because could be negative. Set at 1 for 1st of month.
  byte maxDays = days[m]; // number of days in given month
  if (m==2) {
    maxDays = days[m] + isLeap(y); 
  }
  byte startDOW = dow(y,m,1);  //gets dow of 1st of given month 
  temp += dow_target - dow(y,m,1);
  if (temp < 0) {
    temp += 7;
  } //end if
  temp += 7*(n-1);
  if (temp > maxDays) {
    temp -= 7;
  }
  return temp;
} //end dowDate()
                    
byte dow(int y, byte m, byte d) { //pass non-leading zero values
  static byte t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
  y -= m < 3;
  return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
  // returns [0..6] for [Sunday...Saturday]
} //end of dow()
  
void getLocalTime(int y, byte mo, byte d, byte h, byte m) {
  // **this function sets offMO, offDD, offYYYY, offHH, offMM to local time**
  // we first get a baseline offset
  // NOTE: int y is being passed as RTC's 2-digit date.
  
  if (yyGPS == currentCentury) {   // Converting RTC 2 digit date to 4 digit:
    y += currentCentury;  // takes our 2-digit year and converts to 4 digit
  }
  else { // or if GPS signal available, we just use that
    y = yyGPS; // uses GPS 4-digit year if available
  }
  
  offsetAdj(y,mo,d,h,m,offsetStandardHr,offsetStandardMin);

  // next we do our DST checks, and recalc offset if DST is in effect
 
  if (offMO > startDST[2] && offMO < startStandard[2]) {
      offsetAdj(y,mo,d,h,m,offsetDSTHr,offsetDSTMin);
  } //end if
  else if (offMO == startDST[2]) {
      byte dowDateDST = dowDate(y,startDST[0],startDST[1],startDST[2]);
      if (offDD > dowDateDST) {
        offsetAdj(y,mo,d,h,m,offsetDSTHr,offsetDSTMin);
      } //end if
      else if (offDD == dowDateDST && offHH >= startDST[3]) {
        offsetAdj(y,mo,d,h,m,offsetDSTHr,offsetDSTMin);
      } //end else if
  } //end else if
  else if (offMO == startStandard[2]) {
      byte dowDateStd = dowDate(y,startStandard[0],startStandard[1],startStandard[2]);
      if (offDD < dowDateStd) {
        offsetAdj(y,mo,d,h,m,offsetDSTHr,offsetDSTMin);
      }// end if
      else if (offDD == dowDateStd && offHH < (startStandard[3]-1)) {
        offsetAdj(y,mo,d,h,m,offsetDSTHr,offsetDSTMin);  
      }// end else if
  } //end else if
} //end getLocalTime()


//**** RTC HANDLERS ****//

void getRTC() { //reads all current time/date data from DS3231 chip via I2C
  // ssRTC, mmRTC, hhRTC, dowRTC, ddRTC, moRTC, ctyRTC, yyRTC;

  byte temp_buffer;

  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write(0x00);   //set register points to address 00h on DS3231
  Wire.endTransmission();
  Wire.requestFrom(RTC_I2C_ADDRESS, 7); // need 7 reads to clear this.
  ssRTC = bcd2dec(Wire.read()); // read reg 0  [range 00-59]
  mmRTC = bcd2dec(Wire.read()); // read reg 1 [range 00-59]
  hhRTC = bcd2dec(Wire.read() & 0b00111111); // read reg 2 and mask out BITS 7-8 
  dowRTC = bcd2dec(Wire.read()); // read reg 3 [range 1-7]
  ddRTC = bcd2dec(Wire.read());  // read reg 4 [range 01-31]
  temp_buffer = bcd2dec(Wire.read()); // read reg 5
  moRTC = bcd2dec(temp_buffer & 0b00011111); 
  ctyRTC = temp_buffer >> 7;
  yyRTC = bcd2dec(Wire.read()); //read reg 6 [range 00-99]
  
} //end of getRTC()

bool RTC_detect() { //Detects DS3231 SQW falling edge 
  RTC_SQW_Prev = RTC_SQW_Current;
  RTC_SQW_Current = digitalRead(RTC_SQW_Pin);
  return (RTC_SQW_Prev == HIGH && RTC_SQW_Current == LOW);
} //end of detect_RTC

void displayRTCDate() {   // adjusts to local date if flag set
  getRTC(); // updates ssRTC, mmRTC, hhRTC, dowRTC, ddRTC, moRTC, ctyRTC, yyRTC;
  if (UTC_offset_enable == false) {  // if flag false, we do UTC time and date
    displayRTC_timeOnMax(ddRTC,moRTC,yyRTC);  // displays UTC date
  } //end if
  else {                             // else requests local offset time
    getLocalTime(yyRTC,moRTC,ddRTC,hhRTC,mmRTC); // updates offDD,offMO,offYYYY
    displayRTC_timeOnMax(offDD,offMO,offYYYY % 100);
  }
} //end displayRTCDate


void countUp() {    

  RTC_SQW_Prev = RTC_SQW_Current;
  RTC_SQW_Current = digitalRead(RTC_SQW_Pin);
  if (counter_enable && RTC_SQW_Prev == HIGH && RTC_SQW_Current == LOW) { //tests for falling edge
    
    countSS++;
    
    if (countSS == 60) {
      countSS = 0;
      countMM++;
    } //end if
    if (countMM == 60) {
      countMM = 0;
      countHH++;
    } //end if
    if (countHH == 100) {
      countHH = 0;
    }
    
    displayRTC_timeOnMax(countHH,countMM,countSS);
    
  } //end if 
} //end of countUp

void displayRTC() { //updates display if new RTC time. Detects DS3231 SQW falling edge then trigger display of time update
  
  RTC_SQW_Prev = RTC_SQW_Current;
  RTC_SQW_Current = digitalRead(RTC_SQW_Pin);
      
  if (RTC_SQW_Prev == HIGH && RTC_SQW_Current == LOW) { //test for falling edge
    getRTC(); // updates ssRTC, mmRTC, hhRTC, dowRTC, ddRTC, moRTC, ctyRTC, yyRTC;
    if (UTC_offset_enable == false) {  // flag requests UTC time
      displayRTC_timeOnMax(hhRTC,mmRTC,ssRTC);
    } //end if
    else {                             // flag requests local offset time
      getLocalTime(yyRTC,moRTC,ddRTC,hhRTC,mmRTC); 
      displayRTC_timeOnMax(offHH,offMM,ssRTC);
    } //end else
  } //end if
} // end of displayRTC()

void displayRTC_now() { //immediate retrieve and display of RTC time registers
  getRTC(); // updates ssRTC, mmRTC, hhRTC, dowRTC, ddRTC, moRTC, ctyRTC, yyRTC;
  displayRTC_timeOnMax(hhRTC,mmRTC,ssRTC);
} // end of displayRTC()

void sendRTC(byte reg_addr, byte byte_data) {
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write(reg_addr);   //set register pointer to address on DS3231
  Wire.write(byte_data);
  Wire.endTransmission();
} //end of sendRTC()

byte getSingleRTC(byte reg_addr) {    //returns a single raw byte from the provided address register
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write(reg_addr);   //set to reg address on DS3231
  Wire.endTransmission();
  Wire.requestFrom(RTC_I2C_ADDRESS, 1);
  return (Wire.read());
} //end of getRTC_BCD()

void setRTC_Time(byte hh, byte mm, byte ss) { //must be [0-99]
  //  example use: setRTC_Time(23,49,50);     //hh,mm,ss

  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write(0x00);   //set register pointer to address on DS3231
  Wire.write(dec2bcd(ss)); //set seconds
  Wire.write(dec2bcd(mm)); //set minutes
  Wire.write(dec2bcd(hh)); //set hours. Bit 6 low keeps at 24hr mode. So can leave as is.  
  Wire.endTransmission();
} //end of setRTC_Time()

void setRTC_Date(int yyyy, byte mo, byte dd) { 
  Wire.beginTransmission(RTC_I2C_ADDRESS);
  Wire.write(0x04);   //set register pointer to address on DS3231
  Wire.write(dec2bcd(dd)); //set date. sending the last 2 digits only.
  Wire.write(dec2bcd(mo)); //set month. ignore century as don't have any use for that.
  Wire.write(dec2bcd(yyyy % 100)); //set year. sending the 2 LS digits only.  
  Wire.endTransmission();
} //end of setRTC_Date()

//****  STATE MACHINE **** //

void RunStateMachine() {
  byte temp_buffer;
  unsigned char toggle;
  unsigned char address;
  unsigned char command;
  
  switch (StateMachine) {

    case DEBUG: //remember, it's looping!

        if (RTC_detect()) {
          t1=micros();
        }
        if (PPS_detect()) {
          t2=micros();
        }
        break; //end DEBUG case

        
 // ------------------------------    

    case BOOTUP:
    
        sendRTC(0x0E,0x00); // enables the 1Hz pulse on RTC DS3231's SQW pin
        delay(50);
        temp_buffer = getSingleRTC(0x02); //get hour byte from addr 0x02. Bit 6: LOW (0) = 24 hr mode. HIGH (1) = 12 hr.
        if ((temp_buffer & 0b01000000) != 0) {   //if Bit 6 is HIGH, i.e., if 24 hour time is *not* set, then...
           getRTC(); // grab time 
           if (mmRTC > 58 && ssRTC > 58) { // checking to make sure not near an hours rollover. 
              break;  //keep breaking until we roll over the seconds
           } //end if
           else {
            sendRTC(0x02,temp_buffer ^ 0b01000000); //set BIT 6 low to enable 24 hr time
           } //end else
        } //end if
   
        //StateMachine = DEBUG; // >>>> State Change! <<<<//
        StateMachine = REG_OPS; // >>>> State Change! <<<<//
        t0=micros();
        break;
     //end BOOTUP case

 // ------------------------------    
        
    case REG_OPS:
   
        displayRTC();  //keep the trains running
        
        if (ssRTC == 00) {  //every minute at second 00, do a GPS_INIT check to prep for GPS-to-RTC time update
          GPS_INIT_t0 = millis(); //sets our timer to allow a timeout in the next state
          StateMachine = GPS_INIT; // >>>> State Change! <<<<//
          break;
        } //end if
        if (pulseFlag == 1) {
          detachInterrupt(digitalPinToInterrupt(irReceivePin)); //stop interrupt while we process
          if (rc5.read(&toggle, &address, &command))
            {
              processIR(command);
            }
          attachInterrupt(digitalPinToInterrupt(irReceivePin), ISR_pulse_detected, CHANGE);
        } // end if
        break;
        //end REG_OPS case

 // ------------------------------    

    case TOGGLE_DISPLAY:  //holds the display before resuming regular ops
    
        if (millis() - t1 < 3500) {
           break;
        }
        else {
           StateMachine = REG_OPS;
           break;
        }
          
        //end TOGGLE_DISPLAY case

 // ------------------------------    

    case COUNTER:  //runs the counter
    
        countUp();
        if (pulseFlag == 1) {
          detachInterrupt(digitalPinToInterrupt(irReceivePin)); //stop interrupt while we process
          if (rc5.read(&toggle, &address, &command))
            {
              processIR(command);
            }
          attachInterrupt(digitalPinToInterrupt(irReceivePin), ISR_pulse_detected, CHANGE);
        } // end if
        break;
          
 // ------------------------------    

    case CHECK_PPS:  //displays whether a PPS pulse is coming from GPS
  
        if (millis() - t1 < 4000) {
           if (PPS_detect()) {
              maxDisplay(P,P,S,blank,O,N,blank,blank);
            }
           break;
        }
        else {
           StateMachine = REG_OPS;
           break;
        }
          
        //end TOGGLE_DISPLAY case

 // ------------------------------   
        
    case GPS_INIT:
    
        displayRTC();  // keeps the trains running on the display!
     
       if (PPS_detect()) {  //means PPS is detected on the GPS unit
         StateMachine = GPS_PPS_SYNC; // >>>> State Change! <<<<//
         clearSerialInputBuffer(); //purges old GPS data in serial buffer 
         break;
       } //end if
        
        if (millis() - GPS_INIT_t0 > 4000) {  //timeout this state after 4 secs if no PPS detected
          StateMachine = GPS_NMEA_SYNC; // >>>> State Change! <<<<//
          clearSerialInputBuffer(); //purges old GPS data in serial buffer
          break; 
        } //end if

        break;
        //end GPS_INIT case

 // ------------------------------   
    case GPS_PPS_SYNC: 
         
         byte ssGPS_incr;

         displayRTC(); //keep the trains running while reading serial input!
       
         if (newGPS_timeAvail && !GPS_sec_primed) { //here we prime the GPS seconds by incrementing 1s as we wait for a PPS signal
            ssGPS_incr = (ssGPS + 1) % 60;
            GPS_sec_primed = true;
            newGPS_timeAvail = false; // set this false to force another read post PPS. 
          } //end if

        if (PPS_detect() && GPS_sec_primed) {  // if PPS detected send only the primed second to RTC
           sendRTC(0x00,dec2bcd(ssGPS_incr));  // here is where we would add delay for insanity mode
           PPS_done = true;
        } //end if
        
        if (PPS_done && GPS_sec_primed && newGPS_timeAvail) { // send minutes and hours of next NMEA time read after PPS
          sendRTC(0x01,dec2bcd(mmGPS));
          sendRTC(0x02,dec2bcd(hhGPS));
          GPS_sec_primed = false;
          newGPS_timeAvail = false;
        } //end if
        
        if (newGPS_dateAvail && PPS_done) {
          setRTC_Date(yyGPS,moGPS,ddGPS);
          newGPS_dateAvail = false; //this completed the date and time update.
          PPS_done = false;
          StateMachine = REG_OPS; // >>>> State Change! <<<<//
          break;
        } //end if

        //need a timeout where after 30 seconds of no updates it goes back to REG_OPS
        
        if (NMEA_processFlag) {
           processNMEA();
        } //end if
        else {
          if (Serial.available() > 0) {
            if (Serial.read() == '$') {  //start of NMEA message
                NMEA_processFlag = true;  //we only want to start processing at beginning of message
//                GGA_msg = false;
                RMC_msg = false;
                byteIndex = 1;
            } //end if
          } //end if
        } //end else

        if (millis() - GPS_INIT_t0 > 14000) {  //timeout this state after 10 secs if no PPS detected
            StateMachine = REG_OPS; // >>>> State Change! <<<<//
        }
       
        break;
        
  //end GPS_PPS_SYNC case

 // ------------------------------    

    case GPS_NMEA_SYNC:
    
        displayRTC(); //keep the trains running while reading serial input!
        
        if (newGPS_timeAvail) {
          setRTC_Time(hhGPS,mmGPS,ssGPS);
          newGPS_timeAvail = false;
        } //end if

        if (newGPS_dateAvail) {
          setRTC_Date(yyGPS,moGPS,ddGPS);
          newGPS_dateAvail = false; //this completed the date and time update.
          StateMachine = REG_OPS; // >>>> State Change! <<<<//
          break;
        } //end if

        if (NMEA_processFlag) {
           processNMEA();
        } //end if
        else {
          if (Serial.available() > 0) {
            if (Serial.read() == '$') {  //start of NMEA message
                NMEA_processFlag = true;
//                GGA_msg = false;
                RMC_msg = false;
                byteIndex = 1;
            } //end if
          } //end if
        } //end else

        if (millis() - GPS_INIT_t0 > 14000) {  //timeout this state after 10 secs if no PPS detected
            StateMachine = REG_OPS; // >>>> State Change! <<<<//
        }
        
        break;
        //end GPS_NMEA_SYNC case
     
  } //end switch StateMachine
} //end of RunStateMachine()


void setup() {
  // put your setup code here, to run once:

  delay(1000); //give system time to stabilize

  //start up SPI for Max7219
      SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0)); //per Max7219 Datasheet, 10MHz, MSB
      pinMode(ChipSelectPin, OUTPUT);  //sets the output pin for SS/CS
      digitalWrite(ChipSelectPin,HIGH); // take the CS/SS pin high to deselect the chip
      SPI.begin(); //initialize the SPI bus using our settings from above
      
  initializeMax7219();  //sets all startup parameters for Max7219 chip

  delay(2000);
  
  pinMode(RTC_SQW_Pin, INPUT_PULLUP); // SQW is open drain on DS3231, so need internal pullup enabled.
  pinMode(GPS_PPS_Pin, INPUT);
  pinMode(irReceivePin, INPUT_PULLUP);
  
  Wire.setClock(400000);  //i2C 100kHz typical. 400kHz fast mode. DS3231 RTC supports fast mode.
  delay(100); //more stabilizing
  
  Serial.begin(57600);  // for the GPS unit
  delay(100); //more stabilizing

  pinMode(4, INPUT);
  pinMode(5, OUTPUT);
  
  attachInterrupt (digitalPinToInterrupt(irReceivePin), ISR_pulse_detected, CHANGE); 
  
  StateMachine = BOOTUP;  //set the initial state
}

void loop() {
  // put your main code here, to run repeatedly:
  RunStateMachine();
}
