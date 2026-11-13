const store = require('./services/store')

App({
  globalData: {
    theme: null,
    toy: 'crawlPanda',
    ble: null,
    mock: false,
    lastStatus: null
  },

  onLaunch() {
    this.globalData.theme = store.getTheme()
    this.globalData.toy = store.getToy()
  },

  setBle(client, mock) {
    this.globalData.ble = client
    this.globalData.mock = !!mock
  },

  clearBle() {
    this.globalData.ble = null
    this.globalData.mock = false
    this.globalData.lastStatus = null
  }
})
