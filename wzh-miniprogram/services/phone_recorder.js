// WeChat exposes one recorder for the mini-program. Register its listeners once
// and clear the current page callbacks when the recording ends or is cancelled.
let recorder
let session

function finish(method, result) {
  const current = session
  session = null
  if (current && !current.cancelled) current[method](result)
}

function getRecorder() {
  if (recorder) return recorder
  if (!wx.getRecorderManager) throw new Error('当前微信版本不支持手机录音')
  recorder = wx.getRecorderManager()
  recorder.onStart(() => {
    if (!session) return
    if (session.cancelled) recorder.stop()
    else session.onStart()
  })
  recorder.onStop(result => finish('onStop', result))
  recorder.onError(err => finish('onError', err))
  if (recorder.onInterruptionBegin) {
    recorder.onInterruptionBegin(() => {
      if (!session || session.cancelled) return
      const report = session.onError
      cancelSession(session)
      report({ errMsg: '手机录音被中断，请重新录制' })
    })
  }
  return recorder
}

function cancelSession(current) {
  if (session !== current || current.cancelled) return
  current.cancelled = true
  current.onStart = current.onStop = current.onError = null
  recorder.stop()
}

function start(callbacks) {
  if (session) throw new Error('上一次手机录音仍在结束，请稍候')
  const manager = getRecorder()
  const current = Object.assign({ cancelled: false }, callbacks)
  session = current
  try {
    manager.start({ duration: 20000, sampleRate: 16000, numberOfChannels: 1, format: 'wav' })
  } catch (error) {
    session = null
    throw error
  }
  return {
    stop() { if (session === current && !current.cancelled) manager.stop() },
    cancel() { cancelSession(current) }
  }
}

module.exports = { start }
