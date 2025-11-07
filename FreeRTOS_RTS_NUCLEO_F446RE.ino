#include "rts_common.h"

/* ---------- Globals ---------- */
SemaphoreHandle_t xSharedMutex = NULL;

/* We'll hold three Job objects (used in both schedulers) */
static Job jobs[3];    // used by EDF
static Job jobsRM[3];  // used by RM

/* Mode selection */
enum Mode { MODE_EDF=1, MODE_RM=0 };
static Mode currentMode = MODE_EDF;

/* Monitor states for Serial Plotter */
volatile int plot_t1 = 0, plot_t2 = 0, plot_t3 = 0;

/* CAN queue handle */
typedef struct {
  int id;
  char data[8];
} CANMessage;
QueueHandle_t xCANQueue = NULL;

/* Forward prototypes */
void startEDF(void);
void startRM(void);
void watchdog_sim_task(void *pv);
void plotter_task(void *pv);
void simulateHang(void);
void supervisor_task(void* pv);
void senderTask(void *pv);
void receiverTask(void *pv);

/* ---------- Worker & scheduler code (EDF) ---------- */

static void edf_worker(void* pv) {
  Job* j = (Job*)pv;
  char details[48];
  for (;;) {
    vTaskSuspend(NULL); // dispatcher resumes
    /* mark running */
    j->running = 1;
    if (strcmp(j->name, "T1")==0) plot_t1 = 1;
    if (strcmp(j->name, "T2")==0) plot_t2 = 1;
    if (strcmp(j->name, "T3")==0) plot_t3 = 1;
    digitalWrite(LED_TASK1, (strcmp(j->name,"T1")==0)?HIGH:LOW);
    digitalWrite(LED_TASK2, (strcmp(j->name,"T2")==0)?HIGH:LOW);
    digitalWrite(LED_TASK3, (strcmp(j->name,"T3")==0)?HIGH:LOW);

    snprintf(details, sizeof(details), "%lu", (unsigned long)j->absDeadline);
    LOG_EVENT("START", j->name, details);

    if (xSemaphoreTake(xSharedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(j->exec_ms/2));
      xSemaphoreGive(xSharedMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(j->exec_ms/2));
    }

    vTaskDelay(pdMS_TO_TICKS(j->exec_ms/2));

    LOG_EVENT("COMPLETE", j->name, details);

    /* clear running */
    j->running = 0;
    if (strcmp(j->name, "T1")==0) plot_t1 = 0;
    if (strcmp(j->name, "T2")==0) plot_t2 = 0;
    if (strcmp(j->name, "T3")==0) plot_t3 = 0;
    digitalWrite(LED_TASK1, LOW);
    digitalWrite(LED_TASK2, LOW);
    digitalWrite(LED_TASK3, LOW);

    j->ready = false;
  }
}

static void edf_dispatcher(void* pv) {
  (void)pv;
  for (;;) {
    uint32_t now = now_ms();
    /* release jobs based on nextRelease */
    for (int i=0;i<3;i++) {
      if (!jobs[i].ready && now >= jobs[i].nextRelease) {
        jobs[i].ready = true;
        jobs[i].absDeadline = jobs[i].nextRelease + jobs[i].period_ms;
        char d[32]; snprintf(d,sizeof(d),"%lu",(unsigned long)jobs[i].absDeadline);
        LOG_EVENT("RELEASE", jobs[i].name, d);
      }
    }
    /* choose earliest deadline */
    int sel = -1; uint32_t best=0xFFFFFFFFu;
    for (int i=0;i<3;i++) {
      if (jobs[i].ready && jobs[i].absDeadline < best) {
        best = jobs[i].absDeadline; sel = i;
      }
    }
    if (sel>=0) {
      vTaskResume(jobs[sel].handle);
      /* allow it to run for its period (dispatcher polling model) */
      vTaskDelay(pdMS_TO_TICKS(1)); // give some time; worker does actual execution delays
      /* WAIT here until worker clears ready or deadline passes */
      uint32_t giveup = now_ms() + jobs[sel].period_ms;
      while (jobs[sel].ready && now_ms() < giveup) vTaskDelay(pdMS_TO_TICKS(1));
      jobs[sel].nextRelease += jobs[sel].period_ms;
      if (now_ms() > jobs[sel].absDeadline) {
        char d[32]; snprintf(d,sizeof(d),"%lu",(unsigned long)jobs[sel].absDeadline);
        LOG_EVENT("MISS", jobs[sel].name, d);
      }
    } else {
      vTaskDelay(pdMS_TO_TICKS(1));
    }
  }
}

