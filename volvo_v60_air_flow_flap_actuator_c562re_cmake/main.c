/**
 ******************************************************************************
 * file           : main.c
 * brief          : Main program body
 *                  Calls target system initialization then loop in main.
 ******************************************************************************
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "mx_gpio_default.h"
#include "mx_usart3.h"
#include "ssd1306_fonts.h"
#include "ssd1306_mx2.h"
#include "stm32c562xx.h"
#include "stm32c5xx_hal.h"
#include "stm32c5xx_hal_uart.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Private typedef -----------------------------------------------------------*/
/* Private define ------------------------------------------------------------*/

#define DRIVE_ID 0x1A // 0x12, 0x14, 0x18, 0x1A
#define LIN_USART USART3
#define LIN_TX_BUFFER_SIZE 5
#define LIN_RX_BUFFER_SIZE 16
#define VCP_RX_BUFFER_SIZE 16
#define LIN_SEND_PERIOD 100

/* Private macro -------------------------------------------------------------*/
/* Private variables ---------------------------------------------------------*/

hal_uart_handle_t *huart3_lin;
uint8_t LinTxData[LIN_TX_BUFFER_SIZE];
uint8_t LinRxData[LIN_RX_BUFFER_SIZE];
uint8_t LinRxDataBis[LIN_RX_BUFFER_SIZE];
uint32_t LinSoftTimer;
uint8_t lin_data_received_flag = 0;

hal_uart_handle_t *huart2_vcp;
uint8_t VcpRxData[VCP_RX_BUFFER_SIZE];

hal_tim_handle_t *htim2_encoder;
uint8_t adjustable_byte;

uint8_t button_toggle = 0;
uint8_t send_toggle = 0;

char lcd_line[32];

/* Private functions prototype -----------------------------------------------*/

// https://controllerstech.com/stm32-uart-8-lin-protocol-part-1/
uint8_t Pid_Calc(uint8_t ID);
uint8_t Checksum_Calc(uint8_t PID, uint8_t *data, uint8_t size);
void Error_Handler(void);

/**
 * brief:  The application entry point.
 * retval: none but we specify int to comply with C99 standard
 */
int main(void) {
  /** System Init: this code placed in targets folder initializes your system.
   * It calls the initialization (and sets the initial configuration) of the
   * peripherals. You can use STM32CubeMX to generate and call this code or not
   * in this project. It also contains the HAL initialization and the initial
   * clock configuration.
   */
  if (mx_system_init() != SYSTEM_OK) {
    return (-1);
  } else {
    /*
     * You can start your application code here
     */

    ssd1306_Init();

    ssd1306_Fill(Black);
    ssd1306_SetCursor(20, 0);
    ssd1306_WriteString("ufnalski.edu.pl", Font_6x8, White);
    ssd1306_SetCursor(4, 12);
    ssd1306_WriteString("Volvo flap actuator", Font_6x8, White);
    ssd1306_UpdateScreen();

    htim2_encoder = mx_tim2_gethandle(); // encoder knob
    HAL_TIM_SetCounter(htim2_encoder, 0);
    HAL_TIM_Start_IT(htim2_encoder);

    huart3_lin = mx_usart3_uart_gethandle();

    HAL_UART_ReceiveToIdle_IT(huart3_lin, LinRxData, LIN_RX_BUFFER_SIZE);

    LIN_USART->CR2 |= USART_CR2_LBDIE; // LIN break detection interrupt enable

    huart2_vcp = mx_usart2_uart_gethandle();
    HAL_UART_ReceiveToIdle_DMA(huart2_vcp, VcpRxData, VCP_RX_BUFFER_SIZE);

    LinSoftTimer = HAL_GetTick();

    while (1) {

      if (lin_data_received_flag == 1) {
        lin_data_received_flag = 0;
        sprintf(lcd_line, "RX:[%02X] %02X%02X %02X%02X %02X", DRIVE_ID,
                LinRxDataBis[2], LinRxDataBis[3], LinRxDataBis[4],
                LinRxDataBis[5], LinRxDataBis[6]);
        ssd1306_SetCursor(2, 28);
        ssd1306_WriteString(lcd_line, Font_6x8, White);
        sprintf(lcd_line, "Position: %d",
                (((uint16_t)(LinRxDataBis[2] & 0x0F)) << 8) | LinRxDataBis[3]);
        ssd1306_SetCursor(2, 40);
        ssd1306_WriteString(lcd_line, Font_6x8, White);
        sprintf(lcd_line, "Status: %s",
                (LinRxDataBis[6] == 0) ? "active" : "locked");
        ssd1306_SetCursor(2, 52);
        ssd1306_WriteString(lcd_line, Font_6x8, White);
        ssd1306_UpdateScreen();
      }

      if (HAL_GetTick() - LinSoftTimer > LIN_SEND_PERIOD) {
        LinSoftTimer = HAL_GetTick();

        // while (!(LIN_USART->ISR & USART_ISR_TXE)) {
        //   __NOP();
        // }
        HAL_UART_SendLINBreak(huart3_lin);
      }
    }
  }
} /* end main */

