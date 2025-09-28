/*
  Sources: 
  https://www.instructables.com/ESP32-ESP32-CAM-With-OV2640-Using-SIPHSPI-for-TFT-/
  https://hjwwalters.com/esp32-cam-gc9a01/
   
  push-button: take a photo
  Photos are saved in memory, unless a SDcard is present.
  
  Harware:
  - ESP32-CAM AI_THINKER
  - display ILI9341 or ST7789 with SDcard
  - 1 push-button
  - lipo batterie + charger


_________________________________________________________________
|                                                               |
|       author : Philippe de Craene <dcphilippe@yahoo.fr>       |
|                                                               |
_________________________________________________________________

IDE version 1.8.16 with Expressif Sytems (ESP32) version 2.0.14

//------------------------------------------------------------------------
//  Board            : "AI Thinker ESP32-CAM"
//------------------------------------------------------------------------
//  CPU Frequency    : "240MHz (WiFi/BT)"
//  Flash Mode       : "QIO"
//  Partition Scheme : "No OTA (2MB APP/2MB SPIFFS)" !!!! it is NOT fatfs
//  PSRAM            : "Enable"

//========================================================================

                                  AI-THINKER ESP32-CAM
                                   -------------------
                        [UNUSED]  | 5v           3.3v |  [POWER]
                        [UNUSED]  | GND        GPIO16 |  <- TOUCH_CS [ChipSelect]
                   TFT_MISO   ->  | GPIO12      GPIO0 |  <- to GND for flashing
                   TFT_MOSI   ->  | GPIO13        GND |  [UNUSED]
     [ChipSelect]  TFT_CS     ->  | GPIO15        Vcc |  [UNUSED]
                  TFT_SCLK    ->  | GPIO14   GPIO3/RX |  <- push-button or [SERIAL DEBUG]
                   TFT_DC     ->  | GPIO2    GPIO1/TX |  <- SD_CS [ChipSelect]
            TFT backlight led ->  | GPIO4         GND |  [POWER] not GND!!!!
                                   -------------------
  TFT + Touch-Screen + SDcard share the same SPI, according:
  https://forum.lvgl.io/t/sd-card-disable-my-touch-screen-help-me/14130/12
  esp32-CAM pinout: https://lastminuteengineers.com/esp32-cam-pinout-reference/

  Notes:
  - TFT MISO is not wired, but must be declared
  - TFT RST is wired to Vcc
  - if flash cannot be done put GPIO0 to GND during flash
  - GPIO1 cannot work as a input: so it will be the SDCard chip select
  - GPIO3 becomes the push-button input, wiring: 10K to GND + push-button from Vcc

version history
---------------

v0.3    21 aug 25  - first version display camera with TFT_eFEX.h library
v0.8    31 aug 25  - add deepsleep
v0.9    31 aug 25  - change pins definition for push-button: no more console message 
v1.0    31 aug 25  - revue photos from either SDCard or SPIFFS
v1.1    1 sept 25  - improve display legend and auto disporama
v1.12  14 sept 25  - correct some bug with SDCard

*/

// Parameters
//-------------------
const String VERSION = "v1.12";
//#define INIT_SPIFFS    true  // to format SPIFFS
//#define VERBOSE        true  // to comment for sd card operation
const int diaporamaDelay = 2;  // #seconds for photo show
const int delayBeforeSleep = 120;  // number of 500ms before sleep

// camera
//-------------------
#include "esp_camera.h"        // info: https://randomnerdtutorials.com/esp32-cam-ov2640-camera-settings/
#include "soc/soc.h"           // Disable brownour problems
#include "soc/rtc_cntl_reg.h"  // Disable brownour problems
camera_fb_t *fb = NULL;        // frame buffer

// tft display
//-------------------
//#########################################################################
//###### DON'T FORGET TO UPDATE THE User_Setup.h FILE IN THE LIBRARY ######
//#########################################################################
#define TFT_BL    4  // internal flash led (to remove) => back-light control pin
#define TFT_MISO 12  // internal SDcard Data2 => TFT MISO/SDO NOT CONNECTED !!!
                     // https://github.com/Bodmer/TFT_eSPI/discussions/898