void supervisor_task(void* pv) {
  static int jobCount = 0;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000)); // check every 1 sec
    jobCount++;
    if (jobCount >= 60) { // after 60 seconds
      LOG_EVENT("INFO","Supervisor","Stopping system");
      Serial.flush();
      taskDISABLE_INTERRUPTS();
      while(1); // stop forever
    }
  }
}

void startEDF(void) {
  xSharedMutex = xSemaphoreCreateMutex();
  uint32_t t = now_ms();
  jobs[0] = (Job){ NULL, "T1", 200, 40, t + 10, 0, false, 0 };
  jobs[1] = (Job){ NULL, "T2", 500, 80, t + 20, 0, false, 0 };
  jobs[2] = (Job){ NULL, "T3", 1000,120, t + 30, 0, false, 0 };

  for (int i=0;i<3;i++) {
    xTaskCreate(edf_worker, jobs[i].name, 256, &jobs[i], 2, &jobs[i].handle);
    vTaskSuspend(jobs[i].handle);
  }
  xTaskCreate(edf_dispatcher, "EDF_DISP", 512, NULL, 4, NULL);
}

/* ---------- RM implementation ---------- */

static void rm_worker(void* pv) {
  Job* j = (Job*)pv;
  TickType_t lastWake = xTaskGetTickCount();
  char details[48];
  for (;;) {
    snprintf(details, sizeof(details), "");
    LOG_EVENT("START", j->name, details);
    /* mark running */
    j->running = 1;
    if (strcmp(j->name, "R1")==0) plot_t1 = 1;
    if (strcmp(j->name, "R2")==0) plot_t2 = 1;
    if (strcmp(j->name, "R3")==0) plot_t3 = 1;
    if (strcmp(j->name, "R1")==0) digitalWrite(LED_TASK1, HIGH);
    if (strcmp(j->name, "R2")==0) digitalWrite(LED_TASK2, HIGH);
    if (strcmp(j->name, "R3")==0) digitalWrite(LED_TASK3, HIGH);

    if (xSemaphoreTake(xSharedMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      vTaskDelay(pdMS_TO_TICKS(j->exec_ms/2));
      xSemaphoreGive(xSharedMutex);
    } else {
      vTaskDelay(pdMS_TO_TICKS(j->exec_ms/2));
    }
    vTaskDelay(pdMS_TO_TICKS(j->exec_ms/2));

    LOG_EVENT("COMPLETE", j->name, details);

    /* clear running */
    j->running = 0;
    if (strcmp(j->name, "R1")==0) plot_t1 = 0;
    if (strcmp(j->name, "R2")==0) plot_t2 = 0;
    if (strcmp(j->name, "R3")==0) plot_t3 = 0;
    digitalWrite(LED_TASK1, LOW);
    digitalWrite(LED_TASK2, LOW);
    digitalWrite(LED_TASK3, LOW);

    vTaskDelayUntil(&lastWake, pdMS_TO_TICKS(j->period_ms));
  }
}

void startRM(void) {
  xSharedMutex = xSemaphoreCreateMutex();
  uint32_t t = now_ms();
  jobsRM[0] = (Job){ NULL, "R1", 200, 40, t, 0, false, 0 };
  jobsRM[1] = (Job){ NULL, "R2", 500, 80, t, 0, false, 0 };
  jobsRM[2] = (Job){ NULL, "R3",1000,120, t, 0, false, 0 };

  /* priorities: smaller period -> higher priority */
  xTaskCreate(rm_worker, "R1", 256, &jobsRM[0], 5, &jobsRM[0].handle);
  xTaskCreate(rm_worker, "R2", 256, &jobsRM[1], 4, &jobsRM[1].handle);
  xTaskCreate(rm_worker, "R3", 256, &jobsRM[2], 3, &jobsRM[2].handle);
}

/* ---------- Watchdog simulation and plotter ---------- */

void watchdog_sim_task(void *pv) {
  (void)pv;
  for (;;) {
    LOG_EVENT("WDT_PET","WDT","");
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

/* Plotter task: emits three numeric states for Serial Plotter */
void plotter_task(void *pv) {
  (void)pv;
  for (;;) {
    LOG_PLOT(plot_t1, plot_t2, plot_t3);
    vTaskDelay(pdMS_TO_TICKS(20)); // 50 Hz sampling for plot
  }
}

/* ---------- Utility: simulate hang to show watchdog reset ---------- */
void simulateHang() {
  LOG_EVENT("HANG_SIM","System","");
  while (1) { /* infinite loop — if real IWDG enabled, board resets; here it's just hang */ }
}

/* ------------------- CAN BUS SIMULATION ------------------- */

void senderTask(void *pv) {
  CANMessage msg;
  msg.id = 1;
  strcpy(msg.data, "HELLO");
  for (;;) {
    snprintf(msg.data, sizeof(msg.data), "T%lu", now_ms() / 1000); // update data
    if (xCANQueue) xQueueSend(xCANQueue, &msg, portMAX_DELAY);
    LOG_EVENT("CAN_TX","NodeA", msg.data);
    vTaskDelay(pdMS_TO_TICKS(1000)); // send every 1 sec
  }
}

void receiverTask(void *pv) {
  CANMessage msg;
  for (;;) {
    if (xCANQueue && xQueueReceive(xCANQueue, &msg, portMAX_DELAY) == pdTRUE) {
      // Log received CAN frame
      LOG_EVENT("CAN_RX","NodeB", msg.data);
    }
  }
}

/* ---------- Setup & main ---------- */

void setup() {
  Serial.begin(115200);
  while(!Serial) { delay(5); } // wait for serial
  /* init LEDs */
  pinMode(LED_TASK1, OUTPUT); digitalWrite(LED_TASK1, LOW);
  pinMode(LED_TASK2, OUTPUT); digitalWrite(LED_TASK2, LOW);
  pinMode(LED_TASK3, OUTPUT); digitalWrite(LED_TASK3, LOW);

  LOG_EVENT("INFO","System","Start");
  LOG_EVENT("INFO","ModePrompt","Send 'E' for EDF or 'R' for RM within 3000ms");

  /* wait 3 seconds for user selection */
  uint32_t start = now_ms();
  while (now_ms() - start < 3000) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c=='E' || c=='e') { currentMode = MODE_EDF; LOG_EVENT("INFO","Mode","EDF selected"); break; }
      if (c=='R' || c=='r') { currentMode = MODE_RM; LOG_EVENT("INFO","Mode","RM selected"); break; }
    }
    delay(10);
  }
  /* default if none chosen: keep currentMode default = EDF */
  if (currentMode==MODE_EDF) LOG_EVENT("INFO","Mode","EDF (default)"); else LOG_EVENT("INFO","Mode","RM");

  /* Create CAN queue for message passing */
  xCANQueue = xQueueCreate(10, sizeof(CANMessage));
  if (xCANQueue == NULL) {
    LOG_EVENT("ERROR","CAN","QueueCreateFailed");
    while(1);
  }

  /* Create common tasks: watchdog & plotter */
  xTaskCreate(watchdog_sim_task, "WDT", 256, NULL, 6, NULL);
  xTaskCreate(plotter_task, "PLOT", 256, NULL, 2, NULL);

  /* CAN tasks (low priority) */
  xTaskCreate(senderTask, "CAN_TX", 256, NULL, 1, NULL);
  xTaskCreate(receiverTask, "CAN_RX", 256, NULL, 1, NULL);

  /* start selected scheduler */
  if (currentMode==MODE_EDF) startEDF();
  else startRM();

  /* Supervisor stops after configured time */
  xTaskCreate(supervisor_task, "SUP", 256, NULL, 1, NULL);

  /* start the scheduler (FreeRTOS) */
  vTaskStartScheduler();
}

void loop() {
  /* Not used after scheduler starts */
}

/* ---------- End of sketch ---------- */
