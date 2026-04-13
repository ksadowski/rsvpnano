#include "display/rm67162.h"

#include <driver/spi_master.h>
#include <esp_log.h>

#include "board/BoardConfig.h"

namespace {

constexpr int kSpiFrequency = 75000000;
constexpr int kSendBufferPixels = 0x4000;
static const char *kRm67162Tag = "rm67162";

struct LcdCommand {
  uint8_t cmd;
  uint8_t data[4];
  uint8_t len;
};

constexpr uint8_t kMadctl = 0x36;
constexpr uint8_t kMadMy = 0x80;
constexpr uint8_t kMadMx = 0x40;
constexpr uint8_t kMadMv = 0x20;
constexpr uint8_t kMadRgb = 0x00;

constexpr LcdCommand kQspiInit[] = {
    {0x11, {0x00}, 0x80},
    {0x3A, {0x55}, 0x01},
    {0x51, {0x00}, 0x01},
    {0x29, {0x00}, 0x80},
    {0x51, {0xD0}, 0x01},
};

spi_device_handle_t gSpi = nullptr;
bool gBusReady = false;

void setChipSelect(bool high) { digitalWrite(BoardConfig::PIN_LCD_CS, high ? HIGH : LOW); }

void sendCommand(uint32_t command, const uint8_t *data, uint32_t length) {
  if (gSpi == nullptr) {
    return;
  }

  setChipSelect(false);

  spi_transaction_t transaction = {};
  transaction.flags = SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
  transaction.cmd = 0x02;
  transaction.addr = command << 8;
  if (length != 0) {
    transaction.tx_buffer = data;
    transaction.length = length * 8;
  }

  ESP_ERROR_CHECK(spi_device_polling_transmit(gSpi, &transaction));
  setChipSelect(true);
}

void setAddressWindow(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
  const LcdCommand commands[] = {
      {0x2A, {static_cast<uint8_t>(x1 >> 8), static_cast<uint8_t>(x1),
              static_cast<uint8_t>(x2 >> 8), static_cast<uint8_t>(x2)},
       0x04},
      {0x2B, {static_cast<uint8_t>(y1 >> 8), static_cast<uint8_t>(y1),
              static_cast<uint8_t>(y2 >> 8), static_cast<uint8_t>(y2)},
       0x04},
      {0x2C, {0x00}, 0x00},
  };

  for (const auto &command : commands) {
    sendCommand(command.cmd, command.data, command.len);
  }
}

}  // namespace

void rm67162Init() {
  pinMode(BoardConfig::PIN_LCD_CS, OUTPUT);
  digitalWrite(BoardConfig::PIN_LCD_CS, HIGH);

  pinMode(BoardConfig::PIN_LCD_RST, OUTPUT);
  pinMode(BoardConfig::PIN_LCD_EN, OUTPUT);
  digitalWrite(BoardConfig::PIN_LCD_EN, LOW);
  delay(20);
  digitalWrite(BoardConfig::PIN_LCD_EN, HIGH);
  delay(20);

  digitalWrite(BoardConfig::PIN_LCD_RST, LOW);
  delay(300);
  digitalWrite(BoardConfig::PIN_LCD_RST, HIGH);
  delay(200);

  if (!gBusReady) {
    spi_bus_config_t busConfig = {};
    busConfig.data0_io_num = BoardConfig::PIN_LCD_DATA0;
    busConfig.data1_io_num = BoardConfig::PIN_LCD_DATA1;
    busConfig.sclk_io_num = BoardConfig::PIN_LCD_SCLK;
    busConfig.data2_io_num = BoardConfig::PIN_LCD_DATA2;
    busConfig.data3_io_num = BoardConfig::PIN_LCD_DATA3;
    busConfig.max_transfer_sz = (kSendBufferPixels * static_cast<int>(sizeof(uint16_t))) + 8;
    busConfig.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS;

    spi_device_interface_config_t deviceConfig = {};
    deviceConfig.command_bits = 8;
    deviceConfig.address_bits = 24;
    deviceConfig.mode = SPI_MODE0;
    deviceConfig.clock_speed_hz = kSpiFrequency;
    deviceConfig.spics_io_num = -1;
    deviceConfig.flags = SPI_DEVICE_HALFDUPLEX;
    deviceConfig.queue_size = 17;

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &busConfig, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &deviceConfig, &gSpi));
    gBusReady = true;
  }

  for (int pass = 0; pass < 3; ++pass) {
    for (const auto &command : kQspiInit) {
      sendCommand(command.cmd, command.data, command.len & 0x7F);
      if ((command.len & 0x80) != 0) {
        delay(120);
      }
    }
  }

  ESP_LOGI(kRm67162Tag, "RM67162 QSPI init complete");
}

void rm67162Sleep() {
  sendCommand(0x28, nullptr, 0);
  delay(50);
  sendCommand(0x10, nullptr, 0);
  delay(120);
  digitalWrite(BoardConfig::PIN_LCD_EN, LOW);
  digitalWrite(BoardConfig::PIN_LCD_CS, HIGH);
}

void rm67162SetRotation(uint8_t rotation) {
  uint8_t value = kMadRgb;

  switch (rotation & 0x03) {
    case 1:
      value = kMadMx | kMadMv | kMadRgb;
      break;
    case 2:
      value = kMadMx | kMadMy | kMadRgb;
      break;
    case 3:
      value = kMadMv | kMadMy | kMadRgb;
      break;
    default:
      break;
  }

  sendCommand(kMadctl, &value, 1);
}

void rm67162PushColors(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                       const uint16_t *data) {
  if (gSpi == nullptr || data == nullptr || width == 0 || height == 0) {
    return;
  }

  bool firstSend = true;
  size_t pixelsRemaining = static_cast<size_t>(width) * height;
  const uint16_t *cursor = data;

  setAddressWindow(x, y, x + width - 1, y + height - 1);
  setChipSelect(false);

  while (pixelsRemaining > 0) {
    size_t chunkPixels = pixelsRemaining;
    if (chunkPixels > static_cast<size_t>(kSendBufferPixels)) {
      chunkPixels = kSendBufferPixels;
    }

    spi_transaction_ext_t transaction = {};
    if (firstSend) {
      transaction.base.flags = SPI_TRANS_MODE_QIO;
      transaction.base.cmd = 0x32;
      transaction.base.addr = 0x002C00;
      firstSend = false;
    } else {
      transaction.base.flags =
          SPI_TRANS_MODE_QIO | SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR |
          SPI_TRANS_VARIABLE_DUMMY;
      transaction.command_bits = 0;
      transaction.address_bits = 0;
      transaction.dummy_bits = 0;
    }

    transaction.base.tx_buffer = cursor;
    transaction.base.length = chunkPixels * 16;

    ESP_ERROR_CHECK(
        spi_device_polling_transmit(gSpi, reinterpret_cast<spi_transaction_t *>(&transaction)));

    pixelsRemaining -= chunkPixels;
    cursor += chunkPixels;
  }

  setChipSelect(true);
}
