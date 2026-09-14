/* Распиновка SLZB-06MU, плата r1.73.
 * Снята с работающего моста через JTAG (GPIO-матрица + IO_MUX) и сверена
 * с официальным hw_defs SMLIGHT: github.com/smlight-tech/slzb-esphome,
 * hw_defs/06xu/r1_73.yaml. */
#pragma once

/* --- W5500 на SPI3 (в терминах ESP-IDF — SPI3_HOST) --- */
#define PIN_SPI_SCLK      42
#define PIN_SPI_MOSI      39
#define PIN_SPI_MISO      41
#define PIN_W5500_CS       2
#define PIN_W5500_INT     38
#define PIN_W5500_RST     40
#define W5500_SPI_HOST    SPI3_HOST
#define W5500_SPI_HZ      (20 * 1000 * 1000)

/* --- EFR32MG21 (NCP) на UART2 --- */
#define PIN_NCP_TX        17   /* ESP -> NCP */
#define PIN_NCP_RX        18   /* NCP -> ESP */
#define PIN_NCP_RTS       14   /* выход ESP: "NCP, притормози" */
#define PIN_NCP_CTS       15   /* вход ESP:  "ESP, притормози"  */
#define PIN_NCP_RST       21   /* инверсный */
#define PIN_NCP_BOOT      16   /* инверсный, вход в загрузчик NCP */
#define NCP_UART_NUM      UART_NUM_2
#define NCP_BAUD          115200

/* --- индикация и органы управления --- */
#define PIN_LED1          46   /* инверсный */
#define PIN_LED2          45   /* инверсный */
#define PIN_RJ45_LEDS      1   /* инверсный */
#define PIN_BUTTON         0   /* инверсная */

/* --- порт, на котором мост отдаёт NCP в сеть --- */
#define NCP_TCP_PORT      6638
