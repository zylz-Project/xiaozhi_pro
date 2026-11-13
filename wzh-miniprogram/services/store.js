const THEME_KEY = 'panda-appearance-v1'
const TOY_KEY = 'panda-toy'
const DEVICE_KEY = 'wzh-device-v1'
const THEMES = ['milk', 'salt', 'berry', 'bamboo']
const DEFAULTS = { theme: 'milk', material: 'b', layout: 'c', mode: 'light' }

function normalize(raw) {
  const out = Object.assign({}, DEFAULTS)
  if (!raw || typeof raw !== 'object') return out
  if (THEMES.indexOf(raw.theme) >= 0) out.theme = raw.theme
  if (raw.material === 'a' || raw.material === 'b') out.material = raw.material
  if (raw.layout === 'c' || raw.layout === 'd') out.layout = raw.layout
  if (raw.mode === 'dark' || raw.mode === 'light') out.mode = raw.mode
  return out
}

function getTheme(defaultTheme) {
  try {
    const raw = wx.getStorageSync(THEME_KEY)
    if (raw && typeof raw === 'object') return normalize(raw)
  } catch (e) {}
  const fallback = Object.assign({}, DEFAULTS)
  if (THEMES.indexOf(defaultTheme) >= 0) fallback.theme = defaultTheme
  return fallback
}

function setTheme(patch, defaultTheme) {
  const next = normalize(Object.assign({}, getTheme(defaultTheme), patch || {}))
  try { wx.setStorageSync(THEME_KEY, next) } catch (e) {}
  return next
}

function getToy() {
  try { return wx.getStorageSync(TOY_KEY) || 'crawlPanda' } catch (e) { return 'crawlPanda' }
}

function setToy(id) {
  try { wx.setStorageSync(TOY_KEY, id) } catch (e) {}
}

function getLastDevice() {
  try { return wx.getStorageSync(DEVICE_KEY) || null } catch (e) { return null }
}

function setLastDevice(device) {
  try { wx.setStorageSync(DEVICE_KEY, device || null) } catch (e) {}
}

module.exports = {
  THEME_KEY,
  TOY_KEY,
  DEVICE_KEY,
  DEFAULTS,
  normalize,
  getTheme,
  setTheme,
  getToy,
  setToy,
  getLastDevice,
  setLastDevice
}
