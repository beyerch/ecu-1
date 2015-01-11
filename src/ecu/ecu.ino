#include "DueTimer.h"
#include "table.h"

#define DIAGNOSTIC_MODE 1

#define TRUE 1
#define FALSE 0

#define TAC_IN  3  // pin used for tachometer
#define MAP_IN   2  // pin used for manifold air pressure

#define FUEL_OUT 7  // pin used for fuel injection
#define SPARK_OUT 5  // pin used for spark

#define FUEL_TIMER Timer0
#define SPARK_TIMER Timer1

#define DWELLTIME 5000 // spark coil dwell time in us
#define TDC 360      // crankshaft top dead center in degrees
#define GRACE 0      // degrees between finish fuel inject and discharge spark

#define CALIB_ANGLE 0            // angle of the first tooth after the missing one
#define ANGLE_PER_TOOTH 360.0f   // angle distance between teeth

#define ENGINE_DISPLACEMENT 49  // volume of the engine in cubic centimeters
#define AMBIENT_TEMP 298        // ambient temperature in kelvin
#define R_CONSTANT 287          // R_specific for dry air in J/kg/K
#define AIR_FUEL_RATIO 14.7f      // air to fuel ratio for octane (MAY NEED REVISION)
#define MASS_FLOW_RATE  0.0006f         // fuel injection flow rate in kg/s

volatile float instantDPMS;     //pseudoinstantaneous angular velocity in degrees per us

volatile char fuelOpen;       // whether the fuel injector is open
volatile char chargingSpark;  // whether the spark is charging

volatile char useFuel;        // whether or not to use fuel (only fuel every other cycle)

volatile int fuelDuration;    // how long to fuel inject

volatile int lastTick;        // last tachometer interrupt
volatile int lastTickDelta;   // time difference (us) between last tac interrupt and the previous one
volatile int prevTick;        // previous tachometer interrupt
volatile int prevTickDelta;   // previous lastTickDelta
volatile int predictedTickDelta; //predicated tickDelta 

// not needed volatile int toothCount;      // which tooth we're at on the crankshaft
volatile float lastToothAngle;  // angle of the last tooth that passed by

volatile char recalc;         // flag to recalculate stuff after spark for next cycle


/////////////////////////////////////////////////////////
/*               DIAGNOSTIC VARS                       */
/////////////////////////////////////////////////////////


#ifdef DIAGNOSTIC_MODE

volatile int numSparks;

volatile int sparkChargeStart;
volatile int sparkChargeEnd;
volatile float actualSparkChargeAngle;
volatile float actualSparkAngle;

volatile int fuelStart;
volatile int fuelEnd;
volatile float actualFuelStartAngle;
volatile float actualFuelEndAngle;

#endif

///////////////////////////////////////////////////////////////

float approxAngle;

float sparkAdvAngle;    // angle at which to discharge the spark
float sparkChargeAngle; // angle at which to begin charging the spark
unsigned int sparkChargeTime;    // in how long to begin charging the spark

float fuelStartAngle;   // angle at which to begin injecting fuel
unsigned int fuelStartTime;      // in how long to begin injecting fuel
float fuelEndAngle;     // angle at which to stop injecting fuel
float fuelDurationAngle;  // duration of time to inject fuel

double airVolume;        // volume of air that the engine will intake in m^3
float mapVal;              // manifold air pressure in kPa
int mapPulseHigh;

/////////////////////////////////////////////////////////////////

void setup() {

    Serial.begin(115200);

    pinMode(TAC_IN, INPUT);
    pinMode(MAP_IN, INPUT);

    pinMode(FUEL_OUT, OUTPUT);
    pinMode(SPARK_OUT, OUTPUT);

    chargingSpark = FALSE;
    fuelOpen = FALSE;

    useFuel = TRUE;            // use fuel on the first cycle and every other cycle thereafter
    recalc = TRUE;

    attachInterrupt(TAC_IN, tacISR, RISING); // set up the tachometer ISR
    SPARK_TIMER.attachInterrupt(sparkISR);    // set up the spark ISR
    FUEL_TIMER.attachInterrupt(fuelISR);      // set up the fuel injection ISR

}

