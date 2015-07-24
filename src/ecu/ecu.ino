#include <SPI.h>

/*
 *  Name: ecu.c
 *  Authors: Ivan Pachev and Alex Pink
 *
 *  Description:
 *  A finite state machine designed to control the Honda GX35 engine with 
 *  an EcoTrons interfacing kit. For use with the GX35 EFI Shield and 
 *  Arduino Due.
 *
 *  Note to the programmer:
 *  Please use Arduino IDE to compile / upload sketches! To use the built-in
 *  Arduino libraries, the IDE must be used. The Makefile and code directory
 *  structure is too convoluted to use.
 *
 *  TODO LIST:
 *  calibrate Eco Trons injector & MASS_FLOW_RATE
 *  O2 sensor working
 *  temp sensor linear regression
 *  RPM weighting
 *
 */
#include <Arduino.h>
#include <DueTimer.h>
#include <stdio.h>
#include "ecu.h"
#include "table.h"
#include "tuning.h"
#include "spi_adc.h"
#include "thermistor.h"

/***********************************************************
*                      D E F I N E S
***********************************************************/
// Mode Definitions
#define DIAGNOSTIC_MODE
//#define DEBUG_MODE

// Pin Definitions 
#define KILLSWITCH_IN   9
#define MAP_IN          A0
#define ECT_IN          A8
#define IAT_IN          A7
#define O2_IN           A1
#define TPS_IN          A11
#define TACH_IN         5
#define SPARK_OUT       7
#define FUEL_OUT        8
#define DAQ_CS          4

// Timers
#define FUEL_START_TIMER        Timer0
#define FUEL_STOP_TIMER         Timer1
#define SPARK_CHARGE_TIMER      Timer2
#define SPARK_DISCHARGE_TIMER   Timer3

// Serial
#define SERIAL_PORT Serial

// SPI
#define MAP_ADC_CHNL 4
#define O2_ADC_CHNL  0
#define IAT_ADC_CHNL 2
#define ECT_ADC_CHNL 3
#define TPS_ADC_CHNL 1


/***********************************************************
*             G L O B A L   V A R I A B L E S
***********************************************************/ 
// State Machine
typedef enum {
    READ_SENSORS,
    CALIBRATION,
    CRANKING,
    RUNNING,
    REV_LIMITER,
    SERIAL_OUT
} state;

volatile state currState;

// Booleans
volatile bool killswitch;
volatile bool fuelCycle;
bool revLimit;

// Serial 
static char serialOutBuffer[100];
volatile int serialPrintCount;

// Sensor Readings
volatile float MAPval;  // Manifold Absolute Pressure reading, w/ calibration curve [kPa]
volatile float ECTval;   // Engine Coolant Temperature reading, w/ calibration curve [K]
volatile float IATval;   // Intake Air Temperature reading, w/ calibration curve [K]
volatile float TPSval;   // Throttle Position Sensor Reading [%]
volatile float O2val;    // Oxygen Sensor Reading, w/ calibration curve [AFR]

// Thermistors
struct thermistor ECT = {-45.08, 95.22, 30, 100, 5.0, 1.2};
struct thermistor IAT = {-45.08, 88.83, 30, 100, 5.0, 1.2};

// Real Time Stuff
volatile float currAngularSpeed;             // current speed [degrees per microsecond]
volatile float currRPM;                      // current revolutions per minute [RPM]
volatile unsigned int calibAngleTime;        // time of position calibration [microseconds]
volatile unsigned int lastCalibAngleTime;    // last posiition calibration time, for RPM calcs [microseconds]
float volEff;                       // volumetric efficiency [% out of 100]
float airVolume;                    // volume of inducted air [m^3]
float airMolar;                     // moles of inducted air [mols]
float fuelMass;                     // mass of fuel to be injected [g]
float fuelDuration;                 // length of injection pulse [us]
float fuelDurationDegrees;          // length of injection pulse [degrees]
volatile float sparkChargeAngle;             // when to start inductively charging the spark coil [degrees]
volatile float sparkDischargeAngle;          // when to discharge the coil, chargeAngle + DWELL_TIME [degrees]
volatile float fuelStartAngle;               // when to start the injection pulse [degrees]
float currEngineAngle;              // current engine position [degrees]