#define TFT_MOSI 13  // internal SDcard Data3 => display MOSI/SDI
#define TFT_SCLK 14  // internal SDcard CLK => display SCK
#define TFT_CS   15  // internal SDcard CMD => display Chip Select control pin
#define TFT_DC    2  // internal SDcard Data0 => Data Command control pin
#define TFT_RST  -1  // => must be wired to Vcc
#define TOUCH_CS 16  // internal PSRAM => Chip select pin (T_CS) of touch screen

#include "TFT_eSPI.h"             // https://github.com/Bodmer/TFT_eSPI
TFT_eSPI tft = TFT_eSPI();        // Use hardware SPI
#include <TJpg_Decoder.h>         // https://github.com/Bodmer/TJpg_Decoder
#define backgroundColor  TFT_BLUE //TFT_BLACK
#define textColor        TFT_WHITE //TFT_GREEN
#define screenRotation   3        // 1 or 3

// SD-Card
//-------------------
#include "SPIFFS.h"         // keeps photo if no SDcard
#include "FS.h"
#include <SD.h>
#include <SPI.h>
#ifdef VERBOSE 
 #define SD_CS    -1
#else
 #define SD_CS     1       // console TX => SDcard CS pin
#endif
SPIClass sdSpi = SPIClass(HSPI);  // Declare an HSPI bus object

// to keep data
//-------------------
#include <Preferences.h>    // https://randomnerdtutorials.com/esp32-save-data-permanently-preferences/
Preferences pref;
byte fileNumber = 0;        // keep the number of photos taken

// others
//-------------------
#define pbPin     3         // internal RX => push-button
#define redLed   33         // internal small red led

bool runFlag = false;
bool tsPressed = false, memoTsPressed = false;  // flag allows photo store from display
bool pbPressed = false, memoPbPressed = false;  // flag allows photo store from push-button
bool diaporamaFlag = false; 
int menuDelay = 0, menuCounter = 0;
#define maxList  40        // maximum number of photos to retreive
String photoList[maxList];
int fileIndex = 0;
int fileTotal = 0;
String mess = "photo ";

//
// SETUP
//_____________________________________________________________________________________________

void setup() {

// Assign loop2() to core 0
//-------------------------
  xTaskCreatePinnedToCore(
    loop2,     // Function to implement the task
    "loop2",   // Name of the task
    10000,     // Stack size in bytes
    NULL,      // Task input parameter
    5,         // Priority of the task
    NULL,      // Task handle.
    0          // Core where the task should run
  );

  pinMode(pbPin, INPUT);
  //esp_sleep_enable_ext0_wakeup((gpio_num_t)pbPin, 1);  // doesn't work with GPIO3
  
// start the console
//------------------
  #ifdef VERBOSE
   Serial.begin(115200, SERIAL_8N1, -1, 1);  // RX, TX Initialize just TX on pin 1, leave left GPIO3 (RX)
   delay(1000);
   Serial.println("Starting...");
  #endif  

// start the SDcard: must be done before tft
//------------------
  bool sdOK = false;
  sdSpi.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, SD_CS);
  pinMode(SD_CS, OUTPUT);  // Documentation says that we need to do manually, the Core do not do it.
  if(!SD.begin(SD_CS, sdSpi, 4000000)) {
    #ifdef VERBOSE 
     Serial.print(F("Card Mount Failed\n"));
     delay(1000);
   } 
   else if(SD.cardType() == CARD_NONE) {
     Serial.println(F("No SD card attached"));
    #endif
   }
   else {
    #ifdef VERBOSE 
     Serial.println(F("SD-Card content"));
     ListDir(SD, "/", 0);
    #endif
    sdOK = true;
  }

// start SPIFFS  
//--------------
// info: https://www.programmingelectronics.com/spiffs-esp32/
  #ifdef INIT_SPIFFS
   if(SPIFFS.format()) {
     #ifdef VERBOSE
      Serial.println(F("SPIFFS erased successfully."));
     #endif
   }
  #endif
  SPIFFS.begin(true);
  #ifdef VERBOSE
   Serial.println(F("SPIFFS content")); ListDir(SPIFFS, "/", 0);
  #endif
  fileTotal = GetPhotoList();
  fileIndex = fileTotal;

