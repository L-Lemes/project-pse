/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2024 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  * notas: J Ranhel - rev 2026.07
  * PA5 =1 na entrada do callBack do RxCpltCallback(), =0 na saída
  * PB7 =1 na entrada do PeriodElapsedCallback(), =0 ma saída
  *
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define PROTOCOL_FRAME_SIZE  (sizeof(PNGPRG) - 1U)
#define DISPLAY_DIGIT_COUNT  NDIGSDISP

typedef enum {
  APP_PHASE_BOOT_TEST = 0,
  APP_PHASE_WAIT_LINK,
  APP_PHASE_RUNNING,
  APP_PHASE_CONNECTION_ERROR
} AppPhase;

typedef enum {
  BUTTON_A1 = 0,
  BUTTON_A2,
  BUTTON_A3,
  BUTTON_COUNT
} ButtonId;

typedef enum {
  PROTOCOL_INVALID = 0,
  PROTOCOL_PING,
  PROTOCOL_PONG,
  PROTOCOL_REQUEST_SERVICE,
  PROTOCOL_STOP_REQUEST,
  PROTOCOL_NO_SERVICE,
  PROTOCOL_REQUEST_CHRONO,
  PROTOCOL_REQUEST_ADC,
  PROTOCOL_CHRONO_VALUE,
  PROTOCOL_ADC_VALUE
} ProtocolType;

typedef struct {
  ProtocolType type;
  uint16_t value;
} ProtocolMessage;

typedef enum {
  APP_EVENT_CHRONO_TICK = 0,
  APP_EVENT_ADC_READY,
  APP_EVENT_BUTTON_EDGE,
  APP_EVENT_PROTOCOL
} AppEventType;

typedef struct {
  AppEventType type;
  union {
    uint16_t value;
    ButtonId button;
    ProtocolMessage protocol;
  } data;
} AppEvent;

typedef enum {
  COMM_EVENT_SEND = 0,
  COMM_EVENT_RX_FRAME,
  COMM_EVENT_TX_COMPLETE,
  COMM_EVENT_UART_ERROR
} CommEventType;

typedef struct {
  CommEventType type;
  union {
    ProtocolMessage message;
    uint8_t frame[PROTOCOL_FRAME_SIZE];
  } data;
} CommEvent;

typedef enum {
  DISPLAY_LED_NONE = 0,
  DISPLAY_LED_D1,
  DISPLAY_LED_D2,
  DISPLAY_LED_D3,
  DISPLAY_LED_D4
} DisplayLed;

typedef enum {
  STARTUP_ANIMATION_ROUTE = 0,
  STARTUP_ANIMATION_DRAW_EIGHTS,
  STARTUP_ANIMATION_HOLD_EIGHTS
} StartupAnimationPhase;

typedef struct {
  uint8_t digit;
  uint8_t segment_mask;
} StartupPathFrame;

typedef struct {
  int8_t digits[DISPLAY_DIGIT_COUNT];
  uint8_t decimal_mask;
  DisplayLed blinking_led;
  bool animate_all_eights;
  bool pulse_buzzer;
  uint32_t buzzer_duration_ms;
} DisplayCommand;

typedef struct {
  DisplayCommand command;
  StartupAnimationPhase animation_phase;
  uint8_t animation_frame;
  bool led_on;
  bool buzzer_active;
  bool buzzer_on;
  bool buzzer_latched;
  uint32_t next_led_toggle;
  uint32_t next_animation_frame;
  uint32_t next_animation_phase;
  uint32_t next_buzzer_toggle;
  uint32_t buzzer_deadline;
} DisplayRuntime;

typedef struct {
  AppPhase phase;
  bool ready_beep_pending;
  bool local_display_enabled;
  bool requesting_peer_service;
  bool serving_peer;
  bool no_service_alert;
  bool remote_chrono_valid;
  bool remote_adc_valid;
  bool ping_awaiting_response;
  bool request_adc_next;
  uint8_t missed_pongs;
  uint8_t display_page;
  uint16_t chrono_deciseconds;
  uint16_t adc_millivolts;
  uint16_t remote_chrono_deciseconds;
  uint16_t remote_adc_millivolts;
  uint32_t ping_deadline;
  uint32_t display_deadline;
  uint32_t remote_request_deadline;
  uint32_t alert_deadline;
} AppState;

typedef struct {
  ButtonId id;
  uint16_t pin;
  GPIO_PinState stable_state;
  GPIO_PinState candidate_state;
  uint32_t candidate_since_ms;
} ButtonInput;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ADC_REFERENCE_MV              VDD_VALUE
#define ADC_FULL_SCALE                4095U
#define CHRONO_WRAP_DECISECONDS       6000U

#define BOOT_TEST_MS                  3000U
#define PING_PERIOD_MS                 250U
#define PING_MISSED_LIMIT                3U
#define CHRONO_PERIOD_MS        (DT_CRONO + 1U)
#define ADC_PERIOD_MS              (DT_ADC + 1U)
#define REMOTE_REQUEST_PERIOD_MS (DT_NEWREQ + 1U)
#define LOCAL_PAGE_PERIOD_MS (DT_DISPLAY_MD1 + 1U)
#define SERVICE_PAGE_PERIOD_MS (DT_DISPLAY_MD2 + 1U)
#define DISPLAY_SCAN_PERIOD_MS        DT_MUX_DISP
#define OUTPUT_BLINK_PERIOD_MS      (DT_LEDS + 1U)
#define STARTUP_CHASE_PERIOD_MS (DT_EFEITO_INI + 1U)
#define STARTUP_CHASE_TAIL_LENGTH         3U
#define STARTUP_ALL_SEGMENTS_MS          600U
#define READY_BEEP_MS                    200U
#define NO_SERVICE_ALERT_MS            5000U
#define BUTTON_DEBOUNCE_MS               50U
#define CONTROLLER_POLL_MS    (DT_CKECKEYS + 1U)
#define IO_POLL_MS            (DT_CKECKEYS + 1U)

#define APP_EVENT_QUEUE_LENGTH           16U
#define COMM_EVENT_QUEUE_LENGTH          16U
#define DISPLAY_QUEUE_LENGTH              6U
#define COMM_PENDING_FRAME_COUNT           8U

#define BUTTON_A1_PIN              GPIO_PIN_1
#define BUTTON_A2_PIN              GPIO_PIN_2
#define BUTTON_A3_PIN              GPIO_PIN_3