/***********************************************************
*                  F U N C T I O N S   
***********************************************************/ 
float readTemp(struct thermistor therm, int adc_channel){
    // [K] =                                    [C]                        +       [K]                  
    return   thermistorTemp(therm, readADC(adc_channel)*VOLTS_PER_ADC_BIT) + CELSIUS_TO_KELVIN;
}

float readMAP(void){
    //     [kPa]     =           [ADC]       * [V/ADC]           
    float MAPvoltage = readADC(MAP_ADC_CHNL) * VOLTS_PER_ADC_BIT;

    if( MAPvoltage < 0.5f )
        return 20.0f;
    else if( MAPvoltage > 4.9f )
        return 103.0f;
    else                  
        //          [V]   * [kPa/V] + [kPa]
        return MAPvoltage * 18.86   + 10.57;
}

float readO2(void){
    // [kg/kg] = (           [ADC]     *      [V/ADC]     ) * [(kg/kg)/V] + [kg/kg]
    return       (readADC(O2_ADC_CHNL) * VOLTS_PER_ADC_BIT) * 3.008       + 7.35;
}

/* From experimentation,
    Low TPS rawVal: 301
    High TPS rawVal: 3261
*/

float readTPS(void){
    int rawTPS = readADC(TPS_ADC_CHNL);

    if( rawTPS < TPS_RAW_MIN )
        return 0.0;
    else if( rawTPS > TPS_RAW_MAX )
        return 1.0;
    else
        return ((float)rawTPS - TPS_RAW_MIN) / (TPS_RAW_MAX - TPS_RAW_MIN);
}

float getCurrAngle(float angular_speed, unsigned int calib_time){
    //[degrees] = [degrees/us]  * (  [us]   -     [us]  ) +  [degrees]
    float angle = angular_speed * (micros() - calib_time) + CALIB_ANGLE;
    // always keep the engine angle less than 360 degrees
    return angle >= 360 ? angle - 360 : angle;
}

float injectorPulse(float airVol, float currMAP){
    // calculate moles of air inducted into the cylinder
    // [n]    =   [m^3] * ([kPa]   * [Pa/1kPa]) / ([kg/(mol*K)] * [K] )
    airMolar  = airVol  * (currMAP * 1E-3)      / (R_CONSTANT   * IATval);

    // calculate moles of fuel to be injected 
    // [g fuel] =  [n air] *  [g/n air]     / [g air / g fuel]
    fuelMass    = airMolar * MOLAR_MASS_AIR / AIR_FUEL_RATIO;

    // calculate fuel injection duration in microseconds
    // [us] =  [g]  * [kg/1E3 g] * [1E6 us/s] / [kg/s]
    return fuelMass * 1E3 / MASS_FLOW_RATE;     
}

float timeToStartFueling(float fuelDuration, float angularSpeed, float engineAngle){
    // calculate the angle at which to begin fuel injecting
    // [degrees]   =   [degrees]    - (    [us]     *   [degrees/us]  )
    fuelStartAngle = FUEL_END_ANGLE - (fuelDuration * angularSpeed); 

    // determine when [in us] to start the fuel injection pulse and set a timer appropriately
    //                      (  [degrees]    -    [degrees]   ) /   [degrees/us]   
    return ( (fuelStartAngle - engineAngle) / angularSpeed );
}
float timetoChargeSpark(float sparkDischargeAngle, float currAngularSpeed, float currEngineAngle){

    sparkChargeAngle = sparkDischargeAngle - DWELL_TIME * currAngularSpeed; // calculate angle at which to begin charging the spark

    // determine when [in us] to start charging the coil and set a timer appropriately
    //        (   [degrees]    -    [degrees]   ) /    [degrees/us]
    return ( (sparkChargeAngle - currEngineAngle) / currAngularSpeed ); 
}