void HAL_UART_ErrorCallback(hal_uart_handle_t *huart) { // just for debugging
  if (huart->instance == HAL_UART1) {
    if (LIN_USART->ISR & USART_ISR_FE) { // framing error
      LIN_USART->ICR |= USART_ICR_FECF;
      __NOP();
    }
  }
}

void HAL_UART_LINBreakCallback(hal_uart_handle_t *huart) {
  if (huart->instance == HAL_UART3) {
    LIN_USART->ICR |= USART_ICR_LBDCF; // clear flag

    if (send_toggle == 0) {
      send_toggle = 1;
      adjustable_byte = HAL_TIM_GetCounter(htim2_encoder);
      LinTxData[0] = 0x55; // sync field
      LinTxData[1] = Pid_Calc(DRIVE_ID);

      if (button_toggle == 0) {
        LinTxData[2] = adjustable_byte | 0xF0;
        LinTxData[3] = 0x00; // I haven't experimented with that byte
      } else {               // some unknown unlock payload :)
        LinTxData[2] = rand();
        LinTxData[3] = rand();
      }

      LinTxData[4] =
          Checksum_Calc(0x00, LinTxData + 2, 2); // LIN 1.x excludes PID

      LIN_USART->RQR |=
          USART_RQR_RXFRQ; // flush erroneous byte (coming from break field)
      memset(LinRxData, 0x00, LIN_RX_BUFFER_SIZE);
      HAL_UART_ReceiveToIdle_IT(huart3_lin, LinRxData, LIN_RX_BUFFER_SIZE);
      HAL_UART_Transmit_IT(huart3_lin, LinTxData, LIN_TX_BUFFER_SIZE);
    } else {
      {
        send_toggle = 0;
        // ask servo drive for position
        LinTxData[0] = 0x55;
        LinTxData[1] = Pid_Calc(DRIVE_ID + 1);

        HAL_GPIO_TogglePin(LOGIC_ANALYZER_PORT, LOGIC_ANALYZER_PIN);

        LIN_USART->RQR |=
            USART_RQR_RXFRQ; // flush erroneous byte (coming from break field)
        memset(LinRxData, 0x00, LIN_RX_BUFFER_SIZE);
        HAL_UART_ReceiveToIdle_IT(huart3_lin, LinRxData, LIN_RX_BUFFER_SIZE);
        HAL_UART_Transmit_IT(huart3_lin, LinTxData, 2);
      }
    }
  }
}

void HAL_UART_RxCpltCallback(hal_uart_handle_t *huart, uint32_t size_byte,
                             hal_uart_rx_event_types_t rx_event) {
  if (huart->instance == HAL_UART3) {
    if (size_byte == 8) {
      lin_data_received_flag = 1;
      memcpy(LinRxDataBis, LinRxData, LIN_RX_BUFFER_SIZE);
    }
  }

  if (huart->instance == HAL_UART2) {
    HAL_UART_ReceiveToIdle_DMA(huart2_vcp, VcpRxData, VCP_RX_BUFFER_SIZE);
  }
}

void HAL_EXTI_TriggerCallback(hal_exti_handle_t *hexti,
                              hal_exti_trigger_t trigger) {
  if ((hexti->line == HAL_EXTI_LINE_0) &&
      (trigger == HAL_EXTI_TRIGGER_FALLING)) {
    button_toggle ^= 1;
  }
}

uint8_t Pid_Calc(uint8_t ID) {
  if (ID > 0x3F) {
    Error_Handler();
  }
  uint8_t IDBuf[6];
  for (int i = 0; i < 6; i++) {
    IDBuf[i] = (ID >> i) & 0x01;
  }

  uint8_t P0 = (IDBuf[0] ^ IDBuf[1] ^ IDBuf[2] ^ IDBuf[4]) & 0x01;
  uint8_t P1 = ~((IDBuf[1] ^ IDBuf[3] ^ IDBuf[4] ^ IDBuf[5]) & 0x01);

  ID = ID | (P0 << 6) | (P1 << 7);
  return ID;
}

uint8_t Checksum_Calc(uint8_t PID, uint8_t *data, uint8_t size) {
  uint8_t buffer[size + 2];
  uint16_t sum = 0;
  buffer[0] = PID;
  for (int i = 0; i < size; i++) {
    buffer[i + 1] = data[i];
  }

  for (int i = 0; i < size + 1; i++) {
    sum = sum + buffer[i];
    if (sum > 0xff)
      sum = sum - 0xff;
  }

  sum = 0xff - sum;
  return sum;
}

void Error_Handler(void) {
  while (1) {
    // Choose your favorite way to go into the panic mode :)
  }
}