#define LED_D1_PIN                GPIO_PIN_15
#define LED_D2_PIN                GPIO_PIN_14
#define LED_D3_PIN                GPIO_PIN_13
#define LED_D4_PIN                GPIO_PIN_12
#define LED_ALL_PINS   (LED_D1_PIN | LED_D2_PIN | LED_D3_PIN | LED_D4_PIN)
#define BUZZER_PIN                GPIO_PIN_5
#define BUZZER_ACTIVE_LEVEL        GPIO_PIN_RESET
#define BUZZER_INACTIVE_LEVEL      GPIO_PIN_SET

#define GLYPH_BLANK                  ((int8_t)DIG_APAGADO)
#define GLYPH_LOWER_N                ((int8_t)0x11)
#define GLYPH_LOWER_O                ((int8_t)0x12)
#define GLYPH_LOWER_R                ((int8_t)0x13)
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

UART_HandleTypeDef huart1;
DMA_HandleTypeDef hdma_usart1_rx;
DMA_HandleTypeDef hdma_usart1_tx;

/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 192 * 4,
  .priority = (osPriority_t) osPriorityNormal
};
/* Definitions for display */
osThreadId_t displayHandle;
const osThreadAttr_t display_attributes = {
  .name = "display",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal
};
/* Definitions for uartTx */
osThreadId_t uartTxHandle;
const osThreadAttr_t uartTx_attributes = {
  .name = "uartTx",
  .stack_size = 160 * 4,
  .priority = (osPriority_t) osPriorityAboveNormal
};
/* Definitions for keyboard */
osThreadId_t keyboardHandle;
const osThreadAttr_t keyboard_attributes = {
  .name = "keyboard",
  .stack_size = 96 * 4,
  .priority = (osPriority_t) osPriorityNormal
};
/* Definitions for DMA */
osMessageQueueId_t DMAHandle;
const osMessageQueueAttr_t DMA_attributes = {
  .name = "DMA"
};
/* Definitions for RxEvent */
osMessageQueueId_t RxEventHandle;
const osMessageQueueAttr_t RxEvent_attributes = {
  .name = "RxEvent"
};
/* Definitions for displayCommands */
osMessageQueueId_t displayCommandsHandle;
const osMessageQueueAttr_t displayCommands_attributes = {
  .name = "displayCommands"
};
/* USER CODE BEGIN PV */
uint8_t BufOUT[PROTOCOL_FRAME_SIZE] = {'0','0','0','0','0'};
uint8_t BufIN[PROTOCOL_FRAME_SIZE]  = {'0','0','0','0','0'};
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_USART1_UART_Init(void);
void StartDefaultTask(void *argument);
void fn_display(void *argument);
void fn_uart_tx(void *argument);
void fn_keyboard(void *argument);

static void MX_NVIC_Init(void);
/* USER CODE BEGIN PFP */
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static uint32_t ms_to_ticks(uint32_t milliseconds) {
  const uint32_t frequency = osKernelGetTickFreq();
  return (uint32_t)(((uint64_t)milliseconds * frequency + 999U) / 1000U);
}

static bool time_reached(uint32_t now, uint32_t deadline) {
  return (int32_t)(now - deadline) >= 0;
}

enum {
  SEGMENT_A = (1U << 0),
  SEGMENT_B = (1U << 1),
  SEGMENT_C = (1U << 2),
  SEGMENT_D = (1U << 3),
  SEGMENT_E = (1U << 4),
  SEGMENT_F = (1U << 5),
  SEGMENT_G = (1U << 6)
};

/* The first digit grows from top to bottom. The next one uses this array in
   reverse, so it starts exactly where the preceding digit ended. */
static const uint8_t startup_eight_segment_order[] = {
  SEGMENT_A,
  SEGMENT_F,
  SEGMENT_B,
  SEGMENT_G,
  SEGMENT_E,
  SEGMENT_C,
  SEGMENT_D
};

/* Serpentine route from the upper-left corner to the lower-right corner.
   Only the moving head and its short tail are rendered. */
static const StartupPathFrame startup_route_path[] = {
  {.digit = 3U, .segment_mask = SEGMENT_A},
  {.digit = 2U, .segment_mask = SEGMENT_A},
  {.digit = 1U, .segment_mask = SEGMENT_A},
  {.digit = 0U, .segment_mask = SEGMENT_A},
  {.digit = 0U, .segment_mask = SEGMENT_B},
  {.digit = 0U, .segment_mask = SEGMENT_G},
  {.digit = 1U, .segment_mask = SEGMENT_G},
  {.digit = 2U, .segment_mask = SEGMENT_G},
  {.digit = 3U, .segment_mask = SEGMENT_G},
  {.digit = 3U, .segment_mask = SEGMENT_E},
  {.digit = 3U, .segment_mask = SEGMENT_D},
  {.digit = 2U, .segment_mask = SEGMENT_D},
  {.digit = 1U, .segment_mask = SEGMENT_D},
  {.digit = 0U, .segment_mask = SEGMENT_D}
};

typedef struct {
  ProtocolType type;
  const char *token;
} ProtocolToken;

static const ProtocolToken protocol_tokens[] = {
  {PROTOCOL_PING, PNGPRG},
  {PROTOCOL_PONG, PNGRSP},
  {PROTOCOL_REQUEST_SERVICE, REQSRV},
  {PROTOCOL_STOP_REQUEST, REQOFF},
  {PROTOCOL_NO_SERVICE, MSGNSV},
  {PROTOCOL_REQUEST_CHRONO, REQCRN},
  {PROTOCOL_REQUEST_ADC, REQADC}
};

static void set_buzzer(bool on) {
  HAL_GPIO_WritePin(GPIOB, BUZZER_PIN,
                    on ? BUZZER_ACTIVE_LEVEL : BUZZER_INACTIVE_LEVEL);
}

static bool post_app_event(const AppEvent *event) {
  return (RxEventHandle != NULL)
      && (osMessageQueuePut(RxEventHandle, event, 0U, 0U) == osOK);
}

static bool post_comm_event(const CommEvent *event) {
  return (DMAHandle != NULL)
      && (osMessageQueuePut(DMAHandle, event, 0U, 0U) == osOK);
}

static bool request_transmission(ProtocolType type, uint16_t value) {
  const CommEvent event = {
    .type = COMM_EVENT_SEND,
    .data.message = {.type = type, .value = value}
  };
  return post_comm_event(&event);
}

