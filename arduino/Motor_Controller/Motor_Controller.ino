/*
Testprogramm für das 3 Achsen I2C Stepperboard 
(c) 2025 ATbio - Uni Freiburg - js (updated by LH)
P002752
Arduino IDE 2.3.6
Verwendete Bibliothek: Adafruit MCP23X17 Lib 2.3.2
*/

#include <Adafruit_MCP23X17.h>
#include <EEPROM.h>

// Forward declaration — loadMode is defined in USBTerminal.ino
extern bool loadMode;

#define M1_Switch1_PIN 0  // Pinnummern der Portexpander. Sie werden durchgezählt von GPA0=0 bis GPB7=13
#define M1_Switch2_PIN 1
#define M2_Switch1_PIN 2
#define M2_Switch2_PIN 3
#define M3_Switch1_PIN 4
#define M3_Switch2_PIN 5
#define M1_Enable_PIN 6
#define M1_Dir_PIN 7
#define M1_Step_PIN 8
#define M2_Enable_PIN 9
#define M2_Dir_PIN 10
#define M2_Step_PIN 11
#define M3_Enable_PIN 12
#define M3_Dir_PIN 13
#define M3_Step_PIN 14
#define LEDrot_PIN 13  // LED des Controllers

#define DIO1 3  // Trigger Pins zum Anfahren der abgespeicherten Positionen
#define DIO2 5
#define DIO3 7
#define DIO4 9
#define DIO5 11  // Trigger Pins zum Anfahren der abgespeicherten Positionen
#define DIO6 15
#define DIO7 17
#define DIO8 19

bool debug = 1;  // Schaltet Infotexte auf dem Terminal ein

Adafruit_MCP23X17 mcp1;         // Portexpander 1
Adafruit_MCP23X17 mcp2;         // Portexpander 2
long timeMerker1 = 0;           // Zwischenspeicher für Timingmessungen
float schrittWeite_mm = 1.0;    // Wird für USB Kommandos benötigt
int USB_SchrittSpeed = 2;       // Wird für USB Kommandos benötigt
byte uCTyp = 0;                 // 0=Arduino Uno, Nano oder Mega, 1= Arduino Nano R4
bool singleManipulator = HIGH;  // Wenn nur ein Dreiachsenmanipulator verwendet wird = HIGH

// Hier wird ein Motordefinitionsarray erstellt.
// Jede Zeile enthält die Information für den jewiligen Motor
// Motor 1 bis 3  sind am Manipulator links, Motor 4 bis 6 sind am Manipulator rechts, jeweils in der Reihenfolge X, Y, Z
// Inhalt:
// Portexpandernummer, M1_Enable_PIN, M1_Dir_PIN, M1_Step_PIN, M1_Switch1_PIN, M1_Switch2_PIN, Steps pro Umdrehung, Spindelsteigung in 1/1000mm, Achsenlänge in Schritten
int MotorDef[6][9] = {
  { 1, M1_Enable_PIN, M1_Dir_PIN, M1_Step_PIN, M1_Switch1_PIN, M1_Switch2_PIN, 200, 1500, 6666 },  // Motor 1 (X1)
  { 1, M2_Enable_PIN, M2_Dir_PIN, M2_Step_PIN, M2_Switch1_PIN, M2_Switch2_PIN, 200, 1500, 6666 },  // Motor 2 (Y1)
  { 1, M3_Enable_PIN, M3_Dir_PIN, M3_Step_PIN, M3_Switch1_PIN, M3_Switch2_PIN, 200, 1500, 6666 },  // Motor 3 (Z1)
  { 2, M1_Enable_PIN, M1_Dir_PIN, M1_Step_PIN, M1_Switch1_PIN, M1_Switch2_PIN, 200, 1500, 6666 },  // Motor 4 (X2)
  { 2, M2_Enable_PIN, M2_Dir_PIN, M2_Step_PIN, M2_Switch1_PIN, M2_Switch2_PIN, 200, 1500, 6666 },  // Motor 5 (Y2)
  { 2, M3_Enable_PIN, M3_Dir_PIN, M3_Step_PIN, M3_Switch1_PIN, M3_Switch2_PIN, 200, 1500, 6666 }   // Motor 6 (Z2)
};
// Speicher für die Positionen der Achsen. Abgelegt werden "Schritte" vom Nuppunkt aus gesehen.

long MotorPosition[6] = { 0, 0, 0, 0, 0, 0 };  // Motorpositionen, Angabe in Schritten
// Slots 1-8:  permanent mouse lick positions (loaded from EEPROM at boot)
// Slots 9-12: session-derived task positions (RAM only, set by SETMOUSE command)
//   Slot 9  = current mouse lick Y - reach_1_mm  (short reach)
//   Slot 10 = current mouse lick Y - reach_2_mm  (medium reach)
//   Slot 11 = current mouse lick Y - reach_3_mm  (full reach)
//   Slot 12 = current mouse lick Y - out_mm      (out of reach / ITI)
long PosSpeicher[12][6] = {
  { 0, 0, 0, 0, 0, 0 },  // Slot  1 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  2 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  3 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  4 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  5 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  6 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  7 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  8 — mouse lick pos (EEPROM)
  { 0, 0, 0, 0, 0, 0 },  // Slot  9 — reach pos 1    (RAM only)
  { 0, 0, 0, 0, 0, 0 },  // Slot 10 — reach pos 2    (RAM only)
  { 0, 0, 0, 0, 0, 0 },  // Slot 11 — reach pos 3    (RAM only)
  { 0, 0, 0, 0, 0, 0 }   // Slot 12 — out of reach   (RAM only)
};

