const config = require('./config')

// 云能力是可选的：没有配置真实后端时，所有声音复刻仍可由 BLE mock/本地逻辑完成。
// 生产环境把 config.CLOUD_BASE 换成已在小程序后台配置的 HTTPS 域名，并设置 urlCheck=true。
const TOKEN_KEY = 'wzh-cloud-token'
const PLACEHOLDER = /example\.com$/i

function isEnabled() {
  return !!(config.CLOUD_BASE && !PLACEHOLDER.test(config.CLOUD_BASE))
}

function base() {
  return String(config.CLOUD_BASE || '').replace(/\/$/, '')
}

function getToken() {
  try { return wx.getStorageSync(TOKEN_KEY) || '' } catch (e) { return '' }
}

function setToken(token) {
  try {
    if (token) wx.setStorageSync(TOKEN_KEY, token)
    else wx.removeStorageSync(TOKEN_KEY)
  } catch (e) {}
}

function request(path, method, data) {
  if (!isEnabled()) return Promise.reject(Object.assign(new Error('云服务未配置'), { code: 'cloud_disabled' }))
  return new Promise((resolve, reject) => {
    wx.request({
      url: base() + path,
      method: method || 'GET',
      data: data || {},
      timeout: 20000,
      header: {
        'content-type': 'application/json',
        Authorization: getToken() ? 'Bearer ' + getToken() : ''
      },
      success: res => {
        const body = res.data || {}
        if (res.statusCode >= 200 && res.statusCode < 300 && body.ok !== false) {
          resolve(body.data !== undefined ? body.data : body)
        } else {
          const err = new Error((body.error && body.error.message) || ('云服务错误 ' + res.statusCode))
          err.code = (body.error && body.error.code) || res.statusCode
          reject(err)
        }
      },
      fail: err => reject(Object.assign(new Error((err && err.errMsg) || '网络请求失败'), { code: 'network' }))
    })
  })
}

// 无登录页：用 wx.login 的 code 换取短期云会话，绑定关系仍由 BLE 设备完成。
async function ensureSession() {
  if (!isEnabled()) throw Object.assign(new Error('云服务未配置'), { code: 'cloud_disabled' })
  if (getToken()) return getToken()
  const code = await new Promise((resolve, reject) => {
    wx.login({
      success: res => (res.code ? resolve(res.code) : reject(new Error('微信登录失败'))),
      fail: err => reject(new Error((err && err.errMsg) || '微信登录失败'))
    })
  })
  const data = await request('/v1/auth/wx', 'POST', { code })
  setToken(data.token || '')
  return data.token || ''
}

function uploadVoice(filePath, meta) {
  if (!isEnabled()) return Promise.reject(Object.assign(new Error('云服务未配置'), { code: 'cloud_disabled' }))
  return ensureSession().then(() => new Promise((resolve, reject) => {
    wx.uploadFile({
      url: base() + '/v1/voice/upload',
      filePath: filePath,
      name: 'file',
      timeout: 30000,
      formData: Object.assign({ product: 'crawlPanda' }, meta || {}),
      header: { Authorization: 'Bearer ' + getToken() },
      success: res => {
        let body = {}
        try { body = JSON.parse(res.data || '{}') } catch (e) {}
        if (res.statusCode >= 200 && res.statusCode < 300 && body.ok !== false) {
          resolve(body.data !== undefined ? body.data : body)
        } else {
          reject(Object.assign(new Error((body.error && body.error.message) || '上传失败'), { code: (body.error && body.error.code) || res.statusCode }))
        }
      },
      fail: err => reject(Object.assign(new Error((err && err.errMsg) || '上传失败'), { code: 'network' }))
    })
  }))
}

function createClone(payload) {
  return request('/v1/voice/clone', 'POST', payload || {})
}

function getClone(cloneId) {
  return request('/v1/voice/clone/' + encodeURIComponent(cloneId), 'GET')
}

function listClones(deviceId) {
  return request('/v1/voice/clones?deviceId=' + encodeURIComponent(deviceId || ''), 'GET')
}

function deleteClone(cloneId) {
  return request('/v1/voice/clones/' + encodeURIComponent(cloneId), 'DELETE')
}

function audition(cloneId) {
  return request('/v1/voice/audition', 'POST', { cloneId })
}

module.exports = {
  isEnabled,
  base,
  getToken,
  setToken,
  ensureSession,
  uploadVoice,
  createClone,
  getClone,
  listClones,
  deleteClone,
  audition
}