static bool frame_has_four_digits(const uint8_t frame[PROTOCOL_FRAME_SIZE]) {
  for (uint32_t index = 1U; index < PROTOCOL_FRAME_SIZE; ++index) {
    if (conv_ASC_num(frame[index]) == 0x45) {
      return false;
    }
  }
  return true;
}

static uint16_t decode_four_digits(const uint8_t frame[PROTOCOL_FRAME_SIZE]) {
  uint16_t value = 0U;
  for (uint32_t index = 1U; index < PROTOCOL_FRAME_SIZE; ++index) {
    value = (uint16_t)(value * 10U + (uint16_t)conv_ASC_num(frame[index]));
  }
  return value;
}

static void encode_four_digits(uint16_t value, uint8_t frame[PROTOCOL_FRAME_SIZE]) {
  for (int32_t index = (int32_t)PROTOCOL_FRAME_SIZE - 1; index >= 1; --index) {
    frame[index] = conv_num_ASC((int8_t)(value % 10U));
    value /= 10U;
  }
}

static bool protocol_encode(const ProtocolMessage *message,
                            uint8_t frame[PROTOCOL_FRAME_SIZE]) {
  if ((message == NULL) || (frame == NULL)) {
    return false;
  }

  for (uint32_t index = 0U;
       index < sizeof(protocol_tokens) / sizeof(protocol_tokens[0]); ++index) {
    if (message->type == protocol_tokens[index].type) {
      memcpy(frame, protocol_tokens[index].token, PROTOCOL_FRAME_SIZE);
      return true;
    }
  }

  if (message->type == PROTOCOL_CHRONO_VALUE) {
    const uint16_t value = (uint16_t)(message->value % CHRONO_WRAP_DECISECONDS);
    const uint16_t seconds = (value / 10U) % 60U;
    frame[0] = (uint8_t)'c';
    frame[1] = conv_num_ASC((int8_t)(value / 600U));
    frame[2] = conv_num_ASC((int8_t)(seconds / 10U));
    frame[3] = conv_num_ASC((int8_t)(seconds % 10U));
    frame[4] = conv_num_ASC((int8_t)(value % 10U));
    return true;
  }
  if (message->type == PROTOCOL_ADC_VALUE) {
    frame[0] = (uint8_t)'a';
    encode_four_digits((message->value > ADC_REFERENCE_MV)
                       ? ADC_REFERENCE_MV : message->value, frame);
    return true;
  }
  return false;
}

static bool protocol_decode(const uint8_t frame[PROTOCOL_FRAME_SIZE],
                            ProtocolMessage *message) {
  if ((frame == NULL) || (message == NULL)) {
    return false;
  }

  message->type = PROTOCOL_INVALID;
  message->value = 0U;

  for (uint32_t index = 0U;
       index < sizeof(protocol_tokens) / sizeof(protocol_tokens[0]); ++index) {
    if (memcmp(frame, protocol_tokens[index].token, PROTOCOL_FRAME_SIZE) == 0) {
      message->type = protocol_tokens[index].type;
      return true;
    }
  }

  if ((frame[0] == (uint8_t)'a') && frame_has_four_digits(frame)) {
    const uint16_t millivolts = decode_four_digits(frame);
    if (millivolts <= ADC_REFERENCE_MV) {
      message->type = PROTOCOL_ADC_VALUE;
      message->value = millivolts;
    }
  }
  else if ((frame[0] == (uint8_t)'c') && frame_has_four_digits(frame)
           && (frame[2] <= (uint8_t)'5')) {
    const uint16_t minutes = (uint16_t)conv_ASC_num(frame[1]);
    const uint16_t second_tens = (uint16_t)conv_ASC_num(frame[2]);
    const uint16_t second_units = (uint16_t)conv_ASC_num(frame[3]);
    const uint16_t tenths = (uint16_t)conv_ASC_num(frame[4]);
    message->type = PROTOCOL_CHRONO_VALUE;
    message->value = (uint16_t)(minutes * 600U + second_tens * 100U
                                + second_units * 10U + tenths);
  }

  return message->type != PROTOCOL_INVALID;
}

static uint16_t adc_raw_to_millivolts(uint16_t raw) {
  return (uint16_t)(((uint32_t)raw * ADC_REFERENCE_MV
                     + ADC_FULL_SCALE / 2U) / ADC_FULL_SCALE);
}

static void format_chrono(uint16_t deciseconds,
                          int8_t digits[DISPLAY_DIGIT_COUNT]) {
  const uint16_t value = (uint16_t)(deciseconds % CHRONO_WRAP_DECISECONDS);
  const uint16_t seconds = (value / 10U) % 60U;
  digits[3] = (int8_t)(value / 600U);
  digits[2] = (int8_t)(seconds / 10U);
  digits[1] = (int8_t)(seconds % 10U);
  digits[0] = (int8_t)(value % 10U);
}

static void format_adc(uint16_t millivolts,
                       int8_t digits[DISPLAY_DIGIT_COUNT]) {
  const uint16_t value = (millivolts > ADC_REFERENCE_MV)
      ? ADC_REFERENCE_MV : millivolts;
  digits[3] = (int8_t)(value / 1000U);
  digits[2] = (int8_t)((value / 100U) % 10U);
  digits[1] = (int8_t)((value / 10U) % 10U);
  digits[0] = (int8_t)(value % 10U);
}

static void fill_all_segments(int8_t digits[DISPLAY_DIGIT_COUNT]) {
  memset(digits, 8, DISPLAY_DIGIT_COUNT);
}

static bool display_command_shows_all_eights(const DisplayCommand *command) {
  return command->digits[0] == 8 && command->digits[1] == 8
      && command->digits[2] == 8 && command->digits[3] == 8;
}

static void display_all_eights(DisplayCommand *command) {
  fill_all_segments(command->digits);
  command->decimal_mask = 0x0FU;
}

static void display_measurement(DisplayCommand *command, bool valid,
                                bool is_adc, uint16_t value) {
  if (!valid) {
    display_all_eights(command);
    return;
  }
  if (is_adc) {
    format_adc(value, command->digits);
    command->decimal_mask = 0x08U;
  }
  else {
    format_chrono(value, command->digits);
    command->decimal_mask = 0x0AU;
  }
}

