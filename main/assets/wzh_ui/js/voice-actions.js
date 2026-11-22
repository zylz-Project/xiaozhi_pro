(function () {
  'use strict';

  function confirmAction(action, name) {
    const rename = action === 'rename';
    const dialog = document.createElement('dialog');
    dialog.className = 'wzh-voice-dialog';
    dialog.setAttribute('aria-labelledby', 'voiceActionTitle');
    dialog.innerHTML = `<form method="dialog">
      <h2 id="voiceActionTitle"></h2>
      <label><span>音色名称</span><input type="text" autocomplete="off" inputmode="none" data-edit-on-tap maxlength="48"></label>
      <p data-delete-note>删除后会从本机和云端移除这段音色。</p>
      <p data-name-error role="alert"></p>
      <div class="wzh-voice-dialog-actions">
        <button type="button" autofocus>取消</button><button type="submit"></button>
      </div>
    </form>`;
    dialog.querySelector('h2').textContent = rename ? '编辑音色名称' : `删除“${name}”？`;
    const input = dialog.querySelector('input');
    input.value = rename ? name : '';
    input.disabled = !rename;
    dialog.querySelector('label').hidden = !rename;
    dialog.querySelector('[data-delete-note]').hidden = rename;
    const submit = dialog.querySelector('[type="submit"]');
    submit.textContent = rename ? '保存' : '删除';
    submit.className = rename ? 'primary' : 'danger';
    dialog.querySelector('[type="button"]').onclick = () => dialog.close();
    let value = null;
    return new Promise(resolve => {
      dialog.addEventListener('close', () => { dialog.remove(); resolve(value); }, {once:true});
      dialog.querySelector('form').addEventListener('submit', event => {
        event.preventDefault();
        const text = input.value.trim();
        if (rename && (!text || new TextEncoder().encode(text).length > 48)) {
          dialog.querySelector('[data-name-error]').textContent = '请输入有效名称，最多 48 字节。';
          return;
        }
        value = rename ? text : '';
        dialog.close();
      });
      document.body.append(dialog);
      dialog.showModal();
    });
  }

  async function waitForResult(api, path, ticket) {
    const deadline = Date.now() + 45000;
    const messages = {network:'无法完成云端操作，请检查网络和语音服务。', invalid:'音色不存在或名称无效。',
      save:'音色操作后的本机记录保存失败，请刷新列表确认。', busy:'设备正忙，请稍后重试。', config:'音色服务未配置。'};
    while (Number.isInteger(ticket) && ticket > 0 && Date.now() < deadline) {
      let data;
      try { data = await api(path); }
      catch (error) { if (error.status && error.status < 500) throw error; }
      const result = data?.voice_operations?.find(entry => entry.request === ticket)?.status;
      if (result === 'ok') return;
      if (result && result !== 'busy') throw new Error(messages[result] || '音色操作失败，请刷新列表确认。');
      await new Promise(resolve => setTimeout(resolve, 800));
    }
    throw new Error('暂未确认操作结果，请刷新音色列表确认。');
  }

  window.WzhVoiceActions = {confirmAction, waitForResult};
})();