/***********************************************************
*      A R D U I N O    S E T U P    F U N C T I O N
***********************************************************/ 
void setup(void){
    // Set 12-bit ADC Readings
    //analogReadResolution(12);

    // Pin directions
    pinMode(KILLSWITCH_IN, INPUT);
    pinMode(TACH_IN, INPUT);
    pinMode(SPARK_OUT, OUTPUT);
    pinMode(FUEL_OUT, OUTPUT);

    digitalWrite(SPARK_OUT, LOW);
    digitalWrite(FUEL_OUT, LOW);

    // Initialize Variables
    currAngularSpeed = 0;
    calibAngleTime = 0;             
    lastCalibAngleTime = 0;         

    // set up all interrupts and timers
    attachInterrupt(KILLSWITCH_IN, killswitch_ISR, CHANGE);
    attachInterrupt(TACH_IN, tach_ISR, FALLING);
    SPARK_CHARGE_TIMER.attachInterrupt(chargeSpark_ISR);        
    SPARK_DISCHARGE_TIMER.attachInterrupt(dischargeSpark_ISR);
    FUEL_START_TIMER.attachInterrupt(startFuel_ISR);
    FUEL_STOP_TIMER.attachInterrupt(stopFuel_ISR); 

    // start Serial communication
    SERIAL_PORT.begin(115200);
    serialPrintCount = 1;   // wait 5 cycles before printing any information

    // set up SPI communication to the MCP3304 DAQ
    initSPI();

    killswitch = digitalRead(KILLSWITCH_IN);
    fuelCycle = false;          // start on a non-fuel cycle (arbitrary, since no CAM position sensor)
    revLimit = false;
    currState = READ_SENSORS;   // start off by reading sensors
}

