/*
This code is based on the project found at "https://github.com/bvenneker/Arduino-GCode-Sender", 
by the user Bart Venneker. You can go to the link to see the original project. 
Thanks to him for sharing the code under a free license.
Origanl Version 15-10-2017.002

This code was updated by Carlos Guerrero.
03/05/2020
v1.0

Hardware
Arduino MEGA -> UNO
- Serial1 (pins 18 and 19) communicates with arduino with grbl shield -> 8 and 9

GCode Sender.
Hardware: Arduino Mega,
         ,rotary encoder
         ,sd card reader with SPI interface
         ,a general 4x20 LCD display with i2c interface

Limitations: It does not support directories on the SD card, only files in the root directory are supported. Only support GRBL v1.1
      
(Micro) SD card attached to SPI bus as follows:
CS - pin 53 -> 4
MOSI - pin 51 -> 11
MISO - pin 50 -> 12
CLK - pin 52 -> 13

Rotary Enconder attached af follows:
CLK - pin 2
DT - pin 3
SW - pin 5

LCD Display
the LCD display is connected to pins 20 (SDA) and 21 (SCL) (default i2c connections) -> A4 and A5
*/

#include <LiquidCrystal_I2C.h> // by Frank de Brabander, available in the arduino library manager
#include <Wire.h> 
#include <SPI.h>
#include <SD.h>
#include <Encoder.h>  // by Paul Stoffregen, available in the arduino library manager

#define SD_card_Reader   4 
#define clkPin            2
#define dtPin             3
#define selectPin         5 

#define ENCODER_USE_INTERRUPTS

// Display
LiquidCrystal_I2C lcd(0x3F, 16, 3);  // set the LCD address to 0x27 for a 20 chars and 4 line display

//Encoder
Encoder myEnc(clkPin, dtPin);

// Globals
char WposX[9];            // last known X pos on workpiece, space for 9 characters ( -999.999\0 )
char WposY[9];            // last known Y pos on workpiece
char WposZ[9];            // last known Z heighton workpiece, space for 8 characters is enough( -99.999\0 )

char machineStatus[10];   // last know state (Idle, Run, Hold, Door, Home, Alarm, Check)
bool awaitingOK = false;   // this is se true when we are waiting for the ok signal from the grbl board (see the sendCodeLine() void)
bool homing = false;

unsigned long runningTime, lastUpdateMenu, timeExit = 5000;

unsigned int numLineTotal = 0, numLineAct = 0;

long oldPosition  = 0;

byte timeDelay = 150, optionSelectLast = 0;

void setup() {
  // display 
  //EEPROM.update(0, 5);
  lcd.init();                      // initialize the lcd 
  lcd.backlight();
  lcd.begin(20, 4);

  // inputs (write high to enable pullup)
  pinMode(clkPin,INPUT_PULLUP); 
  pinMode(dtPin,INPUT_PULLUP);
  pinMode(selectPin,INPUT_PULLUP); 
   
  // Ask to connect (you might still use a computer and NOT connect this controller)
  setTextDisplay("",F("   Connect to CNC?    "),"","");
  while (digitalRead(selectPin)) {}  // wait for the button to be pressed
  delay(50);
  while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
  delay(50);
  //Serial1 connections;
  Serial.begin(115200); 
  sendCodeLine(F("$10=0"),false); // Status report: Enable WPos and Disable MPos
  lcd.clear();
}

