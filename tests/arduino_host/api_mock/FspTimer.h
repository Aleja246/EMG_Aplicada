#pragma once
#include "Arduino.h"
#define GPT_TIMER (0)
#define AGT_TIMER (1)
typedef enum { TIMER_MODE_PERIODIC, TIMER_MODE_ONE_SHOT, TIMER_MODE_PWM } timer_mode_t;
typedef enum { TIMER_SOURCE_DIV_1 = 0, TIMER_SOURCE_DIV_2, TIMER_SOURCE_DIV_4 } timer_source_div_t;
typedef struct { uint32_t channel; void const *p_context; uint32_t event; } timer_callback_args_t;
typedef struct { uint8_t channel; IRQn_Type cycle_end_irq; uint8_t cycle_end_ipl; } timer_cfg_t;
using GPTimerCbk_f = void (*)(timer_callback_args_t *);
using Irq_f = void (*)(void);
class FspTimer {
 public:
  bool begin(timer_mode_t mode, uint8_t type, uint8_t channel, float freq_hz, float duty_perc, GPTimerCbk_f cbk = nullptr, void *ctx = nullptr);
  bool begin(timer_mode_t mode, uint8_t type, uint8_t channel, uint32_t period, uint32_t pulse, timer_source_div_t sd, GPTimerCbk_f cbk = nullptr, void *ctx = nullptr);
  bool setup_overflow_irq(uint8_t priority = 12, Irq_f isr_fnc = nullptr);
  bool open(); bool start(); bool stop();
  timer_cfg_t *get_cfg();
  static int8_t get_available_timer(uint8_t &type, bool force = false);
};