/***********************************************************
*       A R D U I N O    L O O P    F U N C T I O N
***********************************************************/ 
void loop(void){

    #ifdef DIAGNOSTIC_MODE

    char serialChar;
    static bool commandLock = 0;

    int MAPraw = readADC(MAP_ADC_CHNL);
    int IATraw = readADC(IAT_ADC_CHNL);
    int ECTraw = readADC(ECT_ADC_CHNL);
    int TPSraw = readADC(TPS_ADC_CHNL);
    int O2raw  = readADC(O2_ADC_CHNL);
    int channelTest = readADC(7);

    sprintf(serialOutBuffer, "%d        %d       %d        %d        %d        %d", 
    MAPraw, ECTraw, IATraw, TPSraw, O2raw, channelTest);

    SERIAL_PORT.println("MAP(raw):  ECT(raw):  IAT(raw):  TPS(raw):  O2(raw):  Test:");
    SERIAL_PORT.println(serialOutBuffer);

    MAPval = readMAP();
    IATval = readTemp(IAT, IAT_ADC_CHNL);
    ECTval = readTemp(ECT, ECT_ADC_CHNL);
    TPSval = readTPS();
    O2val  = readO2();
    killswitch = digitalRead(KILLSWITCH_IN);
    int tachRaw = digitalRead(TACH_IN);

    sprintf(serialOutBuffer, "%2f    %2f    %2f    %2f    %2f        %d        %d", 
    MAPval,(ECTval-CELSIUS_TO_KELVIN), (IATval-CELSIUS_TO_KELVIN), TPSval, killswitch, tachRaw);

    SERIAL_PORT.println("MAP(kPa):    ECT(C):       IAT(C):       TPS(%):   O2(kg/kg):  KillSW:  Tach:");
    SERIAL_PORT.println(serialOutBuffer);
    SERIAL_PORT.println("\n");

    if( SERIAL_PORT.available() ){

        serialChar = SERIAL_PORT.read();

        if(serialChar == 'l'){
            commandLock = true;
            SERIAL_PORT.println("Unlocked.");
        }
        else if(serialChar == 's' && commandLock){
            SERIAL_PORT.println("Performing spark ignition event test in 2s.");
            SPARK_CHARGE_TIMER.start(2000000);
            commandLock = false;
        }
        else if(serialChar == 'f' && commandLock){
            SERIAL_PORT.println("Performing fuel ignition pulse test in 2s.");
            FUEL_START_TIMER.start(2000000);
            fuelDuration = 10000;
            commandLock = false;
        }
    }

    delay(1000);

    #else

    switch(currState){

        // Constantly poll the all sensors. The "default" state (all state flows eventually lead back here).
        case READ_SENSORS:

            currState = READ_SENSORS;

            MAPval = readMAP();
            IATval = readTemp(IAT, IAT_ADC_CHNL);
            //ECTval = readECT();
            TPSval = readTPS();
            O2Val  = readO2();

        break;

        // when the TACH ISR determines it has reached its calibration angle, this state is selected. This state directs the
        // flow of the state machine.
        case CALIBRATION:

            switch(killswitch){
                case true:
                    currRPM = convertToRPM(currAngularSpeed);

                    if( revLimit ){
                        if( currRPM < LOWER_REV_LIMIT){
                            revLimit = false;
                            currState = RUNNING;
                            #ifdef DEBUG_MODE
                            SERIAL_PORT.println("INFO: rev limiter deactivated");
                            #endif
                        }
                        else
                            currState = READ_SENSORS;
                    }
                    else{
                        if( currRPM >= UPPER_REV_LIMIT )
                            currState = REV_LIMITER;
                        else if( currRPM < UPPER_REV_LIMIT && currRPM >= CRANKING_SPEED )
                            currState = RUNNING;
                        else if( currRPM < CRANKING_SPEED && currRPM >= ENGAGE_SPEED )
                            currState = CRANKING;
                        else
                            currState = READ_SENSORS;
                    }
                break;

                case false:
                    currState = READ_SENSORS;
                break;
            }

        #ifdef DEBUG_MODE
        switch(currState){
            case READ_SENSORS:
                SERIAL_PORT.println("INFO: state machine to READ_SENSORS");
            case CRANKING:
                SERIAL_PORT.println("INFO: state machine to CRANKING");
            case RUNNING:
                SERIAL_PORT.println("INFO: state machine to RUNNING");
            case REV_LIMITER:
                SERIAL_PORT.println("INFO: state machine to REV_LIMITER");
            default:
                SERIAL_PORT.println("INFO: state machine to READ_SENSORS");
        }
        #endif

        break;

        // CRANKING uses hardcoded values for Volumetric Efficiency and Spark Advance angle. It is an engine
        // enrichment algorithm that usually employs a rich AFR ratio.
        case CRANKING:

            currState = (serialPrintCount == 0 ? SERIAL_OUT : READ_SENSORS); 

            if(fuelCycle){
                // calculate volume of air inducted into the cylinder, using hardcoded cranking VE
                // [m^3]  =    [%]         *        [m^3]         
                airVolume =  CRANK_VOL_EFF * ENGINE_DISPLACEMENT;

                // Calculate fuel injector pulse length
                fuelDuration = injectorPulse(airVolume, MAPval);

                // update current angular position
                currEngineAngle = getCurrAngle(currAngularSpeed, calibAngleTime);

                // calculate and set the time to start injecting fuel
                FUEL_START_TIMER.start( timeToStartFueling(fuelDuration, currAngularSpeed, currEngineAngle) );
            }

            // find out at what angle to begin/end charging the spark
            sparkDischargeAngle = TDC - CRANK_SPARK_ADV;    // hardcoded cranking spark advance angle

            // update current angular position again, for timer precision
            currEngineAngle = getCurrAngle(currAngularSpeed, calibAngleTime);

            // calculate and set the time to start charging the spark coil
            SPARK_CHARGE_TIMER.start( timetoChargeSpark(sparkDischargeAngle, currAngularSpeed, currEngineAngle) );

        break;

        // This state runs during the normal running operation of the engine. The Volumetric Efficiency and 
        // Spark Advance are determined using fuel map lookup tables. 
        case RUNNING:

            currState = (serialPrintCount == 0 ? SERIAL_OUT : READ_SENSORS);

            if(fuelCycle){
                // table lookup for volumetric efficiency 
                volEff = table2DLookup(&VETable, convertToRPM(currAngularSpeed), MAPval);

                // calculate volume of air inducted into the cylinder
                // [m^3]  =    [%]  *        [m^3]         
                airVolume =  volEff * ENGINE_DISPLACEMENT;
    
                // Calculate fuel injector pulse length
                fuelDuration = injectorPulse(airVolume, MAPval); 

                // update current angular position
                currEngineAngle = getCurrAngle(currAngularSpeed, calibAngleTime);

                // calculate and set the time to start injecting fuel
                FUEL_START_TIMER.start( timeToStartFueling(fuelDuration, currAngularSpeed, currEngineAngle) );
            }

            // find out at what angle to begin charging the spark coil
            sparkDischargeAngle = TDC - table2DLookup(&SATable, convertToRPM(currAngularSpeed), MAPval);  // calculate spark advance angle

            // update current angular position again, for timer precision
            currEngineAngle = getCurrAngle(currAngularSpeed, calibAngleTime);
            
            // calculate and set the time to start charging the spark coil
            SPARK_CHARGE_TIMER.start( timetoChargeSpark(sparkDischargeAngle, currAngularSpeed, currEngineAngle) );

        break;

        // When the engine's RPM is greater than UPPER_REV_LIMIT, the engine must enact a rev limiter algorithm,
        // to prevent possible damage to the engine internals / hardware. The engine doesn't fuel or spark.
        case REV_LIMITER:

            currState = (serialPrintCount == 0 ? SERIAL_OUT : READ_SENSORS);

            #ifdef DEBUG_MODE
            SERIAL_PORT.println("INFO: rev limiter activated");
            #endif

            revLimit = true;

        break;

        case SERIAL_OUT:

            currState = READ_SENSORS;
        
            sprintf(serialOutBuffer, "%5f    %3f         %3f      %4f           %5d", 
                convertToRPM(currAngularSpeed), MAPval, volEff, sparkDischargeAngle, fuelDuration);

            SERIAL_PORT.println("RPM:   MAP(kPa):   VE(%):   SPARK(deg):   FUEL PULSE(us):");
            SERIAL_PORT.println(serialOutBuffer);

        break;
    }

    #endif
}