byte fileMenu() {
  /*
  This is the file menu.
  You can browse up and down to select a file.
  Click the button to select a file

  Move the stick right to exit the file menu and enter the Move menu
  */
  bool readySD = true;
  if(!SD.begin(SD_card_Reader)){
    readySD = false;
    setTextDisplay(F("Error"),F("SD Card Fail!")," ","=>Refresh");
    unsigned long timeWithOutPress = millis();
    
    while(millis() - timeWithOutPress <= timeExit){
      if (digitalRead(selectPin)==LOW) {    // Pushed it!           
        delay(50);
        while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
        delay(50);
        timeWithOutPress = millis();
        if(SD.begin(SD_card_Reader)){
          readySD = true;
        }
      }
    }
  }
  if(readySD){
    byte fileindex=1;
    String fn; 
    byte fc = filecount();
  
    fn= getFileName(fileindex);
    setTextDisplay(F("Files ")," -> " + (String)fn," ",F("Click to select"));
    unsigned long timeWithOutPress = millis();
    
    while(millis() - timeWithOutPress <= timeExit){   
      long newPosition = myEnc.read();
      byte diferencia = abs(newPosition - oldPosition);      
      if (fileindex < fc && newPosition > oldPosition && diferencia != 3) { // down!  
          fileindex++;
          timeWithOutPress = millis();
          fn= getFileName(fileindex);
          lcd.setCursor(0, 1);
          lcd.print(F(" -> ")); lcd.print(fn);
          for (int u= fn.length() + 4; u < 20; u++){ lcd.print(" ");}     
        }
        
      if (newPosition < oldPosition && diferencia != 3) { // up!
        if (fileindex > 1) {      
          fileindex--;
          timeWithOutPress = millis();
          fn="";
          fn= getFileName(fileindex);
          lcd.setCursor(0, 1);
          lcd.print(F(" -> ")); lcd.print(fn);
          for (int u= fn.length() + 4; u < 20; u++){ lcd.print(" ");}
        }
      }  
      if (fileindex > 0 && digitalRead(selectPin)==LOW && fn!="") {    // Pushed it!           
         setTextDisplay(F("Send this file? ")," -> " + fn,"",F("Click to confirm"));  // Ask for confirmation
         delay(50);
         while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
         delay(50);
  
         unsigned long t = millis();
         while (millis()-t <= 2000UL) {
           if (digitalRead(selectPin)==LOW) {  // Press the button again to confirm
             delay(10);
             while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
             return fileindex;  
             break;
             }
          }
          timeWithOutPress = millis();
          setTextDisplay(F("Files ")," -> " + fn,"",F("Click to select"));
       }  
       if(newPosition != oldPosition) {
        oldPosition = newPosition;     
        delay(timeDelay);
      }
    }
  }
  return 0;
}
  
void moveMenu(char axis, float distance){
  lcd.clear();
  String MoveCommand;
  String InitialCommand = "$J=G21G91";
  String SpeedCommand = "F1000";
  unsigned long startTime,lastUpdate;

  clearRXBuffer();
  
  lcd.setCursor(0, 3); lcd.print("Move ");
  lcd.print(axis);
  lcd.print(" Axis ");
  lcd.print(distance/10,3);
  lcd.print("mm");
  
  while (MoveCommand!="-1") { 
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    MoveCommand="";
    // read the state of all inputs
        
    if (newPosition > oldPosition && diferencia != 3) MoveCommand=InitialCommand + axis + distance/10 + SpeedCommand;    
    else if (newPosition < oldPosition && diferencia != 3) MoveCommand=InitialCommand + axis + (-distance/10) + SpeedCommand;     
 
    if (MoveCommand!="") {
      // send the commands        
      sendCodeLine(MoveCommand,true);            
      MoveCommand="";       
    }
    
    if (MoveCommand=="") startTime = millis();
    // get the status of the machine and monitor the receive buffer for OK signals
    
    if (millis() - lastUpdate >= 250) {
      getStatus();
      lastUpdate=millis(); 
      updateDisplayStatus(2);  
    }
    
    if (digitalRead(selectPin)==LOW) { // button is pushed, exit the move loop   
      lcd.clear();
      MoveCommand=F("-1");      
      while (digitalRead(selectPin)==LOW) {}; // wait until the user releases the button
      delay(10);
    } 
    if(newPosition != oldPosition) {
      oldPosition = newPosition;     
      delay(timeDelay);
    }
  }
  sendCodeLine(F("G21"),true);
  sendCodeLine(F("G90"),true); // Switch to relative coordinates
}
  
String getFileName(byte i){
  /*
    Returns a filename.
    if i = 1 it returns the first file
    if i = 2 it returns the second file name
    if i = 3 ... see what I did here?
  */
  byte x = 0;
  String result;
  File root = SD.open("/");    
  while (result=="") {
    File entry =  root.openNextFile();
    if (!entry) {    
        // noting         
        } else {
          if (!entry.isDirectory()) {
            x++;
            if (x==i) result=entry.name();                         
          }
          entry.close(); 
        }
  }
  root.close();  
  return result;
}