// start the display
//------------------
  tft.init(); 
  tft.setRotation(screenRotation);
  tft.fillScreen(backgroundColor);
  tft.setSwapBytes(true);
  tft.setTextColor(textColor, backgroundColor);
  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextDatum(MC_DATUM);
  tft.drawString("Bonjour",  160, 100);
  if(sdOK) tft.drawString("Carte SD ok",  160, 140);
  else     tft.drawString("Carte SD absente",  160, 140);
/*
  Normally strings are printed relative to the top left corner but this can be
  changed with the setTextDatum() function. The library has #defines for:

  TL_DATUM = 0 = Top left
  TC_DATUM = 1 = Top centre
  TR_DATUM = 2 = Top right
  ML_DATUM = 3 = Middle left
  MC_DATUM = 4 = Middle centre
  MR_DATUM = 5 = Middle right
  BL_DATUM = 6 = Bottom left
  BC_DATUM = 7 = Bottom centre
  BR_DATUM = 8 = Bottom right

  L_BASELINE =  9 = Left character baseline (Line the 'A' character would sit on)
  C_BASELINE = 10 = Centre character baseline
  R_BASELINE = 11 = Right character baseline
*/

  TJpgDec.setJpgScale(2);    // The jpeg image can be scaled by a factor of 1, 2, 4, or 8
  TJpgDec.setCallback(tft_output);  // The decoder must be given the exact name of the rendering function

// start camera
//------------------
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);  // turn-off the 'brownout detector'

// camera setup - AI Thinker - ESP32-CAM pins definition
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 5;
  config.pin_d1 = 18;
  config.pin_d2 = 19;
  config.pin_d3 = 21;
  config.pin_d4 = 36;
  config.pin_d5 = 39;
  config.pin_d6 = 34;
  config.pin_d7 = 35;
  config.pin_xclk = 0;
  config.pin_pclk = 22;
  config.pin_vsync = 25;
  config.pin_href = 23;
  config.pin_sscb_sda = 26;
  config.pin_sscb_scl = 27;
  config.pin_pwdn = 32;
  config.pin_reset = -1;

  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;   // YUV422|GRAYSCALE|RGB565|JPEG
  config.frame_size = FRAMESIZE_VGA; 
  /*  UXGA(1600x1200)
      SXGA(1280x1024)
      XGA(1024x768)
      SVGA(800x600)
      VGA(640x480)
      CIF(400x296)
      QVGA(320x240)
      HQVGA(240x176)
      QQVGA(160x120)
  */
  config.jpeg_quality = 12;               // 10-63 ; plus bas = meilleure qualité
  config.fb_count = 1;                    // nombre de frame buffers
  config.fb_location = CAMERA_FB_IN_DRAM;
  //config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  
// camera init
  esp_err_t err = esp_camera_init(&config);
  if(err != ESP_OK) {
    #ifdef VERBOSE
     Serial.printf("Camera init failed with error 0x%x\n", err);
    #endif
    tft.fillScreen(backgroundColor);
    tft.drawString("Erreur", 160, 120);
    delay(2000);
    ESP.restart();
  }
  
  sensor_t * s = esp_camera_sensor_get();
  s->set_vflip(s, 1);          // 0 = disable , 1 = enable
  s->set_hmirror(s, 1);        // 0 = disable , 1 = enable
/*  s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);     // -2 to 2
    s->set_special_effect(s, 0); // 0 to 6 (0 - No Effect, 1 - Negative, 2 - Grayscale, 3 - Red Tint, 4 - Green Tint, 5 - Blue Tint, 6 - Sepia)
    s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
    s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
    s->set_wb_mode(s, 0);        // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
    s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
    s->set_aec2(s, 0);           // 0 = disable , 1 = enable
    s->set_ae_level(s, 0);       // -2 to 2
    s->set_aec_value(s, 300);    // 0 to 1200
    s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30
    s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
    s->set_bpc(s, 0);            // 0 = disable , 1 = enable
    s->set_wpc(s, 1);            // 0 = disable , 1 = enable
    s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable
    s->set_lenc(s, 1);           // 0 = disable , 1 = enable
    s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
    s->set_vflip(s, 0);          // 0 = disable , 1 = enable
    s->set_dcw(s, 1);            // 0 = disable , 1 = enable
    s->set_colorbar(s, 0);       // 0 = disable , 1 = enable
*/

