function pad(n) { return n < 10 ? '0' + n : '' + n }

function formatDuration(seconds) {
  const s = Math.max(0, Math.floor(seconds || 0))
  return pad(Math.floor(s / 60)) + ':' + pad(s % 60)
}

function formatRuntime(minutes) {
  const m = Math.max(0, Math.floor(minutes || 0))
  if (m < 60) return m + 'm'
  const h = Math.floor(m / 60)
  const rest = m % 60
  return rest ? h + 'h' + pad(rest) : h + 'h'
}

function signalText(rssi) {
  if (rssi >= -55) return '强'
  if (rssi >= -70) return '良好'
  if (rssi >= -82) return '一般'
  return '弱'
}

function signalBars(rssi) {
  if (rssi >= -55) return 4
  if (rssi >= -70) return 3
  if (rssi >= -82) return 2
  return 1
}

function clamp(n, min, max) { return Math.min(max, Math.max(min, n)) }

// 设备上报的原始数值可能是浮点（真机温度 51.79999…），界面一律取整显示。
function formatTemp(value) {
  const n = Number(value)
  if (!isFinite(n) || n <= 0) return '--'
  return String(Math.round(n))
}

// 温度分级：>=55 偏高（与固件 tempState 一致），45-54 偏热，其余正常。
function tempLevel(value) {
  const n = Number(value)
  if (!isFinite(n) || n <= 0) return ''
  if (n >= 55) return 'hot'
  if (n >= 45) return 'warm'
  return ''
}

function tempStateText(value) {
  const level = tempLevel(value)
  if (level === 'hot') return '偏高'
  if (level === 'warm') return '偏热'
  return '正常'
}

// 蓝牙往返时延：取实测 ping RTT；设备没给有效值时显示占位符而不是 0ms。
function formatLatency(ms) {
  const n = Math.round(Number(ms) || 0)
  if (!isFinite(n) || n <= 0) return '--'
  return n + 'ms'
}

function initial(name) {
  const chars = String(name || '').trim().split('')
  const cjk = chars.filter(ch => /[\u3400-\u9fff]/.test(ch))
  return (cjk[cjk.length - 1] || chars[0] || '声').slice(0, 1)
}

module.exports = { formatDuration, formatRuntime, formatTemp, tempLevel, tempStateText, formatLatency, signalText, signalBars, clamp, initial }