byte filecount(){
  /*
    Count the number of files on the SD card.
  */
  
  byte c =0;
   File root = SD.open("/"); 
   while (true) {
    File entry =  root.openNextFile();
    if (! entry) {
      root.rewindDirectory(); 
      root.close(); 
      return c; 
      break;} else  {
         if (!entry.isDirectory()) c++;    
         entry.close();
        }  
   }
}

void setTextDisplay(String line1, String line2, String line3, String line4){
  /*
   This writes text to the display
  */        
  if(line1 != ""){
    lcd.setCursor(0, 0);
    lcd.print(line1);
    for (int u= line1.length() ; u < 20; u++){ lcd.print(" ");}
  }
  if(line2 != ""){
    lcd.setCursor(0, 1);
    lcd.print(line2);
    for (int u= line2.length() ; u < 20; u++){ lcd.print(" ");}
  }
  if(line3 != ""){
    lcd.setCursor(0, 2);
    lcd.print(line3);
    for (int u= line3.length() ; u < 20; u++){ lcd.print(" ");}
  }
  if(line4 != ""){
    lcd.setCursor(0, 3);
    lcd.print(line4);
    for (int u= line4.length() ; u < 20; u++){ lcd.print(" ");} 
  }      
}

void sendFile(byte fileIndex){/*
  This procedure sends the cgode to the grbl shield, line for line, waiting for the ok signal after each line

  It also queries the machine status every 500 milliseconds and writes some status information on the display
  */
  String strLine="";
 
  File dataFile;
  
  unsigned long lastUpdate;
  
  String filename;
  filename= getFileName(fileIndex);
  dataFile = SD.open(filename);
  if (!dataFile) {
    setTextDisplay(F("File"),"", F("Error, file not found"),"");
    delay(1000); // show the error
    return;
  }

  setTextDisplay(F("Loading..."),"","",filename);

   // Set the Work Position to zero
  sendCodeLine(F("G90"),true); // absolute coordinates
  sendCodeLine(F("G21"),true);
  //sendCodeLine(F("G92 X0 Y0 Z0"),true);  // set zero
  clearRXBuffer();
    
  // reset the timer
  runningTime = millis();

  // Read the file and count the total line
   
  while ( dataFile.available() ) {     
    strLine = dataFile.readStringUntil('\n'); 
    strLine = ignoreUnsupportedCommands(strLine);
    if (strLine !="") numLineTotal++;
  }
   
  dataFile = SD.open(filename);
  // Read the file and send it to the machine
  while ( dataFile.available() ) {
    if (!awaitingOK) { 
      // If we are not waiting for OK, send the next line      
      strLine = dataFile.readStringUntil('\n'); 
      strLine = ignoreUnsupportedCommands(strLine);
      if (strLine !="") {sendCodeLine(strLine,true); numLineAct++;}    // sending it!
    }

    // get the status of the machine and monitor the receive buffer for OK signals
    if (millis() - lastUpdate >= 250) {   
        lastUpdate=millis();  
        float complete = (float(numLineAct)/float(numLineTotal))*100.0;
        lcd.setCursor(14, 0);
        lcd.print(" ");
        lcd.print(complete,0) ;lcd.print(F("%"));
        updateDisplayStatus(runningTime);          
    }   
  }
  
  
  /* 
   End of File!
   All Gcode lines have been send but the machine may still be processing them
   So we query the status until it goes Idle
  */
  
  while (strcmp (machineStatus,"Idle") != 0) {
    delay(250);
      getStatus();
      updateDisplayStatus(runningTime);     
  }
   // Now it is done.         

   setTextDisplay(F("               100%"),F("   Completed")," ","");
   
   numLineTotal = 0, numLineAct = 0;
   while (digitalRead(selectPin)==HIGH) {} // Wait for the button to be pressed
   delay(50);
   while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
   delay(50);
   dataFile.close();
   resetSDReader();
}

