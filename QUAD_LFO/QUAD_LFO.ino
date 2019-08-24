///////////////////////////////////////////////////////////////
//
//  Quad LFO
//
//  For each LFO:
//
//  A momentary switch to increment through wave tables
//  A potentiometer to control LFO frequency
//  A digital input to re-trigger/sync the LFO
//
//  The LFOs output to pins 11 (LFO1) and 3 (LFO2). Since the output is PWM, it's
//  important to add low-pass filters to these pins to smooth out the resulting waveform
//
//  See accompanying Fritzing documents for circuit information.

//  The MIT License (MIT)
//
//  Copyright (c) 2013-2019 Robert W. Gallup (www.robertgallup.com)
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.
//

// Macros for clearing and setting bits
#define cbi(sfr, bit) (_SFR_BYTE(sfr) &= ~_BV(bit))
#define sbi(sfr, bit) (_SFR_BYTE(sfr) |= _BV(bit))

// Used in calculating frequency tuning word
// Create constant to to only calculate 2^32 once
const unsigned long long POW2TO32 = pow(2, 32);

// Output Pins (PWM - digital pins)
// WARNING: Don't change these!
const byte LFO1_OUTPUT_PIN = 11;
const byte LFO2_OUTPUT_PIN =  3;

// Control Framework
#include "src/CS_Switch.h"
#include "src/CS_Pot.h"

// Waves
#include "wave/noise256.h"
#include "wave/ramp256.h"
#include "wave/saw256.h"
#include "wave/sine256.h"
#include "wave/tri256.h"
#include "wave/pulse8.h"
#include "wave/pulse16.h"
#include "wave/pulse64.h"
#include "wave/sq256.h"

// Wave table pointers
const byte *waveTables[] = {sine256, ramp256, saw256, tri256, pulse8, pulse16, pulse64, sq256, noise256};

#define NUM_WAVES (sizeof(waveTables) / sizeof(byte *))

// LFO Initial wave table numbers
byte LFO_WaveNum1 = 0;
byte LFO_WaveNum2 = 0;
byte LFO_WaveNum3 = 0;
byte LFO_WaveNum4 = 0;

// Optional display
#if defined(DISPLAY)
#include "Display.h"
Display displaySurface;
#endif

#include "Settings.h"

// Calculate LFO frequency ranges
const double LFO1_FREQ_RANGE =    LFO1_FREQ_MAX - LFO1_FREQ_MIN;
const double LFO2_FREQ_RANGE =    LFO2_FREQ_MAX - LFO2_FREQ_MIN;

// Interrupt frequency (16,000,000 / 510)
// 510 is divisor rather than 512 since with phase correct PWM
// an interrupt occurs after one up/down count of the register
// See: https://www.arduino.cc/en/Tutorial/SecretsOfArduinoPWM
const double clock = 31372.549;

// LFO 1 controls
CS_Pot       LFO1_FreqKnob (FREQ1_KNOB_PIN, 3);
CS_Switch    LFO1_WaveSwitch(WAVE1_SWITCH_PIN);

// LFO 2 controls
CS_Pot       LFO2_FreqKnob (FREQ2_KNOB_PIN, 3);
CS_Switch    LFO2_WaveSwitch (WAVE2_SWITCH_PIN);

// LFO 3 controls
CS_Pot       LFO3_FreqKnob (FREQ3_KNOB_PIN, 3);
CS_Switch    LFO3_WaveSwitch(WAVE1_SWITCH_PIN);

// LFO 4 controls
CS_Pot       LFO4_FreqKnob (FREQ4_KNOB_PIN, 3);
CS_Switch    LFO4_WaveSwitch (WAVE2_SWITCH_PIN);

// Generic pin state variable
byte pinState;

// SYNC variables defined for optional SYNC feature, if defined in Settings.h
#if defined(SYNC)
byte lastSYNC1;
#if !defined(SYNC_COMMON)
byte lastSYNC2;
#endif
#endif

// Interrupt variables are volatile
volatile byte tickCounter;               // Counts interrupt "ticks". Reset every 125
volatile byte fourMilliCounter;          // Counter incremented every 4ms

volatile unsigned long accumulatorA;     // Accumulator LFO1
volatile unsigned long LFO1_TuningWord;  // DDS tuning
volatile unsigned long LFO1_Depth;       // CV depth

volatile unsigned long accumulatorB;     // Accumulator LFO2
volatile unsigned long LFO2_TuningWord;  // DDS tuning
volatile unsigned long LFO2_Depth;       // CV depth

volatile unsigned long accumulatorC;     // Accumulator LFO3
volatile unsigned long LFO3_TuningWord;  // DDS tuning
volatile unsigned long LFO3_Depth;       // CV depth

volatile unsigned long accumulatorD;     // Accumulator LFO4
volatile unsigned long LFO4_TuningWord;  // DDS tuning
volatile unsigned long LFO4_Depth;       // CV depth

volatile byte *LFO1_WaveTable;
volatile byte *LFO2_WaveTable;
volatile byte *LFO3_WaveTable;
volatile byte *LFO4_WaveTable;

