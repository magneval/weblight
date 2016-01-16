// Copyright 2015 Mike Tsao
//
// WebLight firmware
// https://github.com/sowbug/weblight

#include "sequencer.h"

#include "eeprom.h"
#include "led_control.h"
#include <stdbool.h>

#define PROGRAM_SIZE_MAX (128)
uint8_t opcodes[PROGRAM_SIZE_MAX];
uint8_t oi = 0;
uint8_t opcode_count = 0;
uint8_t is_playing = false;
uint8_t is_recording = false;
uint16_t elapsed_since_last_cycle_msec = 0;
Transition current_transition = NONE;
uint16_t current_transition_duration_msec = 0;
uint8_t is_transition_in_progress = false;
uint16_t remaining_transition_duration_msec = 0;
uint16_t pause_duration_msec = 0;

// See http://goo.gl/dj7Glt for more than you ever wanted to know
// about division on the AVR.
int32_t current_r, current_g, current_b;
int32_t delta_r, delta_g, delta_b;
int32_t end_r, end_g, end_b;

uint8_t IsRecording() { return is_recording; }

static void VerifySequenceCapacity(uint8_t opcodes) {
  if (opcode_count + opcodes >= PROGRAM_SIZE_MAX) {
    // This will wake up the watchdog.
    StatusBlink(255);
  }
}

static uint8_t ProcessTransition() {
  if (!is_transition_in_progress) {
    return false;
  }
  if (remaining_transition_duration_msec > elapsed_since_last_cycle_msec) {
    remaining_transition_duration_msec -= elapsed_since_last_cycle_msec;
  } else {
    remaining_transition_duration_msec = 0;
  }
  if (remaining_transition_duration_msec == 0) {
    is_transition_in_progress = false;
    SetLEDs(end_r >> 8, end_g >> 8, end_b >> 8);
    return false;
  }

  // TODO: in-progress transition should be completely copied to
  // another space so that we can change the next transition while
  // it's happening
  switch (current_transition) {
  case NONE:
    break;
  case LINEAR_RGB:
    current_r += delta_r * elapsed_since_last_cycle_msec;
    current_g += delta_g * elapsed_since_last_cycle_msec;
    current_b += delta_b * elapsed_since_last_cycle_msec;
    SetLEDs(current_r >> 8, current_g >> 8, current_b >> 8);
    break;
  }
  return true;
}

void HandleCOLOR(uint8_t r, uint8_t g, uint8_t b) {
  if (is_recording) {
    VerifySequenceCapacity(4);
    opcodes[opcode_count++] = COLOR;
    opcodes[opcode_count++] = r;
    opcodes[opcode_count++] = g;
    opcodes[opcode_count++] = b;
  } else {
    is_transition_in_progress = true;
    remaining_transition_duration_msec = current_transition_duration_msec;
    uint8_t cr, cg, cb;
    GetLED(0, &cr, &cg, &cb);
    current_r = cr << 8;
    current_g = cg << 8;
    current_b = cb << 8;
    end_r = r << 8; end_g = g << 8; end_b = b << 8;
    if (current_transition_duration_msec != 0) {
      delta_r = (end_r - current_r) / current_transition_duration_msec;
      delta_g = (end_g - current_g) / current_transition_duration_msec;
      delta_b = (end_b - current_b) / current_transition_duration_msec;
    } else {
      delta_r = 0;
      delta_g = 0;
      delta_b = 0;
    }
    ProcessTransition();
  }
}

void HandleTRANSITION(Transition t, uint16_t duration_msec) {
  if (is_recording) {
    VerifySequenceCapacity(4);
    opcodes[opcode_count++] = TRANSITION;
    opcodes[opcode_count++] = t;
    opcodes[opcode_count++] = duration_msec >> 8;
    opcodes[opcode_count++] = duration_msec & 0xff;
  } else {
    current_transition = t;
    current_transition_duration_msec = duration_msec;
  }
}

void HandlePAUSE(uint16_t duration_msec) {
  if (is_recording) {
    VerifySequenceCapacity(3);
    opcodes[opcode_count++] = PAUSE;
    opcodes[opcode_count++] = duration_msec >> 8;
    opcodes[opcode_count++] = duration_msec & 0xff;
  } else {
    pause_duration_msec = duration_msec;
  }
}

void HandleHALT() {
  if (is_recording) {
    VerifySequenceCapacity(1);
    opcodes[opcode_count++] = HALT;
  }
}

void Record() {
  Stop();
  opcode_count = 0;
  is_recording = true;
  SetProgramMode(SEQUENCER);
}

void Play() {
  Stop();
  is_playing = true;
  is_transition_in_progress = false;
  current_transition = NONE;
  current_transition_duration_msec = 0;
  SetProgramMode(SEQUENCER);
}

void Stop() {
  oi = 0;
  is_playing = false;
  is_recording = false;
  SetProgramMode(AD_HOC);
}

static void AdvanceSequencePointer(uint8_t opcodes) {
  oi += opcodes;
  if (oi >= opcode_count) {
      oi = 0;
  }
}

static uint8_t ProcessPause() {
  if (pause_duration_msec == 0) {
    return false;
  }
  if (pause_duration_msec > elapsed_since_last_cycle_msec) {
    pause_duration_msec -= elapsed_since_last_cycle_msec;
  } else {
    pause_duration_msec = 0;
  }
  if (pause_duration_msec == 0) {
    // Pause is done. Go ahead and do more work.
    return false;
  }
  // Pause is still happening
  return true;
}

void Run(uint16_t msec_since_last) {
  elapsed_since_last_cycle_msec = msec_since_last;
  if (is_recording) {
    return;
  }

  // This need to run even if the sequencer isn't playing, because
  // ad-hoc color changes depend on the transition engine.
  if (ProcessTransition()) {
    return;
  }

  if (!is_playing) {
    return;
  }
  if (opcode_count == 0) {
    return;
  }

  if (ProcessPause()) {
    return;
  }

  switch (opcodes[oi]) {
  case COLOR:
    HandleCOLOR(opcodes[oi + 1], opcodes[oi + 2], opcodes[oi + 3]);
    AdvanceSequencePointer(4);
    break;
  case TRANSITION:
    HandleTRANSITION(opcodes[oi + 1], (opcodes[oi + 2] << 8) | opcodes[oi + 3]);
    AdvanceSequencePointer(4);
    break;
  case PAUSE:
    HandlePAUSE((opcodes[oi + 1] << 8) | opcodes[oi + 2]);
    AdvanceSequencePointer(3);
    break;
  case HALT:
    HandleHALT();
    // Don't advance. Just spin.
    break;
  }
}

void Save() {
  WriteLightProgram(opcodes, opcode_count);
}

void Load() {
  opcode_count = ReadLightProgram(opcodes, PROGRAM_SIZE_MAX);
}