void updateDisplayStatus(unsigned long runtime){
  /*
   I had some issues with updating the display while carving a file
   I created this extra void, just to update the display while carving.
  */

  unsigned long t = millis() - runtime;
  int H,M,S;
  char timeString[9];
  char p[3];
  
  t=t/1000;
  // Now t is the a number of seconds.. we must convert that to "hh:mm:ss"
  H = floor(t/3600);
  t = t - (H * 3600);
  M = floor(t/60);
  S = t - (M * 60);

  sprintf (timeString,"%02d:%02d:%02d",H,M,S);
  timeString[8]= '\0';
  getStatus();

  lcd.setCursor(0, 0);
  lcd.print(machineStatus);
  lcd.print(" ");
  if(runtime == 1){
    lcd.setCursor(0, 3);
    lcd.print("Status Machine Wpos"); 
  }
  else if(runtime > 3){ 
    lcd.print(timeString);
    lcd.print("  ");
  }

  lcd.setCursor(0, 1);
  lcd.print("X:");  lcd.print(WposX);lcd.print("  ");
  
  lcd.setCursor(11, 1);
  lcd.print("Y:");  lcd.print(WposY);
  if(strlen(WposY) == 6)lcd.print(" ");
  else if(strlen(WposY) == 5) lcd.print("  ");
  
  lcd.setCursor(5, 2);
  lcd.print("Z:");  lcd.print(WposZ);lcd.print("  ");
}

void resetSDReader() {
  /* 
   This next SD.begin is to fix a problem, I do not like it but there you go.
   Without this sd.begin, I could not open anymore files after the first run.

   To make this work I have changed the SD library a bit (just added one line of code)
   I added root.close() to SD.cpp
   as explained here http://forum.arduino.cc/index.php?topic=66415.0
  */
  
   while (!SD.begin(SD_card_Reader)) {    
     setTextDisplay(F("Error"),F("SD Card Fail!"),"","");
     delay(2000); 
     }
}

void sendCodeLine(String lineOfCode, bool waitForOk ){
  /*
    This void sends a line of code to the grbl shield, the grbl shield will respond with 'ok'
    but the response may take a while (depends on the command).
    So we immediately check for a response, if we get it, great!
    if not, we set the awaitingOK variable to true, this tells the sendfile() to stop sending code
    We continue to monitor the rx buffer for the 'ok' signal in the getStatus() procedure.
  */
  int updateScreen =0 ;
  Serial.println(lineOfCode);
  awaitingOK = true;  
  // delay(10);
  checkForOk();  
  
  while (waitForOk && awaitingOK) {
    delay(50);
    // this may take long, so still update the timer on screen every second or so
    if (updateScreen++ > 4) {
      updateScreen=0;
      updateDisplayStatus(runningTime);
    }
    checkForOk();      
    }
}
  
void clearRXBuffer(){
  /*
  Just a small void to clear the RX buffer.
  */
  char v;
    while (Serial.available()) {
      v=Serial.read();
      delay(3);
    }
  }
  
String ignoreUnsupportedCommands(String lineOfCode){
  /*
  Remove unsupported codes, either because they are unsupported by GRBL or because I choose to.  
  */
  removeIfExists(lineOfCode,F("G64"));   // Unsupported: G64 Constant velocity mode 
  removeIfExists(lineOfCode,F("G40"));   // unsupported: G40 Tool radius comp off 
  removeIfExists(lineOfCode,F("G41"));   // unsupported: G41 Tool radius compensation left
  removeIfExists(lineOfCode,F("G81"));   // unsupported: G81 Canned drilling cycle 
  removeIfExists(lineOfCode,F("G83"));   // unsupported: G83 Deep hole drilling canned cycle 
  removeIfExists(lineOfCode,F("M6"));    // ignore Tool change
  removeIfExists(lineOfCode,F("M7"));    // ignore coolant control
  removeIfExists(lineOfCode,F("M8"));    // ignore coolant control
  removeIfExists(lineOfCode,F("M9"));    // ignore coolant control
  removeIfExists(lineOfCode,F("M10"));   // ignore vacuum, pallet clamp
  removeIfExists(lineOfCode,F("M11"));   // ignore vacuum, pallet clamp
  removeIfExists(lineOfCode,F("M5"));    // ignore spindle off
  lineOfCode.replace(F("M2 "),"M5 M2 "); // Shut down spindle on program end.
  
  // Ignore comment lines 
  // Ignore tool commands, I do not support tool changers
  if (lineOfCode.startsWith("(") || lineOfCode.startsWith("T") ) lineOfCode="";  
  lineOfCode.trim();  
  return lineOfCode;
}