void setup()
{

#if defined(SYNC)
  // Initialize SYNC pin(s) for optional SYNC feature in Settings.h
  pinMode(SYNC1_PIN, INPUT);
  lastSYNC1 = (HIGH == SYNC1_TRIGGER) ? LOW : HIGH;
#if !defined(SYNC_COMMON)
  pinMode(SYNC2_PIN, INPUT);
  lastSYNC2 = (HIGH == SYNC2_TRIGGER) ? LOW : HIGH;
#endif
#endif

#if defined(DISPLAY)
  // Initialize display
  displaySurface.begin();
#endif

  // Initialize PWM Pins
  pinMode(LFO1_OUTPUT_PIN, OUTPUT);     // pin11= PWM:A
  pinMode(LFO2_OUTPUT_PIN, OUTPUT);     // pin 3= PWM:B

  // Initialize knobs
  LFO1_FreqKnob.begin();
  LFO1_DepthKnob.begin();
  LFO2_FreqKnob.begin();
  LFO2_DepthKnob.begin();


  // Initialize wave tables
  LFO1_WaveTable = (byte*)waveTables[0];
  LFO2_WaveTable = (byte*)waveTables[0];

  // Initialize wave switch states
  while (LFO1_WaveSwitch.stateDebounced() == 0);
  while (LFO2_WaveSwitch.stateDebounced() == 0);

  // Initialize timer
  Setup_timer2();

}


void loop()
{

  // SYNC code included if SYNC option defined in Settings.h
#if defined(SYNC)

  // SYNC 1 + SYNC_COMMON
  pinState = digitalRead(SYNC1_PIN);
  if (pinState != lastSYNC1) {
    lastSYNC1 = pinState;
    if (pinState == SYNC1_TRIGGER) {
      accumulatorA = 0;
#if defined(SYNC_COMMON)
      accumulatorB = 0;
#endif
    }
  }

  // Separate SYNC inputs processed if SYNC1 and SYNC2 pins are different
#if !defined(SYNC_COMMON)
  // SYNC 2 (SYNC pins are separate)
  pinState = digitalRead(SYNC2_PIN);
  if (pinState != lastSYNC2) {
    lastSYNC2 = pinState;
    if (pinState == SYNC2_TRIGGER) accumulatorB = 0;
  }
#endif

#endif

  // Check controls every 1/10 second
  if (fourMilliCounter > 25) {
    fourMilliCounter = 0;

    // If DISPLAY defined in Settings.h
#if defined(DISPLAY)
    displaySurface.update();
#endif

    // LFO 1 wave table switch
    pinState = LFO1_WaveSwitch.stateDebounced();
    if (LFO1_WaveSwitch.changed()) {
      if (pinState == 1) {
        LFO1_WaveNum = (LFO1_WaveNum + 1) % NUM_WAVES;
        LFO1_WaveTable = (byte*)waveTables[LFO1_WaveNum];
      }
    }

    // LFO 2 wave table switch
    pinState = LFO2_WaveSwitch.stateDebounced();
    if (LFO2_WaveSwitch.changed()) {
      if (pinState == 1) {
        LFO2_WaveNum = (LFO2_WaveNum + 1) % NUM_WAVES;
        LFO2_WaveTable = (byte*)waveTables[LFO2_WaveNum];
      }
    }

    // LFO 1 frequency/depth controls
    LFO1_TuningWord = POW2TO32 * (((((double)LFO1_FreqKnob.value() * LFO1_FREQ_RANGE) / 1023L) + LFO1_FREQ_MIN) / clock);
    LFO1_Depth  = LFO1_DepthKnob.value();

    // LFO 2 frequency/depth controls
    LFO2_TuningWord = POW2TO32 * (((((double)LFO2_FreqKnob.value() * LFO2_FREQ_RANGE) / 1023L) + LFO2_FREQ_MIN) / clock);
    LFO2_Depth  = LFO2_DepthKnob.value();
  }

}

//******************************************************************
// timer2 setup
//
void Setup_timer2() {

  // Prescaler 1
  sbi (TCCR2B, CS20);
  cbi (TCCR2B, CS21);
  cbi (TCCR2B, CS22);

  // Non-inverted PWM
  cbi (TCCR2A, COM2A0);
  sbi (TCCR2A, COM2A1);
  cbi (TCCR2A, COM2B0);
  sbi (TCCR2A, COM2B1);

  // Phase Correct PWM
  sbi (TCCR2A, WGM20);
  cbi (TCCR2A, WGM21);
  cbi (TCCR2B, WGM22);

  // Enable interrupt
  sbi (TIMSK2, TOIE2);

}

////////////////////////////////////////////////////////////////
//
// Timer2 Interrupt Service
// Frequency = 16,000,000 / 510 = 31372.5
//
ISR(TIMER2_OVF_vect) {

  byte offset;

  // Count every four milliseconds
  if (tickCounter++ == 125) {
    fourMilliCounter++;
    tickCounter = 0;
  }

  // Sample wave table for LFO1
  accumulatorA  += LFO1_TuningWord;
  offset         = accumulatorA >> 24; // high order byte
  OCR2A          = (pgm_read_byte_near(LFO1_WaveTable + offset) * LFO1_Depth) / 1024L;

  // Sample wave table for LFO2
  accumulatorB  += LFO2_TuningWord;
  offset         = accumulatorB >> 24; // high order byte
  OCR2B          = (pgm_read_byte_near(LFO2_WaveTable + offset) * LFO2_Depth) / 1024L;

}
