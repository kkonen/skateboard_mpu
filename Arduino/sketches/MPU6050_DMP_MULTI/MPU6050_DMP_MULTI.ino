// I2C device class (I2Cdev) demonstration Arduino sketch for MPU6050 class using DMP (MotionApps v2.0)
// 6/21/2012 by Jeff Rowberg <jeff@rowberg.net>
// Updates should (hopefully) always be available at https://github.com/jrowberg/i2cdevlib
//
// Changelog:
//      2013-05-08 - added seamless Fastwire support
//                 - added note about gyro calibration
//      2012-06-21 - added note about Arduino 1.0.1 + Leonardo compatibility error
//      2012-06-20 - improved FIFO overflow handling and simplified read process
//      2012-06-19 - completely rearranged DMP initialization code and simplification
//      2012-06-13 - pull gyro and accel data from FIFO packet instead of reading directly
//      2012-06-09 - fix broken FIFO read sequence and change interrupt detection to RISING
//      2012-06-05 - add gravity-compensated initial reference frame acceleration output
//                 - add 3D math helper file to DMP6 example sketch
//                 - add Euler output and Yaw/Pitch/Roll output formats
//      2012-06-04 - remove accel offset clearing for better results (thanks Sungon Lee)
//      2012-06-01 - fixed gyro sensitivity to be 2000 deg/sec instead of 250
//      2012-05-30 - basic DMP initialization working

/* ============================================
I2Cdev device library code is placed under the MIT license
Copyright (c) 2012 Jeff Rowberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
===============================================
*/

// I2Cdev and MPU6050 must be installed as libraries, or else the .cpp/.h files
// for both classes must be in the include path of your project
#include "I2Cdev.h"

#include "MPU6050_6Axis_MotionApps20.h"
//#include "MPU6050.h" // not necessary if using MotionApps include file

// Arduino Wire library is required if I2Cdev I2CDEV_ARDUINO_WIRE implementation
// is used in I2Cdev.h
#if I2CDEV_IMPLEMENTATION == I2CDEV_ARDUINO_WIRE
    #include "Wire.h"
#endif

// class default I2C address is 0x68
// specific I2C addresses may be passed as a parameter here
// AD0 low = 0x68 (default for SparkFun breakout and InvenSense evaluation board)
// AD0 high = 0x69
MPU6050 mpu;
//MPU6050 mpu(0x69); // <-- use for AD0 high

const int devices = 3;

int intPins[4] = {5,6,10,11};
int addPins[4] = {A0,A1,A2,A3};


// uncomment "OUTPUT_READABLE_QUATERNION" if you want to see the actual
// quaternion components in a [w, x, y, z] format (not best for parsing
// on a remote host such as Processing or something though)
//#define OUTPUT_READABLE_QUATERNION

// uncomment "OUTPUT_READABLE_EULER" if you want to see Euler angles
// (in degrees) calculated from the quaternions coming from the FIFO.
// Note that Euler angles suffer from gimbal lock (for more info, see
// http://en.wikipedia.org/wiki/Gimbal_lock)
//#define OUTPUT_READABLE_EULER

// uncomment "OUTPUT_READABLE_YAWPITCHROLL" if you want to see the yaw/
// pitch/roll angles (in degrees) calculated from the quaternions coming
// from the FIFO. Note this also requires gravity vector calculations.
// Also note that yaw/pitch/roll angles suffer from gimbal lock (for
// more info, see: http://en.wikipedia.org/wiki/Gimbal_lock)
#define OUTPUT_READABLE_YAWPITCHROLL

// uncomment "OUTPUT_READABLE_REALACCEL" if you want to see acceleration
// components with gravity removed. This acceleration reference frame is
// not compensated for orientation, so +X is always +X according to the
// sensor, just without the effects of gravity. If you want acceleration
// compensated for orientation, us OUTPUT_READABLE_WORLDACCEL instead.
//#define OUTPUT_READABLE_REALACCEL

// uncomment "OUTPUT_READABLE_WORLDACCEL" if you want to see acceleration
// components with gravity removed and adjusted for the world frame of
// reference (yaw is relative to initial orientation, since no magnetometer
// is present in this case). Could be quite handy in some cases.
//#define OUTPUT_READABLE_WORLDACCEL

// uncomment "OUTPUT_TEAPOT" if you want output that matches the
// format used for the InvenSense teapot demo
//#define OUTPUT_TEAPOT


#define LED_PIN 13 // (Arduino is 13, Teensy is 11, Teensy++ is 6)
bool blinkState = false;


// MPU control/status vars
bool dmpReady[4] = {false,false,false,false};  // set true if DMP init was successful