String removeIfExists(String lineOfCode,String toBeRemoved ){
  if (lineOfCode.indexOf(toBeRemoved) >= 0 ) lineOfCode.replace(toBeRemoved," ");
  return lineOfCode;
}

void checkForOk(){
  // read the receive buffer (if anything to read)
  char c,lastc;
  bool error5 = false;
  c=64;
  lastc=64;
   while (Serial.available()) {
    c = Serial.read();  
    if (lastc==':' && c=='5') error5=true; 
    else if (lastc=='o' && c=='k') {
      awaitingOK=false; 
    }
    lastc=c;
    delay(3);             
    }   
    if(error5){
      setTextDisplay(F("error:5"),F("Homing cycle failure"),F("It's not enabled"),F("via settings ($22)"));
      delay(5000);
    }
}

void getStatus(){
  /*
    This gets the status of the machine
    The status message of the machine might look something like this (this is a worst scenario message)
    The max length of the message is 72 characters long (including carriage return).
    <Idle|WPos:0.000,0.000,0.000|FS:0,0>   
  */
  
  char content[80];
  char character;
  byte index=0;
  bool completeMessage=false;
  int i=0;
  int c=0;
  checkForOk();

  Serial.print(F("?"));  // Ask the machine status
  unsigned long times = millis();
  while (Serial.available() == 0) {
    if(homing) setTextDisplay(F("       Homing"),F("       Cycle")," ",F("   Please Wait..."));  
  }  // Wait for response 
  while (Serial.available()) {
    character=Serial.read();  
    content[index] = character;   
    if (content[index] =='>') completeMessage=true; // a simple check to see if the message is complete
    if (index>0) {
      if (content[index]=='k' && content[index-1]=='o') {
        awaitingOK=false; 
      }
    }
    index++;
    delay(1); 
  }

  if (!completeMessage) { return; }   
  i++;
  while (c<9 && content[i] !='|') {machineStatus[c++]=content[i++]; machineStatus[c]=0; } // get the machine status
  while (content[i++] != ':') ; // skip until the first ':'
  c=0;
  while (c<8 && content[i] !=',') { WposX[c++]=content[i++]; WposX[c] = 0;} // get WposX
  c=0; i++;
  while (c<8 && content[i] !=',') { WposY[c++]=content[i++]; WposY[c] = 0;} // get WposY
  c=0; i++;
  while (c<8 && content[i] !='|') { WposZ[c++]=content[i++]; WposZ[c] = 0;} // get WposZ
  if (WposZ[0]=='-')   
   { WposZ[5]='0';WposZ[6]=0;}
  else 
   { WposZ[4]='0';WposZ[5]=0;}    
}

void menuP(){
  unsigned long timeWithOutPress = millis();
  byte optionSelect = 1;
  setTextDisplay(F("    Main Screen"),F("  Control"),F("  Card Menu"),F(""));
  moveOption(optionSelect);
  while(millis() - timeWithOutPress <= timeExit){
    long newPosition = myEnc.read();
    
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect < 3) optionSelect++;    
      moveOption(optionSelect); 
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect > 1) optionSelect--;
      moveOption(optionSelect);
    }   
    else if (digitalRead(selectPin)==LOW) {  // Press the button
      delay(10);
      while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1: 
          controlMenu(); 
        break;
        case 2: 
          byte a = fileMenu();
          if (a != 0) sendFile(a);
        break;
      }
      break;
    } 
    if(newPosition != oldPosition) {
      oldPosition = newPosition;     
      delay(timeDelay);
    }  
  }
  lcd.clear();
}