static void publish_display_command(const DisplayCommand *command) {
  if (osMessageQueuePut(displayCommandsHandle, command, 0U, 0U) != osOK) {
    DisplayCommand discarded;
    (void)osMessageQueueGet(displayCommandsHandle, &discarded, NULL, 0U);
    (void)osMessageQueuePut(displayCommandsHandle, command, 0U, 0U);
  }
}

static void display_service_page(DisplayCommand *command,
                                 const AppState *state) {
  const bool remote = state->display_page >= 2U;
  const bool is_adc = (state->display_page & 1U) != 0U;
  const bool valid = remote
      ? (is_adc ? state->remote_adc_valid : state->remote_chrono_valid)
      : state->local_display_enabled;
  const uint16_t value = remote
      ? (is_adc ? state->remote_adc_millivolts
                : state->remote_chrono_deciseconds)
      : (is_adc ? state->adc_millivolts : state->chrono_deciseconds);
  command->blinking_led = (DisplayLed)(DISPLAY_LED_D1 + state->display_page);
  display_measurement(command, valid, is_adc, value);
}

static void app_publish_display(const AppState *state) {
  DisplayCommand command = {
    .digits = {GLYPH_BLANK, GLYPH_BLANK, GLYPH_BLANK, GLYPH_BLANK}
  };

  if ((state->phase == APP_PHASE_BOOT_TEST)
      || (state->phase == APP_PHASE_WAIT_LINK)) {
    display_all_eights(&command);
  }
  else if (state->phase == APP_PHASE_CONNECTION_ERROR) {
    const int8_t text[] = {GLYPH_LOWER_N, GLYPH_LOWER_O, 0x0C,
                           GLYPH_LOWER_N};
    memcpy(command.digits, text, sizeof(text));
  }
  else if (state->no_service_alert) {
    const int8_t text[] = {GLYPH_LOWER_R, 0x0E, 5, GLYPH_LOWER_N};
    memcpy(command.digits, text, sizeof(text));
    command.pulse_buzzer = true;
    command.buzzer_duration_ms = NO_SERVICE_ALERT_MS;
  }
  else if (state->serving_peer) {
    display_service_page(&command, state);
  }
  else if (state->local_display_enabled) {
    const bool is_adc = state->display_page != 0U;
    display_measurement(&command, true, is_adc,
                        is_adc ? state->adc_millivolts
                               : state->chrono_deciseconds);
    command.blinking_led = is_adc ? DISPLAY_LED_D2 : DISPLAY_LED_D1;
  }
  else {
    display_all_eights(&command);
  }

  /* Requirement a.3/a.4: immediately after reset, keep every segment and
     decimal point continuously on for the complete 3 s boot-test interval.
     In later states, the same 8.8.8.8 placeholder uses the snake animation. */
  command.animate_all_eights =
      (state->phase != APP_PHASE_BOOT_TEST)
      && display_command_shows_all_eights(&command);
  if (state->ready_beep_pending) {
    command.pulse_buzzer = true;
    command.buzzer_duration_ms = READY_BEEP_MS;
  }
  publish_display_command(&command);
}

static void app_reset_display_cycle(AppState *state, uint32_t now) {
  state->display_page = 0U;
  if (state->serving_peer) {
    state->display_deadline = now + ms_to_ticks(SERVICE_PAGE_PERIOD_MS);
  }
  else if (state->local_display_enabled) {
    state->display_deadline = now + ms_to_ticks(LOCAL_PAGE_PERIOD_MS);
  }
  else {
    state->display_deadline = 0U;
  }
}

static void app_initialize(AppState *state) {
  memset(state, 0, sizeof(*state));
  state->phase = APP_PHASE_BOOT_TEST;
  state->ping_deadline = osKernelGetTickCount() + ms_to_ticks(BOOT_TEST_MS);
  app_publish_display(state);
}

static void app_handle_button(AppState *state, ButtonId button) {
  if ((button >= BUTTON_COUNT) || (state->phase != APP_PHASE_RUNNING)) {
    return;
  }

  switch (button) {
    case BUTTON_A1:
      state->local_display_enabled = true;
      if (state->serving_peer) {
        state->serving_peer = false;
        (void)request_transmission(PROTOCOL_NO_SERVICE, 0U);
      }
      app_reset_display_cycle(state, osKernelGetTickCount());
      break;
    case BUTTON_A2:
      state->requesting_peer_service = true;
      state->no_service_alert = false;
      (void)request_transmission(PROTOCOL_REQUEST_SERVICE, 0U);
      break;
    case BUTTON_A3:
      state->requesting_peer_service = false;
      (void)request_transmission(PROTOCOL_STOP_REQUEST, 0U);
      break;
    default:
      break;
  }

  app_publish_display(state);
}

static void app_handle_protocol(AppState *state, const ProtocolMessage *message) {
  const uint32_t now = osKernelGetTickCount();

  if (state->phase == APP_PHASE_CONNECTION_ERROR) {
    return;
  }

  if (message->type == PROTOCOL_PING) {
    (void)request_transmission(PROTOCOL_PONG, 0U);
    return;
  }

  if (message->type == PROTOCOL_PONG) {
    if ((state->phase == APP_PHASE_WAIT_LINK)
        || (state->phase == APP_PHASE_RUNNING)) {
      state->ping_awaiting_response = false;
      state->missed_pongs = 0U;
      if (state->phase == APP_PHASE_WAIT_LINK) {
        state->phase = APP_PHASE_RUNNING;
        app_reset_display_cycle(state, now);
        state->ready_beep_pending = true;
        app_publish_display(state);
        state->ready_beep_pending = false;
      }
    }
    return;
  }

  if (state->phase != APP_PHASE_RUNNING) {
    return;
  }

  switch (message->type) {
    case PROTOCOL_REQUEST_SERVICE:
      state->serving_peer = true;
      state->remote_chrono_valid = false;
      state->remote_adc_valid = false;
      state->request_adc_next = false;
      state->remote_request_deadline = now;
      app_reset_display_cycle(state, now);
      break;
    case PROTOCOL_STOP_REQUEST:
      state->serving_peer = false;
      app_reset_display_cycle(state, now);
      break;
    case PROTOCOL_NO_SERVICE:
      if (state->requesting_peer_service) {
        state->requesting_peer_service = false;
        state->no_service_alert = true;
        state->alert_deadline = now + ms_to_ticks(NO_SERVICE_ALERT_MS);
      }
      break;
    case PROTOCOL_REQUEST_CHRONO:
      (void)request_transmission(PROTOCOL_CHRONO_VALUE,
                                 state->chrono_deciseconds);
      break;
    case PROTOCOL_REQUEST_ADC:
      (void)request_transmission(PROTOCOL_ADC_VALUE,
                                 state->adc_millivolts);
      break;
    case PROTOCOL_CHRONO_VALUE:
      state->remote_chrono_deciseconds = message->value;
      state->remote_chrono_valid = true;
      break;
    case PROTOCOL_ADC_VALUE:
      state->remote_adc_millivolts = message->value;
      state->remote_adc_valid = true;
      break;
    default:
      break;
  }

  app_publish_display(state);
}

