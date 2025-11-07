#ifndef RTS_COMMON_H
#define RTS_COMMON_H

#include <Arduino.h>
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>

inline uint32_t now_ms() { return (uint32_t)millis(); }

/* Human-readable log macro (CSV-like) */
#define LOG_EVENT(event, task, detail) do { \
  char _buf[128]; \
  snprintf(_buf, sizeof(_buf), "%lu,%s,%s,%s", (unsigned long)now_ms(), event, task, detail); \
  Serial.println(_buf); \
} while(0)

/* Serial-Plotter friendly: prints three numeric states separated by spaces */
#define LOG_PLOT(t1,t2,t3) do { \
  char _p[64]; snprintf(_p,sizeof(_p),"%d %d %d", (int)(t1), (int)(t2), (int)(t3)); \
  Serial.println(_p); \
} while(0)

/* Job struct */
typedef struct {
  TaskHandle_t handle;
  const char* name;
  uint32_t period_ms;
  uint32_t exec_ms;
  uint32_t nextRelease;
  uint32_t absDeadline;
  volatile bool ready;
  volatile uint8_t running; /* 0 or 1 — used for plot/LED */
} Job;

/* LED pins (NUCLEO-F446RE) - use Arduino core names */
#define LED_TASK1 PB0
#define LED_TASK2 PB7
#define LED_TASK3 PB14

extern SemaphoreHandle_t xSharedMutex;

#endif