void controlMenu(){
  String table[]={"  Auto Home","  Unlock GRBL","  Move Axis","  Reset Zero", "  Return to Zero"};
  unsigned long timeWithOutPress = millis();
  byte optionSelect = 1;
  setTextDisplay(F("    Control Menu"),table[0],table[1],table[2]);
  optionSelectLast = 0;
  moveOption(optionSelect);
  
  while(millis() - timeWithOutPress <= timeExit){
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect < 5) optionSelect++; 
      if(optionSelect > 3) setTextDisplay("",table[optionSelect-3],table[optionSelect-2],table[optionSelect-1]);       
      moveOption(optionSelect); 
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect > 1) optionSelect--;
      if(optionSelect >= 3) setTextDisplay("",table[optionSelect-3],table[optionSelect-2],table[optionSelect-1]);      
      moveOption(optionSelect);
    }   
    else if (digitalRead(selectPin)==LOW) {  // Press the button
      delay(10);
      while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1: 
          homing = true;
          sendCodeLine(F("$H"),true);
          sendCodeLine(F("G10 P0 L20 X0 Y0 Z0"),true);
          homing = false;
        break;
        case 2: 
          sendCodeLine(F("$X"),true);
        break;
        case 3: 
          menuMoveAxis();
        break;
        case 4: 
          sendCodeLine(F("G10 P0 L20 X0 Y0 Z0"),true);
        break;
        case 5: 
          sendCodeLine(F("G90 G0 X0 Y0"),true);
          sendCodeLine(F("G90 G0 Z0"),true);
        break;
      }
      break;
    }   
    if(newPosition != oldPosition) {
      oldPosition = newPosition;     
      delay(timeDelay);
    }      
  }
  lcd.clear();
}

void menuMoveAxis(){
  unsigned long timeWithOutPress = millis();
  byte optionSelect = 1;

  setTextDisplay(F("   Main Move Axis"),F("=>Move 10.000mm"),F("  Move  1.000mm"),F("  Move  0.100mm"));
  moveOption(optionSelect);
  
  while(millis() - timeWithOutPress <= timeExit){
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect < 3) optionSelect++;    
      moveOption(optionSelect); 
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect > 1) optionSelect--;
      moveOption(optionSelect);
    }   
    else if (digitalRead(selectPin)==LOW) {  // Press the button
      delay(10);
      while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1: 
          setAxisToMove(100);
        break;
        case 2: 
          setAxisToMove(10);
        break;
        case 3: 
          setAxisToMove(1);
        break;
      }
      break;
    }  
    if(newPosition != oldPosition) {
    oldPosition = newPosition;     
    delay(timeDelay);
    }  
  }
  lcd.clear();
}

void setAxisToMove(byte distance){
  unsigned long timeWithOutPress = millis();
  byte optionSelect = 1;

  lcd.setCursor(0, 0);
  lcd.print("    Move ");
  lcd.print(float(distance/10.0),3);
  lcd.print("mm");

  setTextDisplay("",F("=>Move X"), F("  Move Y"),F("  Move Z"));
  
  while(millis() - timeWithOutPress <= timeExit){
    long newPosition = myEnc.read();
    byte diferencia = abs(newPosition - oldPosition);
    if (newPosition > oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect < 3) optionSelect++;    
      moveOption(optionSelect); 
    }
    else if (newPosition < oldPosition && diferencia != 3) {  // Press the button
      timeWithOutPress = millis();
      if(optionSelect > 1) optionSelect--;
      moveOption(optionSelect);
    }   
    else if (digitalRead(selectPin)==LOW) {  // Press the button
      delay(10);
      while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
      switch (optionSelect) {
        case 1: 
          moveMenu('X', distance);
        break;
        case 2: 
          moveMenu('Y', distance);
        break;
        case 3: 
         moveMenu('Z', distance);
        break;
      }
      break;
    }   
    if(newPosition != oldPosition) {
      oldPosition = newPosition;     
      delay(timeDelay);
    }   
  }
  lcd.clear();
}


void moveOption(byte optionSelect){  
  lcd.setCursor(0, 1); lcd.print("  ");  
  lcd.setCursor(0, 2); lcd.print("  ");
  lcd.setCursor(0, 3); lcd.print("  "); 
  if(optionSelect > 3) optionSelect = 3; 
  lcd.setCursor(0, optionSelect);
  lcd.print("=>");  
}

void loop(){  
  if (millis() - lastUpdateMenu >= 250) {      
    lastUpdateMenu=millis();  
    updateDisplayStatus(1);          
  }
  if (digitalRead(selectPin)==LOW) {  // Press the button
    delay(10);
    while (digitalRead(selectPin)==LOW) {} // Wait for the button to be released
    menuP();
  }
}