// get photo index number
  pref.begin("parameters", false);
  fileNumber = pref.getUInt("num", 0);
  
  tft.drawString(VERSION, 160, 220);
  delay(1000);

}      // end of setup

//
// LOOP
//_____________________________________________________________________________________________

void loop() {

// menu action every 500ms
//------------------------
  static unsigned long memoTempo = 0;
  unsigned long tempo = millis()/500;

  if(tempo > memoTempo) {
    memoTempo = tempo;

// deepsleep action
//-----------------
    menuDelay--;
    if(menuDelay < -delayBeforeSleep) {
      tft.fillScreen(backgroundColor);
      tft.drawString("bon dodo...",  160, 120);
      delay(1000);
      pinMode(redLed, INPUT);     // desactivated red led
      pinMode(TFT_BL, INPUT);     // desactivate tft backlight
      esp_deep_sleep_start();     // goto sleep
    }

// push-button action
//-------------------
    memoPbPressed = pbPressed;
    pbPressed = digitalRead(pbPin);
    if(pbPressed && !memoPbPressed) {
      menuDelay = 2;
      SavePicture();
    }

// touch screen action
//--------------------
    uint16_t xTS = 0, yTS = 0;        // touch-screen position
    memoTsPressed = tsPressed;
    tsPressed = false;
    if((tft.getTouch(&xTS, &yTS))&& !memoTsPressed && runFlag) {
      #ifdef VERBOSE
       Serial.printf("x: %i   ", xTS);
       Serial.printf("y: %i\n ", yTS);
      #endif

      tsPressed = true;
      menuDelay = 2;
      MenuDisplay();

// take photo
      if((xTS >100)&&(xTS <220)) {
        menuDelay = 4;
        SavePicture();
      }
      else {
        menuCounter++;
        menuDelay = 10;

// right part of screen
        if(xTS > 220) {
          if(menuCounter ==1) {
            tft.drawString("mode revue",  160, 120);
            delay(300);
            if(fileTotal <1)       mess = "aucune photo";
            else if(fileTotal ==1) mess = "une seule photo";
            else                 { mess = String(fileTotal); mess += " photos"; }
            tft.drawString(mess,  160, 140);
          }  // end of test menuCounter ==1
          else if(fileTotal > 0) {
            if(yTS < 80) {
              if(--fileIndex < 1) fileIndex = fileTotal;
              mess = "photo " + String(fileIndex);
              tft.fillRect( 100, 120, 120, 40, backgroundColor);
              tft.drawString(mess,  160, 140);
              delay(100);
              DrawPicture(photoList[fileIndex -1].c_str());
            } 
            else if(yTS > 160) { 
              if(++fileIndex > fileTotal) fileIndex = 1;
              mess = "photo " + String(fileIndex);
              tft.fillRect( 100, 120, 120, 40, backgroundColor);
              tft.drawString(mess,  160, 140);
              delay(100);
              DrawPicture(photoList[fileIndex -1].c_str());
            } 
            else if((yTS > 100)&&(yTS < 140)) {
              tft.drawString("suppression...",  160, 120);
              delay(10);
              File root = SD.open("/");;
              if(root) SD.remove((const char *)photoList[fileIndex -1].c_str());
              else {
                root = SPIFFS.open("/");
                SPIFFS.remove((const char *)photoList[fileIndex -1].c_str());
              }
              fileTotal = GetPhotoList();
              if(fileIndex > fileTotal) fileIndex = fileTotal;
              if(fileTotal > 0) {
                mess = "photo" + String(fileIndex);
                tft.drawString(mess,  160, 140);
                delay(100);
                DrawPicture(photoList[fileIndex -1].c_str());
              }
              else menuDelay = 0;
            }
          }  // end of test menuCounter >1
          else menuDelay = 0;
        }    // end of test xTS > 220

// left part of screen
        else if(xTS < 100) {
          if(yTS  > 140) {
            if(menuCounter ==1) {
              tft.drawString("diaporama",  160, 120);
              delay(300);
              if(fileTotal <1)       mess = "aucune photo";
              else if(fileTotal ==1) mess = "une seule photo";
              else                 { mess = String(fileTotal); mess += " photos"; }
              tft.drawString(mess,  160, 140);
            }  // end of test menuCounter ==1
            else {
              fileIndex = fileTotal;
              if(fileTotal > 0) diaporamaFlag = true;
            }
          }    // end of test yTS > 140
          else {
            diaporamaFlag = false;
            menuCounter = 0;
            menuDelay = 1;
          }
        }  // end of test xTS < 100
      }    // end else 
    }  // end of test tft.getTouch
  }    // end of test tempo

// diaporama option
//-----------------
  if(diaporamaFlag) {
    if(fileIndex > 0) {
      menuDelay = 1;
      DrawPicture(photoList[fileIndex -1].c_str());
      fileIndex--;
      delay(diaporamaDelay *1000);
    }
  }

// Menu management or camera opération
//------------------------------------
  if(menuDelay > 0) return;
  else  menuCounter = 0;

// take and display pictures
//--------------------------
  esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();      // prise de la photo
  if(!fb) {
    tft.fillScreen(backgroundColor);
    tft.drawString("Erreur", 160, 120);
    delay(1000);
    return;
  }
  uint16_t w = 0, h = 0;
  TJpgDec.getJpgSize(&w, &h, (const uint8_t*)fb->buf, fb->len);
  TJpgDec.drawJpg(0, 0, (const uint8_t*)fb->buf, fb->len);

  runFlag = true;    // redled blinks as soon as a 1st picture is displayed

}      // end of loop