/***********************************************************
*   I N T E R R U P T   S E R V I C E    R O U T I N E S
***********************************************************/
void tach_ISR(void){

    lastCalibAngleTime = calibAngleTime;
    calibAngleTime = micros();
    currAngularSpeed = calcSpeed(calibAngleTime, lastCalibAngleTime, currAngularSpeed);

    // all of the following code assumes one full rotation has occurred (with a single toothed
    // crankshaft, this is true)

    fuelCycle = !fuelCycle;
    serialPrintCount = (serialPrintCount >= 9 ? serialPrintCount = 0 : serialPrintCount + 1);

    currState = CALIBRATION;
}

void killswitch_ISR(void){
    killswitch = digitalRead(KILLSWITCH_IN);
}

void chargeSpark_ISR(void){
    SPARK_CHARGE_TIMER.stop();
    // start chargine coil
    digitalWrite(SPARK_OUT, HIGH);
    // set discharge timer for dwell time
    SPARK_DISCHARGE_TIMER.start(DWELL_TIME);

    #ifdef DIAGNOSTIC_MODE
    SERIAL_PORT.println("Charging spark.");
    #endif

}

void dischargeSpark_ISR(void){
    // discharge coil
    digitalWrite(SPARK_OUT, LOW);
    SPARK_DISCHARGE_TIMER.stop();

    #ifdef DIAGNOSTIC_MODE
    SERIAL_PORT.println("Discharging spark.");
    #endif
}

void startFuel_ISR(void){
    FUEL_START_TIMER.stop();
    // start injecting fuel
    digitalWrite(FUEL_OUT, HIGH);
    // set discharge timer for dwell time
    FUEL_STOP_TIMER.start(fuelDuration);

    #ifdef DIAGNOSTIC_MODE
    SERIAL_PORT.println("Starting fuel pulse for 10ms.");
    #endif
}

void stopFuel_ISR(void){
    // stop injecting fuel
    digitalWrite(FUEL_OUT, LOW);
    FUEL_STOP_TIMER.stop();

    #ifdef DIAGNOSTIC_MODE
    SERIAL_PORT.println("Ending fuel pulse.");
    #endif
}