static void app_handle_event(AppState *state, const AppEvent *event) {
  switch (event->type) {
    case APP_EVENT_CHRONO_TICK:
      state->chrono_deciseconds = (uint16_t)(
          (state->chrono_deciseconds + 1U) % CHRONO_WRAP_DECISECONDS);
      app_publish_display(state);
      break;
    case APP_EVENT_ADC_READY:
      state->adc_millivolts = adc_raw_to_millivolts(event->data.value);
      app_publish_display(state);
      break;
    case APP_EVENT_BUTTON_EDGE:
      app_handle_button(state, event->data.button);
      break;
    case APP_EVENT_PROTOCOL:
      app_handle_protocol(state, &event->data.protocol);
      break;
    default:
      break;
  }
}

static bool app_process_ping(AppState *state, uint32_t now) {
  if (((state->phase != APP_PHASE_WAIT_LINK)
       && (state->phase != APP_PHASE_RUNNING))
      || !time_reached(now, state->ping_deadline)) {
    return true;
  }
  if (state->ping_awaiting_response
      && (++state->missed_pongs >= PING_MISSED_LIMIT)) {
    state->phase = APP_PHASE_CONNECTION_ERROR;
    state->serving_peer = false;
    state->requesting_peer_service = false;
    state->no_service_alert = false;
    app_publish_display(state);
    return false;
  }
  (void)request_transmission(PROTOCOL_PING, 0U);
  state->ping_awaiting_response = true;
  state->ping_deadline = now + ms_to_ticks(PING_PERIOD_MS);
  return true;
}

static void app_process_deadlines(AppState *state) {
  const uint32_t now = osKernelGetTickCount();

  if (state->phase == APP_PHASE_BOOT_TEST) {
    if (time_reached(now, state->ping_deadline)) {
      state->phase = APP_PHASE_WAIT_LINK;
      state->ping_deadline = now;
      app_publish_display(state);
    }
    return;
  }

  if (!app_process_ping(state, now)) {
    return;
  }

  if ((state->phase != APP_PHASE_RUNNING) || state->no_service_alert) {
    if (state->no_service_alert && time_reached(now, state->alert_deadline)) {
      state->no_service_alert = false;
      app_reset_display_cycle(state, now);
      app_publish_display(state);
    }
    return;
  }

  if (state->serving_peer
      && time_reached(now, state->remote_request_deadline)) {
    (void)request_transmission(state->request_adc_next
                               ? PROTOCOL_REQUEST_ADC
                               : PROTOCOL_REQUEST_CHRONO, 0U);
    state->request_adc_next = !state->request_adc_next;
    state->remote_request_deadline =
        now + ms_to_ticks(REMOTE_REQUEST_PERIOD_MS);
  }

  if ((state->serving_peer || state->local_display_enabled)
      && time_reached(now, state->display_deadline)) {
    const uint8_t page_count = state->serving_peer ? DISPLAY_DIGIT_COUNT : 2U;
    const uint32_t page_ms = state->serving_peer
        ? SERVICE_PAGE_PERIOD_MS : LOCAL_PAGE_PERIOD_MS;
    state->display_page = (uint8_t)((state->display_page + 1U) % page_count);
    state->display_deadline = now + ms_to_ticks(page_ms);
    app_publish_display(state);
  }
}

static uint16_t glyph_to_segments(int8_t glyph) {
  switch (glyph) {
    case GLYPH_LOWER_N:
      return 0xAB00U;  /* c, e, g */
    case GLYPH_LOWER_O:
      return 0xA300U;  /* c, d, e, g */
    case GLYPH_LOWER_R:
      return 0xAF00U;  /* e, g */
    default:
      return conv_7_seg(glyph);
  }
}

static void scan_display(const int8_t digits[DISPLAY_DIGIT_COUNT],
                         uint8_t decimal_mask) {
  static uint8_t digit = 0U;
  static const uint16_t digit_select[DISPLAY_DIGIT_COUNT] = {
    0x0008U, 0x0004U, 0x0002U, 0x0001U
  };
  uint16_t serialized = glyph_to_segments(digits[digit]);

  if ((decimal_mask & (uint8_t)(1U << digit)) != 0U) {
    serialized &= 0x7FFFU;
  }
  serialized |= digit_select[digit];
  serializar(serialized);
  digit = (uint8_t)((digit + 1U) % DISPLAY_DIGIT_COUNT);
}

static void scan_startup_segments(
    const uint8_t segments[DISPLAY_DIGIT_COUNT]) {
  static uint8_t scanned_digit = 0U;
  static const uint16_t digit_select[DISPLAY_DIGIT_COUNT] = {
    0x0008U, 0x0004U, 0x0002U, 0x0001U
  };

  serializar((uint16_t)(0xFF00U
      & (uint16_t)~((uint16_t)segments[scanned_digit] << 8))
      | digit_select[scanned_digit]);
  scanned_digit = (uint8_t)((scanned_digit + 1U) % DISPLAY_DIGIT_COUNT);
}

static void scan_startup_route(uint8_t frame_index) {
  uint8_t segments[DISPLAY_DIGIT_COUNT] = {0U, 0U, 0U, 0U};
  const uint8_t route_frame_count = (uint8_t)(sizeof(startup_route_path)
      / sizeof(startup_route_path[0]));

  for (uint8_t trail = 0U; trail < STARTUP_CHASE_TAIL_LENGTH; ++trail) {
    if (frame_index < trail) {
      break;
    }
    const uint8_t path_index = (uint8_t)(frame_index - trail);
    if (path_index >= route_frame_count) {
      continue;
    }
    const StartupPathFrame *frame = &startup_route_path[path_index];
    segments[frame->digit] |= frame->segment_mask;
  }
  scan_startup_segments(segments);
}

