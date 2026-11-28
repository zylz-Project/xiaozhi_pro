/* Follow the firmware's transport decision across hotspot interruptions. */
window.WzhNetworkTransport = (() => {
  'use strict';
  const label = target => target === 'cellular' ? '4G' : 'Wi-Fi';

  async function select(target, {initial, request, read, progress, recheck = false}) {
    const deadline = Date.now() + 45000;
    const before = initial.transport_switch?.id;
    const action = label(target) + (recheck ? ' 重新检测' : ' 切换');
    let net = initial, sent = false, acceptedId, replyLost = false, unchanged = 0;
    let interrupted = false;
    while (Date.now() < deadline) {
      if (net) {
        const job = net.transport_switch;
        const pending = job?.state === 'pending';
        const ours = sent && job?.target === target &&
          (acceptedId ? job.id === acceptedId : job.id !== before);
        if (ours && job.state === 'failed')
          throw new Error((!recheck && job.message) || action + '失败，请重试');
        const selected = net.transport === target || (!net.transport && target === 'wifi');
        if (!pending && selected && (!recheck || (ours && job.state === 'succeeded')))
          return net;
        if (!pending && net.transport !== 'probing') {
          if (!sent) {
            progress('正在提交 ' + action + '请求…');
            try {
              const result = await request(target);
              acceptedId = result.transport_switch?.id;
            } catch (error) {
              if (error.status) throw error;
              replyLost = true;
            }
            sent = true;
          } else if (replyLost && !ours && ++unchanged >= 2) {
            throw new Error('未能确认切换请求，请保持手机连接设备热点后重试');
          }
        }
        progress(sent ? '正在确认 ' + action + '进度…' :
          pending ? '设备正在切换到 ' + label(job.target) + '…' : '正在选择联网通道…');
      }
      const delay = deadline - Date.now();
      if (delay <= 0) break;
      await new Promise(resolve => setTimeout(resolve, Math.min(600, delay)));
      const remaining = deadline - Date.now();
      if (remaining <= 0) break;
      try {
        net = await read(Math.min(3000, remaining));
        interrupted = false;
      } catch (error) {
        if (error.status === 401 || error.status === 403) throw error;
        net = null;
        interrupted = true;
        progress('连接暂时中断，正在查询切换进度…');
      }
    }
    throw new Error(interrupted ? '请重新连接设备热点，再查看切换结果'
                                : '等待切换结果超时，请刷新查看设备状态');
  }

  return {label, select};
})();