uint8_t mpuIntStatus[devices];   // holds actual interrupt status byte from MPU
uint8_t devStatus[devices];      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount[devices];     // count of all bytes currently in FIFO
uint8_t fifoBuffer[devices][64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

// packet structure for InvenSense teapot demo
uint8_t teapotPacket[14] = { '$', 0x02, 0,0, 0,0, 0,0, 0,0, 0x00, 0x00, '\r', '\n' };



// ================================================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt[4] = {false,false,false,false};     // indicates whether MPU interrupt pin has gone high
void (*callback_ptr[4])();

void dmpDataReady0() {
    mpuInterrupt[0] = true;
}
void dmpDataReady1() {
    mpuInterrupt[1] = true;
}
void dmpDataReady2() {
    mpuInterrupt[2] = true;
}
void dmpDataReady3() {
    mpuInterrupt[3] = true;
}

bool isDmpReady() {
  for (int i = 0; i < devices; i++) {
    if (dmpReady[i] == false)
      return false;
  }
  return true;
}

bool allMpuInt() {
  for (int i = 0; i < devices; i++) {
    if (mpuInterrupt[i] == false)
      return false;
  }
  return true;
}

int allFifoCountLargerThan(int _psize) {
  for (int i = 0; i < devices; i++) {
    if (fifoCount[i] < _psize)
      return false;
  }
  return true;
}

// ================================================================
// ===                 MPU ADDRESS COMMUNICATION                ===
// ================================================================

void selectDevice(int dev){
  for(int i=0; i<devices; i++){
    if(i == dev){
      digitalWrite(addPins[i],LOW);
    }else{
      digitalWrite(addPins[i],HIGH);
    }
  }
}

// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void setup() {

    callback_ptr[0] = &dmpDataReady0;
    callback_ptr[1] = &dmpDataReady1;
    callback_ptr[2] = &dmpDataReady2;
    callback_ptr[3] = &dmpDataReady3;

    Wire.begin();
    Wire.setClock(400000); // 400kHz I2C clock. Comment this line if having compilation difficulties


    //SET ALL DEVICES TO ADDRESS 0x69
    for(int i=0; i<devices; i++){
      pinMode(addPins[i], OUTPUT);
      digitalWrite(addPins[i], HIGH);
    }
    
    // initialize serial communication
    Serial.begin(115200);
    while (!Serial); // wait for Leonardo enumeration, others continue immediately


    Serial.println(F("Initializing I2C devices..."));
    for(int i=0; i<devices; i++){

      selectDevice(i);
      // initialize device
      mpu.initialize();
      pinMode(intPins[i], INPUT);
 
  
      // verify connection
      Serial.print("Testing MPU"); Serial.print(i); Serial.print(" connection... ");
      bool connection = mpu.testConnection();
      Serial.println(connection ? F("connection successful!") : F("connection FAILED!"));
  
      if(connection){
        // load and configure the DMP
        Serial.println(F("   Initializing DMP..."));
        devStatus[i] = mpu.dmpInitialize();
    
        // supply your own gyro offsets here, scaled for min sensitivity
        mpu.setXGyroOffset(220);
        mpu.setYGyroOffset(76);
        mpu.setZGyroOffset(-85);
        mpu.setZAccelOffset(1788); // 1688 factory default for my test chip
    
        // make sure it worked (returns 0 if so)
        if (devStatus[i] == 0) {
            // turn on the DMP, now that it's ready
            Serial.println(F("   Enabling DMP..."));
            mpu.setDMPEnabled(true);
    
            // enable Arduino interrupt detection
            Serial.print("  Enabling interrupt detection (Arduino interrupt pin "); Serial.print(intPins[i]); Serial.println(") ...");
            attachInterrupt(digitalPinToInterrupt(intPins[i]), callback_ptr[i], RISING);
            mpuIntStatus[i] = mpu.getIntStatus();
    
            // set our DMP Ready flag so the main loop() function knows it's okay to use it
            Serial.println(F("   DMP ready! Waiting for first interrupt..."));
            dmpReady[i] = true;
    
            // get expected DMP packet size for later comparison
            packetSize = mpu.dmpGetFIFOPacketSize();
        } else {
            // ERROR!
            // 1 = initial memory load failed
            // 2 = DMP configuration updates failed
            // (if it's going to break, usually the code will be 1)
            Serial.print(F("   DMP Initialization failed (code "));
            Serial.print(devStatus[i]);
            Serial.println(F(")"));
        }
      }
  
      

    
      //next dev
    }

    // configure LED for output
    pinMode(LED_PIN, OUTPUT);

    // wait for ready
    Serial.println(F("\nSend any character to begin data collection: "));
    while (Serial.available() && Serial.read()); // empty buffer
    while (!Serial.available());                 // wait for data
    while (Serial.available() && Serial.read()); // empty buffer again

    for(int i=0; i<devices; i++){
      selectDevice(i);
      mpu.resetFIFO();
    }
}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void loop() {
    // if programming failed, don't try to do anything
    if (!isDmpReady()) return;

    // wait for MPU interrupt or extra packet(s) available
    while (!allMpuInt() && !allFifoCountLargerThan(packetSize)) {
        // other program behavior stuff here
        // .
        // .
        // .
        // if you are really paranoid you can frequently test in between other
        // stuff to see if mpuInterrupt is true, and if so, "break;" from the
        // while() loop to immediately process the MPU data
        // .
        // .
        // .
    }

    for(int i=0; i<devices; i++){
      selectDevice(i);
      // reset interrupt flag and get INT_STATUS byte
      mpuInterrupt[i] = false;
      mpuIntStatus[i] = mpu.getIntStatus();
  
      // get current FIFO count
      fifoCount[i] = mpu.getFIFOCount();

      Serial.print("MPU "); Serial.print(i); Serial.print(" FifoCount: ");Serial.println(fifoCount[i]);
      // check for overflow (this should never happen unless our code is too inefficient)
      if ((mpuIntStatus[i] & 0x10) || fifoCount[i] == 1024) {
          // reset so we can continue cleanly
          mpu.resetFIFO();
          Serial.println(F("FIFO overflow!"));
  
      // otherwise, check for DMP data ready interrupt (this should happen frequently)
      } else if (mpuIntStatus[i] & 0x02) {
          // wait for correct available data length, should be a VERY short wait
          while (fifoCount[i] < packetSize) fifoCount[i] = mpu.getFIFOCount();
  
          // read a packet from FIFO
          while(fifoCount[i] >= packetSize){
            mpu.getFIFOBytes(fifoBuffer[i], packetSize);
            
            // track FIFO count here in case there is > 1 packet available
            // (this lets us immediately read more without waiting for an interrupt)
            fifoCount[i] -= packetSize;

          }
         
          /*#ifdef OUTPUT_READABLE_QUATERNION
              // display quaternion values in easy matrix form: w x y z
              mpu.dmpGetQuaternion(&q, fifoBuffer[i]);
              Serial.print("quat\t");
              Serial.print(q.w);
              Serial.print("\t");
              Serial.print(q.x);
              Serial.print("\t");
              Serial.print(q.y);
              Serial.print("\t");
              Serial.println(q.z);
          #endif
  
          #ifdef OUTPUT_READABLE_EULER
              // display Euler angles in degrees
              mpu.dmpGetQuaternion(&q, fifoBuffer[i]);
              mpu.dmpGetEuler(euler, &q);
              Serial.print("euler\t");
              Serial.print(euler[0] * 180/M_PI);
              Serial.print("\t");
              Serial.print(euler[1] * 180/M_PI);
              Serial.print("\t");
              Serial.println(euler[2] * 180/M_PI);
          #endif
  
          #ifdef OUTPUT_READABLE_YAWPITCHROLL
              // display Euler angles in degrees
              mpu.dmpGetQuaternion(&q, fifoBuffer[i]);
              mpu.dmpGetGravity(&gravity, &q);
              mpu.dmpGetYawPitchRoll(ypr, &q, &gravity);
              Serial.print("ypr\t");
              Serial.print(ypr[0] * 180/M_PI);
              Serial.print("\t");
              Serial.print(ypr[1] * 180/M_PI);
              Serial.print("\t");
              Serial.println(ypr[2] * 180/M_PI);
          #endif
  
          #ifdef OUTPUT_READABLE_REALACCEL
              // display real acceleration, adjusted to remove gravity
              mpu.dmpGetQuaternion(&q, fifoBuffer[i]);
              mpu.dmpGetAccel(&aa, fifoBuffer[i]);
              mpu.dmpGetGravity(&gravity, &q);
              mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
              Serial.print("areal\t");
              Serial.print(aaReal.x);
              Serial.print("\t");
              Serial.print(aaReal.y);
              Serial.print("\t");
              Serial.println(aaReal.z);
          #endif
  
          #ifdef OUTPUT_READABLE_WORLDACCEL
              // display initial world-frame acceleration, adjusted to remove gravity
              // and rotated based on known orientation from quaternion
              mpu.dmpGetQuaternion(&q, fifoBuffer[i]);
              mpu.dmpGetAccel(&aa, fifoBuffer[i]);
              mpu.dmpGetGravity(&gravity, &q);
              mpu.dmpGetLinearAccel(&aaReal, &aa, &gravity);
              mpu.dmpGetLinearAccelInWorld(&aaWorld, &aaReal, &q);
              Serial.print("aworld\t");
              Serial.print(aaWorld.x);
              Serial.print("\t");
              Serial.print(aaWorld.y);
              Serial.print("\t");
              Serial.println(aaWorld.z);
          #endif
      
          #ifdef OUTPUT_TEAPOT
              // display quaternion values in InvenSense Teapot demo format:
              teapotPacket[2] = fifoBuffer[i][0];
              teapotPacket[3] = fifoBuffer[i][1];
              teapotPacket[4] = fifoBuffer[i][4];
              teapotPacket[5] = fifoBuffer[i][5];
              teapotPacket[6] = fifoBuffer[i][8];
              teapotPacket[7] = fifoBuffer[i][9];
              teapotPacket[8] = fifoBuffer[i][12];
              teapotPacket[9] = fifoBuffer[i][13];
              Serial.write(teapotPacket, 14);
              teapotPacket[11]++; // packetCount, loops at 0xFF on purpose
          #endif
*/
    }
    
        // blink LED to indicate activity
        blinkState = !blinkState;
        digitalWrite(LED_PIN, blinkState);
    }
}