void loop() {

    if (recalc) {

#ifdef DIAGNOSTIC_MODE
        Serial.print("instant dpms");
        Serial.println(instantDPMS, 9);
        Serial.print("times sparked ");
        Serial.println(numSparks);

        Serial.print("spark charge begin angle ");
        Serial.println(sparkChargeAngle);
        Serial.print("ACTUAL spark charge angle ");
        Serial.println(actualSparkChargeAngle);
        Serial.print("spark adv ");
        Serial.println(sparkAdvAngle);
        Serial.print("ACTUAL spark angle ");
        Serial.println(actualSparkAngle);
        Serial.print("spark charge duration: ");
        Serial.println(sparkChargeEnd - sparkChargeStart);

        Serial.print("fuel start angle ");
        Serial.println(fuelStartAngle);
        Serial.print("actual fuel start angle ");
        Serial.println(actualFuelStartAngle);
        Serial.print("fuel end angle ");
        Serial.println(fuelEndAngle);
        Serial.print("actual fuel end ");
        Serial.println(actualFuelEndAngle);
        Serial.print("fuel injection duration ");
        Serial.println(fuelDuration);
        Serial.print("actual fuel injection duration ");
        Serial.println(fuelEnd - fuelStart);

        Serial.println("");
#endif


        mapPulseHigh = pulseIn(MAP_IN, HIGH);    // virtual engine uses unfiltered PWM, so we don't use ADC
        mapVal = (float)mapPulseHigh / (float)(mapPulseHigh + pulseIn(MAP_IN, LOW)); // read in manifold air pressure

        sparkAdvAngle = TDC - tableLookup(&SATable, instantDPMS * 166667, mapVal);  // calculate spark advance angle

        // calculate volume of air to be taken in in m^3
        
        airVolume = tableLookup(&VETable, instantDPMS * 166667, mapVal * 100) * ENGINE_DISPLACEMENT;
        Serial.print("airvolume ");
        Serial.println(airVolume);
	//TODO
        // add constants for amount of fuel that is inject as the injector is opening  and closing (then calculate how much fuel to inject)
        fuelDuration = airVolume * mapVal * 1013 / (R_CONSTANT * AMBIENT_TEMP * AIR_FUEL_RATIO * MASS_FLOW_RATE);
        Serial.print("fuelduration ");
        Serial.println(fuelDuration);

        fuelDurationAngle = fuelDuration * instantDPMS; // calculate the angular displacement during fuel injection
        fuelEndAngle = TDC - 60;
        //fuelEndAngle = sparkAdvAngle - GRACE;           // calculate the angle at which to stop fuel injecting
        fuelStartAngle = fuelEndAngle - fuelDurationAngle; // calculate the angle at which to begin fuel injecting

        sparkChargeAngle = sparkAdvAngle - DWELLTIME * instantDPMS; // calculate angle at which to begin charging the spark

        // figure out in how long we need to begin charging spark (how long until sparkChargeAngle)
        approxAngle = lastToothAngle + (micros() - lastTick) * instantDPMS;
        sparkChargeTime = (sparkChargeAngle - approxAngle) / instantDPMS;

        // check if we haven't already started charging the spark (i.e. if the spark charge time is in the future)
        if (sparkChargeTime > 128 && !chargingSpark)
            SPARK_TIMER.start(sparkChargeTime-127); // set timer to begin charging spark on time

        // check if we need to fuel on the current cycle
        if (useFuel) {
            // figure out in how long we need to begin fuel injecting
            approxAngle = lastToothAngle + (micros() - lastTick) * instantDPMS;
            fuelStartTime = (fuelStartAngle - approxAngle) / instantDPMS;

            // check if we haven't already begun fueling (i.e. if the fuel start time is in the future)
            if (fuelStartTime > 128 && !fuelOpen)
                FUEL_TIMER.start(fuelStartTime-127); // set timer to begin injecting on time
        }
        recalc = FALSE;
    }
}

//fuel injection
void fuelISR()
{
    FUEL_TIMER.stop();   // prevent timer from restarting

    if (fuelOpen)  // if currently injecting fuel
    {
        digitalWrite(FUEL_OUT, LOW);  // close fuel injector
#ifdef DIAGNOSTIC_MODE
        fuelEnd = micros();
        actualFuelEndAngle = lastToothAngle + (fuelEnd - lastTick) * instantDPMS;
#endif
        fuelOpen = FALSE;             // no longer injecting fuel
    }
    else           // if currently not injecting fuel
    {
        digitalWrite(FUEL_OUT, HIGH); // open fuel injector
#ifdef DIAGNOSTIC_MODE
        fuelStart = micros();
        actualFuelStartAngle = lastToothAngle + (fuelStart - lastTick) * instantDPMS;
#endif
        fuelOpen = TRUE;              // currently injecting fuel
        if(fuelDuration > 128)
        	FUEL_TIMER.start(fuelDuration-127);  // inject for fuel duration and then stop
        else
        	FUEL_TIMER.start(1);
    }
}

//spark advance
void sparkISR()
{
    SPARK_TIMER.stop();  // prevent timer from restarting

    if (chargingSpark)   // if charging, time to discharge!
    {
        // send signal to discharge
        digitalWrite(SPARK_OUT, LOW);
#ifdef DIAGNOSTIC_MODE
        sparkChargeEnd = micros();
        actualSparkAngle = lastToothAngle + (sparkChargeEnd - lastTick) * instantDPMS;
#endif
        chargingSpark = FALSE;  // no longer charging
        recalc = TRUE;          // time to begin recalculating for the new cycle
        useFuel = !useFuel;     // if we just fueled, not necessary to fuel on the next cycle
        numSparks++;

    }
    else                 // if not charging, time to charge
    {
        // send signal to begin charge
        digitalWrite(SPARK_OUT, HIGH);
#ifdef DIAGNOSTIC_MODE
        sparkChargeStart = micros();
        actualSparkChargeAngle = lastToothAngle + (sparkChargeStart - lastTick) * instantDPMS;
#endif
        chargingSpark = TRUE;   // currently charging spark
        SPARK_TIMER.start(DWELLTIME-127); // discharge after DWELLTIME us
    }
}

// tachometer
void tacISR()
{
    prevTick = lastTick;    // keep track of the previous tachometer tick.
    lastTick = micros();    // record the current tachometer tick

    prevTickDelta = lastTickDelta;
    lastTickDelta = lastTick - prevTick; // calculate time between lastTick and prevTick
    predictedTickDelta = 2 * lastTickDelta - prevTickDelta;

    lastToothAngle = CALIB_ANGLE;
    // calculate the angular velocity using the time between the last 2 ticks
    instantDPMS = ANGLE_PER_TOOTH / predictedTickDelta;
}