static void scan_startup_eights(uint8_t frame_index) {
  const uint8_t segment_count = (uint8_t)(
      sizeof(startup_eight_segment_order)
      / sizeof(startup_eight_segment_order[0]));
  uint8_t segments[DISPLAY_DIGIT_COUNT] = {0U, 0U, 0U, 0U};

  for (uint8_t path_index = 0U; path_index <= frame_index; ++path_index) {
    const uint8_t digit_order = (uint8_t)(path_index / segment_count);
    const uint8_t digit = digit_order;
    const uint8_t position = (uint8_t)(path_index % segment_count);
    const uint8_t segment_index = ((digit_order & 1U) == 0U)
        ? (uint8_t)(segment_count - 1U - position) : position;
    segments[digit] |= startup_eight_segment_order[segment_index];
  }
  scan_startup_segments(segments);
}

static uint8_t startup_eight_frame_count(void) {
  return (uint8_t)(DISPLAY_DIGIT_COUNT
      * (sizeof(startup_eight_segment_order)
         / sizeof(startup_eight_segment_order[0])));
}

static void set_blinking_led(DisplayLed led, bool on) {
  uint16_t selected_pin = 0U;
  HAL_GPIO_WritePin(GPIOB, LED_ALL_PINS, GPIO_PIN_SET);

  switch (led) {
    case DISPLAY_LED_D1: selected_pin = LED_D1_PIN; break;
    case DISPLAY_LED_D2: selected_pin = LED_D2_PIN; break;
    case DISPLAY_LED_D3: selected_pin = LED_D3_PIN; break;
    case DISPLAY_LED_D4: selected_pin = LED_D4_PIN; break;
    default: break;
  }

  if (on && (selected_pin != 0U)) {
    HAL_GPIO_WritePin(GPIOB, selected_pin, GPIO_PIN_RESET);
  }
}

static uint8_t animation_frame_count(StartupAnimationPhase phase) {
  if (phase == STARTUP_ANIMATION_ROUTE) {
    return (uint8_t)(sizeof(startup_route_path) / sizeof(startup_route_path[0]));
  }
  return phase == STARTUP_ANIMATION_DRAW_EIGHTS
      ? startup_eight_frame_count() : 0U;
}

static void display_animation_reset(DisplayRuntime *runtime, uint32_t now) {
  runtime->animation_phase = STARTUP_ANIMATION_ROUTE;
  runtime->animation_frame = 0U;
  runtime->next_animation_frame = now + ms_to_ticks(STARTUP_CHASE_PERIOD_MS);
  runtime->next_animation_phase = now + ms_to_ticks(
      STARTUP_CHASE_PERIOD_MS * animation_frame_count(runtime->animation_phase));
}

static void display_accept_command(DisplayRuntime *runtime,
                                   const DisplayCommand *incoming, uint32_t now) {
  if (incoming->blinking_led != runtime->command.blinking_led) {
    runtime->led_on = true;
    runtime->next_led_toggle = now + ms_to_ticks(OUTPUT_BLINK_PERIOD_MS);
  }
  if (incoming->animate_all_eights && !runtime->command.animate_all_eights) {
    display_animation_reset(runtime, now);
  }
  if (incoming->pulse_buzzer && !runtime->buzzer_latched) {
    runtime->buzzer_latched = true;
    runtime->buzzer_active = true;
    runtime->buzzer_on = true;
    runtime->next_buzzer_toggle = now + ms_to_ticks(OUTPUT_BLINK_PERIOD_MS);
    runtime->buzzer_deadline = now + ms_to_ticks(incoming->buzzer_duration_ms);
  }
  else if (!incoming->pulse_buzzer) {
    runtime->buzzer_latched = false;
    if (!runtime->buzzer_active
        || runtime->command.buzzer_duration_ms == NO_SERVICE_ALERT_MS) {
      runtime->buzzer_active = false;
      runtime->buzzer_on = false;
    }
  }
  runtime->command = *incoming;
}

static void display_update_runtime(DisplayRuntime *runtime, uint32_t now) {
  if (time_reached(now, runtime->next_led_toggle)) {
    runtime->led_on = !runtime->led_on;
    runtime->next_led_toggle = now + ms_to_ticks(OUTPUT_BLINK_PERIOD_MS);
  }
  if (runtime->command.animate_all_eights
      && time_reached(now, runtime->next_animation_phase)) {
    runtime->animation_phase = (StartupAnimationPhase)(
        (runtime->animation_phase + 1U) % 3U);
    runtime->animation_frame = 0U;
    runtime->next_animation_frame = now + ms_to_ticks(STARTUP_CHASE_PERIOD_MS);
    runtime->next_animation_phase = now + ms_to_ticks(
        runtime->animation_phase == STARTUP_ANIMATION_HOLD_EIGHTS
        ? STARTUP_ALL_SEGMENTS_MS
        : STARTUP_CHASE_PERIOD_MS
          * animation_frame_count(runtime->animation_phase));
  }
  if (runtime->command.animate_all_eights
      && runtime->animation_phase != STARTUP_ANIMATION_HOLD_EIGHTS
      && time_reached(now, runtime->next_animation_frame)) {
    runtime->animation_frame = (uint8_t)((runtime->animation_frame + 1U)
        % animation_frame_count(runtime->animation_phase));
    runtime->next_animation_frame = now + ms_to_ticks(STARTUP_CHASE_PERIOD_MS);
  }
  if (runtime->buzzer_active && time_reached(now, runtime->buzzer_deadline)) {
    runtime->buzzer_active = false;
    runtime->buzzer_on = false;
  }
  else if (runtime->buzzer_active
           && time_reached(now, runtime->next_buzzer_toggle)) {
    runtime->buzzer_on = !runtime->buzzer_on;
    runtime->next_buzzer_toggle = now + ms_to_ticks(OUTPUT_BLINK_PERIOD_MS);
  }
}

