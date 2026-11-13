// 音频工具：WAV 解析 → 16kHz 单声道 PCM16，以及按固件协议分片上传。
// 固件 voice_upload_* 只接受 5–20 秒、16kHz、单声道 PCM16LE（160000–640000 字节）。

const TARGET_RATE = 16000
const MIN_SECONDS = 5
const MAX_SECONDS = 20
const CHUNK_BYTES = 2048

function fail(message) {
  return Promise.reject(new Error(message))
}

function readFile(filePath) {
  return new Promise((resolve, reject) => {
    if (!filePath || filePath.indexOf('mock://') === 0) {
      reject(new Error('演示文件不能直接上传'))
      return
    }
    if (!wx.getFileSystemManager) {
      reject(new Error('当前微信版本不支持读取文件'))
      return
    }
    wx.getFileSystemManager().readFile({
      filePath: filePath,
      success: res => resolve(res.data),
      fail: err => reject(new Error((err && err.errMsg) || '读取音频失败'))
    })
  })
}

function readSample(view, p, bits, format) {
  if (bits === 8) return (view.getUint8(p) - 128) / 128
  if (bits === 16) return view.getInt16(p, true) / 32768
  if (bits === 24) {
    let v = view.getUint8(p) | (view.getUint8(p + 1) << 8) | (view.getUint8(p + 2) << 16)
    if (v & 0x800000) v |= 0xff000000
    return v / 8388608
  }
  return format === 3 ? view.getFloat32(p, true) : view.getInt32(p, true) / 2147483648
}

function parseWav(buffer) {
  if (!buffer || !buffer.byteLength || buffer.byteLength < 44) throw new Error('WAV 文件为空或过短')
  const view = new DataView(buffer)
  const tag = (offset) => String.fromCharCode(view.getUint8(offset), view.getUint8(offset + 1), view.getUint8(offset + 2), view.getUint8(offset + 3))
  if (tag(0) !== 'RIFF' || tag(8) !== 'WAVE') throw new Error('不是有效的 WAV 文件')

  let fmt = null
  let dataOffset = 0
  let dataLength = 0
  let pos = 12
  while (pos + 8 <= view.byteLength) {
    const id = tag(pos)
    const size = view.getUint32(pos + 4, true)
    const body = pos + 8
    if (id === 'fmt ' && size >= 16) {
      let format = view.getUint16(body, true)
      let channels = view.getUint16(body + 2, true)
      let sampleRate = view.getUint32(body + 4, true)
      let bits = view.getUint16(body + 14, true)
      if (format === 0xfffe && size >= 26) format = view.getUint16(body + 24, true)
      fmt = { format, channels, sampleRate, bits }
    } else if (id === 'data') {
      dataOffset = body
      dataLength = Math.min(size, view.byteLength - body)
    }
    pos = body + size + (size % 2)
  }
  if (!fmt || !dataOffset || !dataLength) throw new Error('WAV 缺少 fmt 或 data 段')
  if (fmt.format !== 1 && fmt.format !== 3) throw new Error('只支持 PCM 或浮点 WAV')
  if (fmt.channels < 1 || fmt.channels > 2) throw new Error('只支持单声道或双声道 WAV')
  if ([8, 16, 24, 32].indexOf(fmt.bits) < 0) throw new Error('不支持 ' + fmt.bits + ' 位 WAV')
  if (!fmt.sampleRate) throw new Error('WAV 采样率无效')

  const frameBytes = (fmt.bits / 8) * fmt.channels
  const frames = Math.floor(dataLength / frameBytes)
  const mono = new Float32Array(frames)
  const sampleBytes = fmt.bits / 8
  for (let i = 0; i < frames; i++) {
    const p = dataOffset + i * frameBytes
    if (fmt.channels === 1) mono[i] = readSample(view, p, fmt.bits, fmt.format)
    else {
      const a = readSample(view, p, fmt.bits, fmt.format)
      const b = readSample(view, p + sampleBytes, fmt.bits, fmt.format)
      mono[i] = (a + b) / 2
    }
  }
  return Object.assign({ samples: mono }, fmt)
}

function resample(samples, sourceRate) {
  if (sourceRate === TARGET_RATE) return samples
  const ratio = sourceRate / TARGET_RATE
  const length = Math.max(1, Math.round(samples.length / ratio))
  const out = new Float32Array(length)
  for (let i = 0; i < length; i++) {
    const pos = i * ratio
    const left = Math.floor(pos)
    const right = Math.min(samples.length - 1, left + 1)
    const mix = pos - left
    out[i] = samples[left] * (1 - mix) + samples[right] * mix
  }
  return out
}

