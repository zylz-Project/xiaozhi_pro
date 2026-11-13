module.exports = {
  // BLE 协议：Nordic UART 风格，服务/特征值可按固件实现替换，只需同步 docs/ble-protocol.md。
  BLE: {
    SERVICE_UUID: '6E400001-B5A3-F393-E0A9-E50E24DCCA9E',
    RX_UUID: '6E400002-B5A3-F393-E0A9-E50E24DCCA9E',
    TX_UUID: '6E400003-B5A3-F393-E0A9-E50E24DCCA9E',
    NAME_PREFIXES: ['CrawlPanda', 'Dino', 'RedPanda', 'WZH'],
    SCAN_TIMEOUT: 8000,
    CONNECT_TIMEOUT: 10000,
    MTU: 180
  },
  PROTOCOL_VERSION: 2,
  // 云能力占位：声音复刻上传/生成。未配置时小程序会走本地演示逻辑。
  CLOUD_BASE: 'https://wzh-api.example.com',
  USE_MOCK_IN_DEVTOOLS: true
}