//
// LOOP2 runs on core 0
//_____________________________________________________________________________________________

void loop2(void* pvParameters) {

//core0 setup
//-------------------------
// start the red led and push-button
  pinMode(redLed, OUTPUT);

//core0 loop
//-------------------------
  for(;;) {
    while(runFlag) {
      digitalWrite(redLed, LOW); delay(90);
      digitalWrite(redLed, HIGH); delay(410);
    }
	digitalWrite(redLed, LOW); 
  }
}      // end of loop2

//============================================================================================
// list of functions
//============================================================================================

//
// DrawPicture() : display selected picture from SDcard or SPIFFS
//____________________________________________________________________________________________

void DrawPicture( const char *photo) {
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  delay(10);
  File root = SD.open(photo);
  if(root) TJpgDec.drawSdJpg(0, 0, photo);
  else {
    root = SPIFFS.open(photo);
    TJpgDec.drawFsJpg(0, 0, photo);
  }
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  root.close();
}
           
//
// SavePicture() : store the picture to SDcard or SPIFFS
//____________________________________________________________________________________________

void SavePicture() {
  bool sdOK = false;
  char fileName[20] = "pic";                   // picture file name
  sprintf(fileName, "/%d.jpg", fileNumber);    // file name creation
  delay(10);
  File file = SD.open(fileName, FILE_WRITE);
  if(file) sdOK = true;
  else file = SPIFFS.open(fileName, FILE_WRITE);
  
  file.write((const uint8_t *)fb->buf, fb->len);
  file.close();
  pref.putUInt("num", ++fileNumber);           // keep the file index number
  tft.drawString(String(fileName), 160, 120);
  if(sdOK) {
    tft.drawString("sur carte SD", 160, 140);
    #ifdef VERBOSE
     Serial.println("sdcard saved");
     ListDir(SD, "/", 0);
    #endif
  } else {
    tft.drawString("en memoire", 160, 140);
  }
  fileTotal = GetPhotoList();
}      // end of SavePicture()

//
// GetPhotoList() : //Gets all image files in the SD card root directory
//____________________________________________________________________________________________