static void display_render(const DisplayRuntime *runtime) {
  const DisplayCommand *command = &runtime->command;
  if (command->animate_all_eights
      && command->blinking_led == DISPLAY_LED_NONE) {
    if (runtime->animation_phase == STARTUP_ANIMATION_HOLD_EIGHTS) {
      HAL_GPIO_WritePin(GPIOB, LED_ALL_PINS, GPIO_PIN_RESET);
    }
    else {
      const uint8_t segments_per_digit = (uint8_t)(
          sizeof(startup_eight_segment_order)
          / sizeof(startup_eight_segment_order[0]));
      const uint8_t active_digit =
          runtime->animation_phase == STARTUP_ANIMATION_ROUTE
          ? startup_route_path[runtime->animation_frame].digit
          : (uint8_t)(runtime->animation_frame / segments_per_digit);
      set_blinking_led((DisplayLed)(DISPLAY_LED_D1
          + DISPLAY_DIGIT_COUNT - 1U - active_digit), true);
    }
  }
  else {
    set_blinking_led(command->blinking_led,
                     runtime->led_on
                     && command->blinking_led != DISPLAY_LED_NONE);
  }

  set_buzzer(runtime->buzzer_on);
  if (!command->animate_all_eights
      || runtime->animation_phase == STARTUP_ANIMATION_HOLD_EIGHTS) {
    scan_display(command->digits, command->decimal_mask);
  }
  else if (runtime->animation_phase == STARTUP_ANIMATION_ROUTE) {
    scan_startup_route(runtime->animation_frame);
  }
  else {
    scan_startup_eights(runtime->animation_frame);
  }
}
/* USER CODE END 0 */

int main(void) {

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_USART1_UART_Init();

  /* Initialize interrupts */
  MX_NVIC_Init();
  /* USER CODE BEGIN 2 */

  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* USER CODE END RTOS_QUEUES */

  /* creation of DMA */
  DMAHandle = osMessageQueueNew(COMM_EVENT_QUEUE_LENGTH, sizeof(CommEvent),
                                &DMA_attributes);

  /* creation of RxEvent */
  RxEventHandle = osMessageQueueNew(APP_EVENT_QUEUE_LENGTH, sizeof(AppEvent),
                                    &RxEvent_attributes);

  /* creation of displayCommands */
  displayCommandsHandle = osMessageQueueNew(DISPLAY_QUEUE_LENGTH,
                                             sizeof(DisplayCommand),
                                             &displayCommands_attributes);

  if ((DMAHandle == NULL) || (RxEventHandle == NULL)
      || (displayCommandsHandle == NULL)) {
    Error_Handler();
  }

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of display */
  displayHandle = osThreadNew(fn_display, NULL, &display_attributes);

  /* creation of uartTx */
  uartTxHandle = osThreadNew(fn_uart_tx, NULL, &uartTx_attributes);

  /* creation of keyboard */
  keyboardHandle = osThreadNew(fn_keyboard, NULL, &keyboard_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  if ((defaultTaskHandle == NULL) || (displayHandle == NULL)
      || (uartTxHandle == NULL)
      || (keyboardHandle == NULL)) {
    Error_Handler();
  }
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1) {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

void SystemClock_Config(void) {
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK) {
    Error_Handler();
  }
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_ADC;
  PeriphClkInit.AdcClockSelection = RCC_ADCPCLK2_DIV6;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK) {
    Error_Handler();
  }
}

static void MX_NVIC_Init(void) {
  /* ADC1_2_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(ADC1_2_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(ADC1_2_IRQn);
  /* USART1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(USART1_IRQn);
}

static void MX_ADC1_Init(void) {

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_1CYCLE_5;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

static void MX_USART1_UART_Init(void) {

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK) {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

static void MX_DMA_Init(void) {

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);
  /* DMA1_Channel5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

}

static void MX_GPIO_Init(void) {
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */
  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_10|GPIO_PIN_6|GPIO_PIN_9, GPIO_PIN_RESET);

  /* PB5 drives an active-low buzzer. Keep it silent during startup. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14|GPIO_PIN_15, GPIO_PIN_SET);

  /*Configure GPIO pins : PA1 PA2 PA3 */
  GPIO_InitStruct.Pin = GPIO_PIN_1|GPIO_PIN_2|GPIO_PIN_3;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : PB10 PB12 PB13 PB14
                           PB15 PB5 PB6 PB9 */
  GPIO_InitStruct.Pin = GPIO_PIN_10|GPIO_PIN_12|GPIO_PIN_13|GPIO_PIN_14
                          |GPIO_PIN_15|GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  set_buzzer(false);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
static void app_controller_run(void) {
  AppState state;
  AppEvent event;

  app_initialize(&state);
  for (;;) {
    if (osMessageQueueGet(RxEventHandle, &event, NULL,
                          ms_to_ticks(CONTROLLER_POLL_MS)) == osOK) {
      app_handle_event(&state, &event);
    }
    app_process_deadlines(&state);
  }
}

void fn_uart_tx(void *argument) {
  uint8_t pending_frames[COMM_PENDING_FRAME_COUNT][PROTOCOL_FRAME_SIZE];
  uint8_t pending_head = 0U;
  uint8_t pending_tail = 0U;
  uint8_t pending_count = 0U;
  bool tx_busy = false;
  bool rx_needs_start = true;
  CommEvent event;
  (void)argument;

  for (;;) {
    if (rx_needs_start && (HAL_UART_Receive_DMA(&huart1, BufIN,
                                                PROTOCOL_FRAME_SIZE) == HAL_OK)) {
      rx_needs_start = false;
    }

    if (osMessageQueueGet(DMAHandle, &event, NULL,
                          ms_to_ticks(CONTROLLER_POLL_MS)) == osOK) {
      switch (event.type) {
        case COMM_EVENT_SEND:
          if (pending_count < COMM_PENDING_FRAME_COUNT) {
            if (protocol_encode(&event.data.message,
                                pending_frames[pending_tail])) {
              pending_tail = (uint8_t)((pending_tail + 1U)
                                       % COMM_PENDING_FRAME_COUNT);
              ++pending_count;
            }
          }
          break;
        case COMM_EVENT_RX_FRAME:
        {
          ProtocolMessage decoded;
          if (protocol_decode(event.data.frame, &decoded)) {
            const AppEvent app_event = {
              .type = APP_EVENT_PROTOCOL,
              .data.protocol = decoded
            };
            (void)post_app_event(&app_event);
          }
          break;
        }
        case COMM_EVENT_TX_COMPLETE:
          tx_busy = false;
          break;
        case COMM_EVENT_UART_ERROR:
          (void)HAL_UART_AbortReceive(&huart1);
          (void)HAL_UART_AbortTransmit(&huart1);
          tx_busy = false;
          rx_needs_start = true;
          break;
        default:
          break;
      }
    }

    if (!tx_busy && (pending_count > 0U)) {
      memcpy(BufOUT, pending_frames[pending_head], PROTOCOL_FRAME_SIZE);
      if (HAL_UART_Transmit_DMA(&huart1, BufOUT, PROTOCOL_FRAME_SIZE) == HAL_OK) {
        pending_head = (uint8_t)((pending_head + 1U)
                                 % COMM_PENDING_FRAME_COUNT);
        --pending_count;
        tx_busy = true;
      }
    }
  }
}