function toPcm16(samples) {
  const out = new Int16Array(samples.length)
  for (let i = 0; i < samples.length; i++) {
    const v = Math.max(-1, Math.min(1, samples[i]))
    out[i] = v < 0 ? Math.round(v * 32768) : Math.round(v * 32767)
  }
  return out
}

function wavToPcm(buffer) {
  const wav = parseWav(buffer)
  let samples = resample(wav.samples, wav.sampleRate)
  let duration = samples.length / TARGET_RATE
  if (duration < MIN_SECONDS) {
    throw new Error('音频只有 ' + duration.toFixed(1) + ' 秒，请上传 5–20 秒')
  }
  if (duration > MAX_SECONDS) {
    samples = samples.subarray(0, TARGET_RATE * MAX_SECONDS)
    duration = MAX_SECONDS
  }
  const pcm = toPcm16(samples)
  return {
    pcm,
    bytes: pcm.length * 2,
    duration,
    sampleRate: TARGET_RATE,
    sourceRate: wav.sampleRate,
    channels: wav.channels
  }
}

function mockPcm(seconds) {
  const count = Math.round(TARGET_RATE * (seconds || 6))
  const pcm = new Int16Array(count)
  for (let i = 0; i < count; i++) {
    pcm[i] = Math.round(Math.sin((2 * Math.PI * 220 * i) / TARGET_RATE) * 6000)
  }
  return { pcm, bytes: pcm.length * 2, duration: count / TARGET_RATE, sampleRate: TARGET_RATE, sourceRate: TARGET_RATE, channels: 1 }
}

const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'

function base64(bytes) {
  let out = ''
  for (let i = 0; i < bytes.length; i += 3) {
    const a = bytes[i]
    const b = i + 1 < bytes.length ? bytes[i + 1] : 0
    const c = i + 2 < bytes.length ? bytes[i + 2] : 0
    out += B64[a >> 2]
    out += B64[((a & 3) << 4) | (b >> 4)]
    out += i + 1 < bytes.length ? B64[((b & 15) << 2) | (c >> 6)] : '='
    out += i + 2 < bytes.length ? B64[c & 63] : '='
  }
  return out
}

function delay(ms) { return new Promise(resolve => setTimeout(resolve, ms)) }

async function uploadPcm(client, pcm, name, onProgress) {
  const bytes = new Uint8Array(pcm.pcm.buffer, pcm.pcm.byteOffset, pcm.pcm.byteLength)
  if (bytes.length < 160000 || bytes.length > 640000 || bytes.length % 2) {
    throw new Error('音频长度必须为 5–20 秒')
  }
  const begin = await client.request('voice_upload_begin', { name: name, bytes: bytes.length }, 15000)
  const limit = Math.max(256, Math.min(CHUNK_BYTES, Number(begin && begin.chunkLimit) || CHUNK_BYTES))
  let sent = 0
  try {
    for (let offset = 0; offset < bytes.length; offset += limit) {
      const end = Math.min(bytes.length, offset + limit)
      const data = base64(bytes.subarray(offset, end))
      await client.request('voice_upload_chunk', { data: data }, 30000)
      sent = end
      if (onProgress) onProgress(sent / bytes.length)
    }
    await client.request('voice_upload_end', {}, 15000)
  } catch (error) {
    // A failed transfer otherwise leaves the device's upload buffer busy
    // until disconnect, so a second attempt from the same session fails.
    try { await client.request('voice_upload_cancel', {}, 5000) } catch (e) {}
    throw error
  }
  await delay(600)

  let sawBusy = false
  for (let i = 0; i < 100; i++) {
    let status = null
    try { status = await client.request('get_status', {}, 12000) } catch (e) {}
    if (status) {
      if (status.voice_management_busy) sawBusy = true
      if (!status.voice_management_busy && (sawBusy || i >= 5)) {
        const state = String(status.voice_management_status || '')
        if (state === 'ok') return status
        if (!state || state === 'uncertain') {
          throw new Error('音色生成结果尚未确认，请先在音色管理中检查')
        }
        if (state === 'cancelled') throw new Error('音色生成已取消')
        if (state === 'network') throw new Error('网络异常，音色生成失败')
        if (state === 'config') throw new Error('设备未配置语音服务，暂时不能生成音色')
        if (state === 'full') throw new Error('复刻音色已满，请先删除一个')
        throw new Error('音色生成失败（' + state + '）')
      }
    }
    await delay(800)
  }
  throw new Error('音色生成超时，请稍后在音色管理中刷新')
}

module.exports = {
  TARGET_RATE,
  MIN_SECONDS,
  MAX_SECONDS,
  readFile,
  parseWav,
  wavToPcm,
  mockPcm,
  base64,
  uploadPcm
}