int GetPhotoList() {
  digitalWrite(TFT_CS, HIGH);
  digitalWrite(TOUCH_CS, HIGH);
  int i = 0;
  delay(10);
  File root = SD.open("/");
  if(!root) root = SPIFFS.open("/");
  File file = root.openNextFile();
  while(file && (i < maxList)) {
    if(!file.isDirectory()) {
      String temp = "/";
      temp += file.name();
      if(temp.endsWith(".jpg")) {
        photoList[i] = temp;
        i++;
      }
    }
    file = root.openNextFile();
  }
  return i;
}

//
// MenuDisplay()
//____________________________________________________________________________________________

void MenuDisplay() {
  tft.drawRect(100, 20, 120, 200, backgroundColor);
  //tft.drawString("prendre", 160, 100);
  //tft.drawString("la", 160, 120);
  //tft.drawString("photo", 160, 140);

  tft.drawRect(10, 20, 80, 60, backgroundColor);
  tft.drawString("diapo",50, 50);
  tft.drawRect(10, 160, 80, 60, backgroundColor);
  tft.drawString("stop", 50, 190);

  int cx =  270;  // middle of the x
  int cy1 =  40;  // middle of the y, bottom of upstair triangle
  int cy2 = 120;  // bottom of the cercle
  int cy3 = 200;  // bottom of the downstait triangle
  int i = 20;     // half of the size
  
  tft.drawTriangle(cx    , cy1 - i, // peak
                   cx - i, cy1 + i, // bottom left
                   cx + i, cy1 + i, // bottom right
                   backgroundColor);
  //tft.drawCircle(cx, cy2, i, backgroundColor);
  //tft.drawRect(cx - i, cy2 - i, 2 * i, 2* i, backgroundColor);
  tft.drawLine(cx + i, cy2 + i, cx - i, cy2 -i, backgroundColor);
  tft.drawLine(cx + i, cy2 - i, cx - i, cy2 +i, backgroundColor);
  tft.drawTriangle(cx    , cy3 + i, // peak
                   cx - i, cy3 - i, // bottom left
                   cx + i, cy3 - i, // bottom right
                   backgroundColor);
}

//
// tft_output()
//____________________________________________________________________________________________

bool tft_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  if( y >= tft.height()) return 0;  // Stop further decoding as image is running off bottom of screen

  // This function will clip the image block rendering automatically at the TFT boundaries
  tft.pushImage(x, y, w, h, bitmap);
  return 1;      // Return 1 to decode next block
}

//
// SDCard functions
//____________________________________________________________________________________________

#ifdef VERBOSE
void ListDir(fs::FS &fs, const char *dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);

  File root = fs.open(dirname);
  if(!root) {
    Serial.println(F("Error to open directory"));
    return;
  } 
  if(!root.isDirectory()) {
    Serial.println(F("Not a directory"));
    return;
  }

  File file = root.openNextFile();
  while(file) {
    if(file.isDirectory()) {
      Serial.print(F("\tDIR : "));
      Serial.println(file.name());
      if(levels >0) ListDir(fs, file.name(), levels -1);
    }
    else {
      Serial.print(F("\tFILE: "));
      Serial.print(file.name());
      Serial.print(F("\tSIZE: "));
      Serial.println(file.size());
    }
    file = root.openNextFile();
  }
}      // end of ListDir()

void CopyFile(fs::FS &fs1, const char * path1, fs::FS &fs2, const char * path2) {
  Serial.printf("copying: %s/%s to %s/%s\n", fs1, path1, fs2, path2);
  File file1 = fs1.open(path1, FILE_READ);
  if( fs2.open(path2, FILE_READ)) Serial.printf("%s yet exists, delete it first\n", path2);
  File file2 = fs2.open(path2, FILE_WRITE);

  size_t n;  
  uint8_t buf[64];
  while((n = file1.read(buf, sizeof(buf))) > 0) {
    Serial.println(n);
    file2.write(buf, n);
  }
  file2.close();
}
#endif

void DeleteFile(fs::FS &fs, const char * path) {
  if(fs.remove((const char *)path)) {
    #ifdef VERBOSE
     Serial.printf("File: %s/%s deleted\n", fs, String(path));
  } else {
     Serial.printf("Error deleting: %s/%s\n", fs, String(path));
    #endif
  }
}