void fn_display(void *argument) {
  DisplayRuntime runtime = {
    .command = {
      .digits = {8, 8, 8, 8},
      .decimal_mask = 0x0FU,
      .blinking_led = DISPLAY_LED_NONE
    },
    .animation_phase = STARTUP_ANIMATION_ROUTE,
    .led_on = true
  };
  DisplayCommand incoming;
  uint32_t now = osKernelGetTickCount();
  uint32_t next_scan = now + ms_to_ticks(DISPLAY_SCAN_PERIOD_MS);
  (void)argument;

  reset_pinos_emula_SPI();
  set_blinking_led(DISPLAY_LED_NONE, false);
  set_buzzer(false);
  runtime.next_led_toggle = now + ms_to_ticks(OUTPUT_BLINK_PERIOD_MS);
  display_animation_reset(&runtime, now);

  for (;;) {
    while (osMessageQueueGet(displayCommandsHandle, &incoming, NULL, 0U) == osOK) {
      display_accept_command(&runtime, &incoming, osKernelGetTickCount());
    }

    now = osKernelGetTickCount();
    display_update_runtime(&runtime, now);
    display_render(&runtime);

    if (time_reached(now, next_scan)) {
      next_scan = now + ms_to_ticks(DISPLAY_SCAN_PERIOD_MS);
    }
    (void)osDelayUntil(next_scan);
    next_scan += ms_to_ticks(DISPLAY_SCAN_PERIOD_MS);
  }
}

void fn_keyboard(void *argument) {
  ButtonInput buttons[BUTTON_COUNT] = {
    {.id = BUTTON_A1, .pin = BUTTON_A1_PIN},
    {.id = BUTTON_A2, .pin = BUTTON_A2_PIN},
    {.id = BUTTON_A3, .pin = BUTTON_A3_PIN}
  };
  uint32_t now;
  uint32_t next_poll;
  uint32_t next_chrono;
  uint32_t next_adc;
  (void)argument;

  (void)HAL_ADCEx_Calibration_Start(&hadc1);
  now = osKernelGetTickCount();
  for (uint32_t index = 0U; index < BUTTON_COUNT; ++index) {
    const GPIO_PinState initial_state =
        HAL_GPIO_ReadPin(GPIOA, buttons[index].pin);
    buttons[index].stable_state = initial_state;
    buttons[index].candidate_state = initial_state;
    buttons[index].candidate_since_ms = HAL_GetTick();
  }
  next_poll = now + ms_to_ticks(IO_POLL_MS);
  next_chrono = now + ms_to_ticks(CHRONO_PERIOD_MS);
  next_adc = now + ms_to_ticks(ADC_PERIOD_MS);

  for (;;) {
    now = osKernelGetTickCount();
    const uint32_t now_ms = HAL_GetTick();

    /* Keep the professor's keyboard-task model: all three keys follow the
       same polling and debounce path, so no key depends on an EXTI setup. */
    for (uint32_t index = 0U; index < BUTTON_COUNT; ++index) {
      ButtonInput *button = &buttons[index];
      const GPIO_PinState sampled_state =
          HAL_GPIO_ReadPin(GPIOA, button->pin);

      if (sampled_state != button->candidate_state) {
        button->candidate_state = sampled_state;
        button->candidate_since_ms = now_ms;
      }
      else if ((sampled_state != button->stable_state)
               && ((uint32_t)(now_ms - button->candidate_since_ms)
                   >= BUTTON_DEBOUNCE_MS)) {
        button->stable_state = sampled_state;
        if (sampled_state == GPIO_PIN_RESET) {
          const AppEvent button_event = {
            .type = APP_EVENT_BUTTON_EDGE,
            .data.button = button->id
          };
          (void)post_app_event(&button_event);
        }
      }
    }

    if (time_reached(now, next_chrono)) {
      const AppEvent event = {
        .type = APP_EVENT_CHRONO_TICK
      };
      (void)post_app_event(&event);
      next_chrono += ms_to_ticks(CHRONO_PERIOD_MS);
      if (time_reached(now, next_chrono)) {
        next_chrono = now + ms_to_ticks(CHRONO_PERIOD_MS);
      }
    }

    if (time_reached(now, next_adc)) {
      (void)HAL_ADC_Start_IT(&hadc1);
      next_adc = now + ms_to_ticks(ADC_PERIOD_MS);
    }

    if (time_reached(now, next_poll)) {
      next_poll = now + ms_to_ticks(IO_POLL_MS);
    }
    (void)osDelayUntil(next_poll);
    next_poll += ms_to_ticks(IO_POLL_MS);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
  if (hadc->Instance == ADC1) {
    const AppEvent event = {
      .type = APP_EVENT_ADC_READY,
      .data.value = (uint16_t)HAL_ADC_GetValue(hadc)
    };
    (void)post_app_event(&event);
  }
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    CommEvent event = {
      .type = COMM_EVENT_RX_FRAME
    };
    memcpy(event.data.frame, BufIN, PROTOCOL_FRAME_SIZE);
    (void)post_comm_event(&event);

    if (HAL_UART_Receive_DMA(&huart1, BufIN, PROTOCOL_FRAME_SIZE) != HAL_OK) {
      const CommEvent error_event = {
        .type = COMM_EVENT_UART_ERROR
      };
      (void)post_comm_event(&error_event);
    }
  }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    const CommEvent event = {
      .type = COMM_EVENT_TX_COMPLETE
    };
    (void)post_comm_event(&event);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  if (huart->Instance == USART1) {
    const CommEvent event = {
      .type = COMM_EVENT_UART_ERROR
    };
    (void)post_comm_event(&event);
  }
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartDefaultTask */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void *argument) {
  /* USER CODE BEGIN 5 */
  (void)argument;
  app_controller_run();
  /* USER CODE END 5 */
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
  /* USER CODE BEGIN Callback 0 */
  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM4) {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */
  /* USER CODE END Callback 1 */
}

void Error_Handler(void) {
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1) {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