int Speed[2][4] = {
  // Definiert Pausenzeiten
  { 700, 600, 400, 180 },   // Speed 0 bis 3 uC 0 (Mega)
  { 1000, 700, 500, 250 },  // Speed 0 bis 3 uC 1 (Nano R4)
};

byte bytes[4];  // Zwischenspeicher für Umwandlung Long to bytes

#define POS1_ADDRESS 0   // Startadressen der 4 eingespeicherten Positionen im EEProm
#define POS2_ADDRESS 24  // 0 + 24
#define POS3_ADDRESS 48  // 24 + 24
#define POS4_ADDRESS 72  // 48 + 24
#define POS5_ADDRESS 96  
#define POS6_ADDRESS 120  
#define POS7_ADDRESS 144  
#define POS8_ADDRESS 168  


union LongBytes {  // Für die Umwandlung von Long in Byte
  long longValue;
  byte byteArray[4];
};


void setup() {
  pinMode(LEDrot_PIN, OUTPUT);
  pinMode(DIO1, INPUT_PULLUP);  // Eingänge für die Triggerpins
  pinMode(DIO2, INPUT_PULLUP);
  pinMode(DIO3, INPUT_PULLUP);
  pinMode(DIO4, INPUT_PULLUP);
  pinMode(DIO5, INPUT_PULLUP);  // Eingänge für die Triggerpins
  pinMode(DIO6, INPUT_PULLUP);
  pinMode(DIO7, INPUT_PULLUP);
  pinMode(DIO8, OUTPUT);
  digitalWrite(DIO8, 0); // "Not Ready" Signal setzen
  Serial.begin(115200);                           // Serielle Schnittstelle initialisieren
  unsigned long timeout = millis();               //
  while (!Serial && millis() - timeout < 2000) {  // Maximal 2 Sekunden auf serielle Verbindung warten
  }
  Serial.println(F("System bereit (mit Timeout-Fallback)"));
  Serial.println("Demo - 3 Achsen Schrittmotorsteuerung mit I2C");

  if (!mcp1.begin_I2C(0x20)) {  // Initialisierung Portexpander Positioniersystem 1
    Serial.println("Error Portexpander 1");
    while (1)
      ;
  }
  if (!singleManipulator) {
    if (!mcp2.begin_I2C(0x21)) {  // Initialisierung Portexpander Positioniersystem 2
      Serial.println("Error Portexpander 2");
      while (1)
        ;
    }
  }

  Wire.setClock(400000);         // I2C Busgeschwindigkeit erhöhen
  Serial.println("Starte ...");  //Achsen initialisieren

  mcpIOinit();                                  // IOs der Portexpander definieren
  /*
  Serial.println("Starte Initialisierung...");  //Achsen initialisieren
  initAchse(1);
  if (singleManipulator == LOW) initAchse(4);
  initAchse(2);
  if (singleManipulator == LOW) initAchse(5);
  initAchse(3);
  if (!singleManipulator) initAchse(6);
*/
  readPositionFromEEPROM(POS1_ADDRESS, PosSpeicher[0]); // Gespeichte Positionen aus EEPROM auslesen
  readPositionFromEEPROM(POS2_ADDRESS, PosSpeicher[1]);
  readPositionFromEEPROM(POS3_ADDRESS, PosSpeicher[2]);
  readPositionFromEEPROM(POS4_ADDRESS, PosSpeicher[3]);
  readPositionFromEEPROM(POS5_ADDRESS, PosSpeicher[4]);
  readPositionFromEEPROM(POS6_ADDRESS, PosSpeicher[5]);
  readPositionFromEEPROM(POS7_ADDRESS, PosSpeicher[6]);
  readPositionFromEEPROM(POS8_ADDRESS, PosSpeicher[7]);



  // printPosArray();
  if (debug == 1) printUSBinfo();
  if (debug == 1) printPositionen();

  digitalWrite(DIO8, 1); // Setze das "READY Bit"
  /*
  goTo(2, 25.0, 3);                 // Beispiel: Fahre Achse 2 (Y1) zu Position 25 mm mit Geschwindikeit 2
  goTo(4, 25.0, 3);                 // Beispiel: Fahre Achse 4 (Y2) zu Position 25 mm mit Geschwindikeit 2
  delay(1000);                      // mache 1 s Pause
  GoToXYZ(1, 25.0, 30.0, 35.0, 3);  // Fahre  Positioniersystem 1 zur Grundstellung - Achse x, y, und z nacheinander
  GoToXYZ(2, 25.0, 30.0, 35.0, 3);  // Fahre  Positioniersystem 2 zur Grundstellung - Achse x, y, und z nacheinander
   */
  //mcp1.digitalWrite(M1_Enable_PIN,LOW);

digitalWrite(DIO8, 1); // DIO Freigabe
}

void loop() {
  digitalWrite(DIO8, 1);
  handleLineCommands(); // MUST run before USBcommand — consumes full lines like
                        // SETMOUSE before the single-char handler can eat them
  USBcommand();
  chkMotorOff();

  // Ignore DIO triggers while loading positions from PC via serial
  if (!loadMode && digitalRead(DIO7) == LOW) DIOBefehl();
}
