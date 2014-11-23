#include <avr/io.h>
#include <avr/interrupt.h>
#include "table.h" 

//input
#define MAP PD5 //manifold air pressure
#define RPM PD6  //RPM
//output
#define FUEL PD1 
#define SPARK PD2


#define DWELLTIME 10 //n microseconds
#define FUELTIME 10 //n amount of fuel per unit time
#define ANGLEDISTANCE 10 //n angular distance between pins
#define SPARKOFFSET 10//allowable difference for spark offset (in time milliseconds)
#define TOOTHOFFSET 10//allowable deviation between pin detection (in time milliseconds)
#define TOOTHNUM 10 //number of teeth
#define TDC 360 //top dead center in degrees
#define GRACE 10 // degrees between release of spark charge and closing fuel injector
#define CONFIGTIMEROFFSET 15 //degrees to configure the timers  within

volatile char fuelOpen; //0 or 1
volatile int fuelDurration; //amount of fuel (translates to time to keep injector open)

volatile char charging;

volatile float curAngle; //last known angle
volatile float avgTime; //average time between tics

volatile int lastTime; //last counted time

volatile int toothCount; // tooth count
volatile float approxAngle; //current approximate angle 

volatile char valSet; //set to false after the spark has been fired so that new values are calculated other wise it will be true so that we only calculate the spark angle and amount of fuel once and not every run through of the main loop

//fuel injection
ISR(TIMER0_COMPA_vect)
{
   if(fuelOpen)
   {
      //close fuel injector
      fuelOpen = 0;
   }
   else
   {
      //open fuel injector
      fuelOpen = 1;
      //run timer for fuel Duration 
   }
}

//spark advance
ISR(TIMER2_COMPA_vect)
{
   if(!charging)
   {
      charging = 1;
      //run timer for dwell time
   }
   else
   {
      //discharge
      charging = 0;
      //flag to recalculate when to fire spark and how much fuel to inject
      valSet = 0;
   }
}

void tacISR()
{
   lastTime = //timer value * conversion factor to microseconds
   int diffrence = lastTime - (2*avgTime);
   if(diffrence < 0)
      diffrence *= -1;
   if(diffrence <= TOOTHOFFSET)
   {
      toothCount = 0;
      curAngle = 0;
   }
   else
   {
      toothCount++;
      curAngle = toothCount * ANGLEDISTANCE;
      avgTime = (lastTime + avgTime) / 2
   }
   approxAngle = curAngle;
   //run timer for a while
}

int main(void)
{
   float sparkAngle; //angle to release the spark at
   float fuelAngle; //angle to stop fueling at
   int airVolume;
   int rpm;
   int map;

   //angles to set timers at
   float fuelStart;
   float sparkStart;
   char timerSet = 0;
   //initialize timers
   noInterrupts();
   attachInterrupt(0,tacISR, CHANGE);
   interrupts();

   //configure timer 0 timer 2
   curAngle = approxAngle = 0;
   valSet = 0;
   for(;;)
   {
      //read RPM MAP
      approxAngle = curAngle + //timer value converted to microseconds * RPM converted to rotations per microsecond

      //if we have not calculated when to fire the spark and how much fuel to inject do calculations
      if(!valSet)
      {
	 sparkAngle = TDC - tableLookup(SATable,rpm, map );
	 fuelAngle = sparkAngle - GRACE;
	 airVolume = tableLookup(VETable, rpmValue, mapValue);
	 fuelAmmount = //equation for fuel amount from air volume, convert to time (microseconds)

	 fuelStart = fuelAngle - (fuelAmmount * //RPM converted to rotations per microsecond)
	 sparkStart = sparkAngle - (DWELLTIME * //RPM converted to rotations per microsecond;
	 timerSet = 0;
	 valSet = 1;
      }
      //if the timers are not yet set
      if(!timerSet)
      {
	 //if we are close enough to where we need to start fueling and the fuel is not already open
	 if(fuelStart - approxAngle <= CONFIGTIMEROFFSET && !fuelOpen)
	 {
	    fuelOpen = 1;
	    //start fuel timer
	 }
	 //if we are close enough to where we need to start charging the spark and the spark is not already charging
	 if(sparkStart - approxAngle <= CONFIGTIMEROFFSET && !charging)
	 {
	    charging = 1;
	    //start spark timer
	 }

	 //the timers are set
	 if(charging && fuelOpen)
	 {
	    timerSet = 1;
	 }
      }     
   }
}
