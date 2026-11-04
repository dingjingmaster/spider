'use strict';
const $ = id => document.getElementById(id);
const state = {spider: null, run: null, active: false, results: [], workflow: null, trace: null, traceRequest: 0, traceSteps: [], token: sessionStorage.getItem('spider-token') || ''};
const statuses = {queued: '排队中', running: '运行中', succeeded: '已完成', failed: '失败', partial: '部分完成', waiting: '等待子分支', done: '已完成', skipped: '已跳过', cancelled: '已停止'};
const defaults = workflowDefaults();
function node(tag, text, cls) { const e = document.createElement(tag); if (text !== undefined) e.textContent = text; if (cls) e.className = cls; return e; }
function notify(text, error = false) { $('notice').textContent = text; $('notice').className = error ? 'error' : ''; $('notice').hidden = false; }
async function api(path, method = 'GET', body, binary = false) {
  const headers = {'X-Spider-Request': '1'};
  if (state.token) headers.Authorization = `Bearer ${state.token}`;
  if (body !== undefined) headers['Content-Type'] = 'application/json';
  const response = await fetch(path, {method, headers, body: body === undefined ? undefined : JSON.stringify(body)});
  if (!response.ok) {
    let text = `请求失败 (${response.status})`; try { text = (await response.json()).error || text; } catch (_) {}
    if (response.status === 401) { $('token-panel').hidden = false; $('connection').textContent = '需要访问口令'; }
    throw new Error(text);
  }
  return binary ? response.blob() : response.json();
}
async function action(button, fn) { button.disabled = true; try { await fn(); } catch (e) { notify(e.message, true); } finally { button.disabled = false; } }
function input(type, value) { const e = node('input'); e.type = type; if (type === 'checkbox') e.checked = !!value; else e.value = value ?? ''; return e; }
const workflowTypes = ['request', 'foreach', 'parallel', 'condition', 'download', 'emit', 'join'];
const clone = value => JSON.parse(JSON.stringify(value));
function workflowDefaults() {
  return {schema_version: 2, name: '新建爬虫', entry: 'request', vars: {}, defaults: {timeout_seconds: 30, retries: 2, delay_ms: 300, max_bytes: 4194304}, max_steps: 1000, interval_seconds: 0, enabled: true, steps: [
    {id: 'request', type: 'request', request: {url: 'https://example.com/', method: 'GET'}, extract: {format: 'html', row_selector: '', next_selector: '', fields: [{name: 'title', selector: 'h1', type: 'string', required: true}]}, next: 'each'},
    {id: 'each', type: 'foreach', items: '{{steps.request.items}}', body: 'emit'},
    {id: 'emit', type: 'emit', fields: {title: '{{input.title}}'}}
  ]};
}
function workflowExample() {
  return {schema_version: 2, name: '列表与详情示例', entry: 'list', vars: {}, defaults: {timeout_seconds: 30, retries: 2, delay_ms: 300, max_bytes: 4194304}, max_steps: 1000, interval_seconds: 0, enabled: true, steps: [
    {id: 'list', type: 'request', request: {url: 'https://example.com/list', method: 'GET'}, extract: {format: 'html', row_selector: 'a.detail', fields: [{name: 'url', selector: '', attribute: 'href', type: 'string', required: true}]}, next: 'each'},
    {id: 'each', type: 'foreach', items: '{{steps.list.items}}', body: 'detail'},
    {id: 'detail', type: 'request', request: {url: '{{input.url}}', method: 'GET'}, extract: {format: 'html', row_selector: '', fields: [{name: 'title', selector: 'h1', type: 'string', required: true}]}, next: 'save'},
    {id: 'save', type: 'emit', fields: {url: '{{input.url}}', title: '{{steps.detail.items.0.title}}'}}
  ]};
}
function stepConfiguration(type) {
  return clone({request: {request: {url: 'https://example.com/', method: 'GET'}, extract: {format: 'html', fields: [{name: 'title', selector: 'h1', type: 'string'}]}}, foreach: {items: '{{input.items}}', body: ''}, parallel: {branches: {first: '', second: ''}}, condition: {test: {left: '{{input.id}}', op: 'exists'}, then: '', else: ''}, download: {url: '{{input.url}}'}, emit: {fields: {title: '{{input.title}}'}}, join: {fields: {value: '{{input}}'}}}[type] || {});
}
function jsonObject(text, label) {
  let value; try { value = JSON.parse(text || '{}'); } catch (_) { throw new Error(`${label} 必须是有效 JSON`); }
  if (!value || typeof value !== 'object' || Array.isArray(value)) throw new Error(`${label} 必须是 JSON 对象`);
  return value;
}
function validateWorkflow(rule) {
  if (!rule || rule.schema_version !== 2 || !Array.isArray(rule.steps)) throw new Error('工作流需要 schema_version: 2 和 steps 数组');
  const ids = new Set();
  for (const step of rule.steps) {
    if (!step || !/^[A-Za-z_][A-Za-z0-9_-]{0,63}$/.test(step.id || '')) throw new Error('步骤 ID 必须以字母或下划线开头，最长 64 字符');
    if (ids.has(step.id)) throw new Error(`步骤 ID 重复：${step.id}`);
    if (!workflowTypes.includes(step.type)) throw new Error(`未知步骤类型：${step.type}`);
    ids.add(step.id);
  }
  if (!ids.has(rule.entry)) throw new Error('入口必须指向已有步骤 ID');
  for (const step of rule.steps) {
    const refs = [step.next, ...(step.type === 'foreach' ? [step.body] : []), ...(step.type === 'condition' ? [step.then, step.else] : []), ...(step.type === 'parallel' ? Object.values(step.branches || {}) : [])];
    for (const ref of refs) if (ref && !ids.has(ref)) throw new Error(`步骤 ${step.id} 引用了不存在的步骤：${ref}`);
  }
  return rule;
}
function syncWorkflowJSON() { $('workflow-json').value = JSON.stringify(state.workflow, null, 2); }
function refreshPreviewSteps() {
  const selected = $('preview-step').value; $('preview-step').replaceChildren();
  for (const step of state.workflow.steps) if (step.type === 'request') { const opt = node('option', step.id); opt.value = step.id; $('preview-step').append(opt); }
  if ([...$('preview-step').options].some(opt => opt.value === selected)) $('preview-step').value = selected;
}
function collectRequestExtraction(card, step) {
  if (!card.querySelector('[data-request-extract-enabled]').checked) { delete step.extract; return; }
  let fields; try { fields = JSON.parse(card.querySelector('[data-request-fields]').value); } catch (_) { throw new Error('提取字段必须是有效 JSON 数组'); }
  if (!Array.isArray(fields)) throw new Error('提取字段必须是 JSON 数组');
  step.extract = {...step.extract, fields, format: card.querySelector('[data-request-format]').value, row_selector: card.querySelector('[data-request-row]').value.trim(), next_selector: card.querySelector('[data-request-next]').value.trim()};
}
function requestEditor(card, config) {
  const form = node('div', undefined, 'request-form'); const grid = node('div', undefined, 'form-grid'); const controls = {};
  const enableLabel = node('label', undefined, 'inline'); const enabled = input('checkbox', false); enabled.dataset.requestExtractEnabled = ''; enableLabel.append(enabled, node('span', '启用字段提取（登录或纯请求步骤可关闭）')); form.append(enableLabel);
  for (const [key, labelText, options] of [['url', '请求地址 / URL 模板'], ['method', '请求方式', ['GET', 'POST']], ['format', '响应格式', ['html', 'json']], ['row', '列表选择器 / JSON Pointer'], ['next', '下一页选择器 / JSON Pointer']]) {
    const label = node('label', labelText); const control = options ? node('select') : input('text', '');
    if (options) for (const value of options) { const option = node('option', value); option.value = value; control.append(option); }
    control.dataset[`request${key[0].toUpperCase()}${key.slice(1)}`] = ''; controls[key] = control; label.append(control); grid.append(label);
  }
  form.append(grid); const fieldLabel = node('label', '提取字段（JSON 数组：name / selector / attribute / type / required）'); const fields = node('textarea'); fields.rows = 5; fields.spellcheck = false; fields.dataset.requestFields = ''; fieldLabel.append(fields); form.append(fieldLabel); card.append(form);
  const updateDisabled = () => { for (const control of [controls.format, controls.row, controls.next, fields]) control.disabled = !enabled.checked; };
  const refresh = () => {
    const data = jsonObject(config.value, '步骤配置'); const request = data.request || {}; const extract = data.extract || {};
    enabled.checked = Object.hasOwn(data, 'extract');
    controls.url.value = request.url || ''; controls.method.value = request.method || 'GET'; controls.format.value = extract.format || 'html'; controls.row.value = extract.row_selector || ''; controls.next.value = extract.next_selector || '';
    fields.value = JSON.stringify(enabled.checked ? extract.fields || [] : [{name: 'title', selector: 'h1', type: 'string'}], null, 2); updateDisabled();
  };
  for (const key of ['url', 'method']) controls[key].onchange = () => {
    try { const data = jsonObject(config.value, '步骤配置'); data.request = {...data.request, [key]: controls[key].value.trim()}; config.value = JSON.stringify(data, null, 2); captureWorkflow(); } catch (e) { notify(e.message, true); }
  };
  const updateExtraction = () => {
    updateDisabled();
    try { const data = jsonObject(config.value, '步骤配置'); collectRequestExtraction(card, data); config.value = JSON.stringify(data, null, 2); captureWorkflow(); } catch (e) { notify(e.message, true); }
  };
  for (const control of [enabled, controls.format, controls.row, controls.next, fields]) control.onchange = updateExtraction;
  refresh(); return refresh;
}
function renderWorkflow() {
  const rule = state.workflow;
  $('wf-name').value = rule.name || ''; $('wf-entry').value = rule.entry || ''; $('wf-max-steps').value = rule.max_steps ?? 1000; $('wf-interval').value = rule.interval_seconds || 0; $('wf-enabled').checked = rule.enabled !== false;
  $('wf-vars').value = JSON.stringify(rule.vars || {}, null, 2); $('wf-defaults').value = JSON.stringify(rule.defaults || {}, null, 2);
  $('workflow-steps').replaceChildren();
  rule.steps.forEach((step, index) => {
    const card = node('div', undefined, 'workflow-step'); const controls = node('div', undefined, 'step-controls');
    const idLabel = node('label', '步骤 ID'); const id = input('text', step.id); id.dataset.stepId = ''; idLabel.append(id);
    const typeLabel = node('label', '类型'); const type = node('select'); type.dataset.stepType = ''; for (const value of workflowTypes) { const opt = node('option', value); opt.value = value; type.append(opt); } type.value = step.type; typeLabel.append(type);
    const nextLabel = node('label', 'next（留空结束分支）'); const next = input('text', step.next); next.dataset.stepNext = ''; nextLabel.append(next);
    const remove = node('button', '删除步骤'); remove.onclick = () => { try { collectWorkflow(false); state.workflow.steps.splice(index, 1); renderWorkflow(); } catch (e) { notify(e.message, true); } };
    controls.append(idLabel, typeLabel, nextLabel, remove); card.append(controls);
    const label = node('label', '步骤配置（JSON；可包含重试、on_error 等属性）'); const config = node('textarea'); config.rows = 8; config.spellcheck = false; config.dataset.stepConfig = '';
    const {id: ignoredId, type: ignoredType, next: ignoredNext, ...rest} = step; config.value = JSON.stringify(rest, null, 2); label.append(config);
    const refreshRequest = step.type === 'request' ? requestEditor(card, config) : null;
    if (step.type === 'request') { const advanced = node('details'); advanced.append(node('summary', '完整步骤 JSON（请求头、请求体、query / json、重试等）'), label); card.append(advanced); }
    else card.append(label);
    type.onchange = () => { config.value = JSON.stringify(stepConfiguration(type.value), null, 2); try { collectWorkflow(false); renderWorkflow(); } catch (e) { notify(e.message, true); } };
    for (const el of [id, next]) el.onchange = captureWorkflow;
    config.onchange = () => { try { if (refreshRequest) refreshRequest(); collectWorkflow(false); } catch (e) { notify(e.message, true); } };
    $('workflow-steps').append(card);
  });
  syncWorkflowJSON(); refreshPreviewSteps();
}
function collectWorkflow(validate = true) {
  const rule = clone(state.workflow);
  rule.name = $('wf-name').value.trim(); rule.entry = $('wf-entry').value.trim(); rule.max_steps = Number($('wf-max-steps').value); rule.interval_seconds = Number($('wf-interval').value); rule.enabled = $('wf-enabled').checked;
  rule.vars = jsonObject($('wf-vars').value, 'vars'); rule.defaults = jsonObject($('wf-defaults').value, 'defaults');
  rule.steps = [...$('workflow-steps').children].map(card => {
    const step = jsonObject(card.querySelector('[data-step-config]').value, '步骤配置');
    step.id = card.querySelector('[data-step-id]').value.trim(); step.type = card.querySelector('[data-step-type]').value;
    const next = card.querySelector('[data-step-next]').value.trim(); if (next) step.next = next; else delete step.next;
    const fields = card.querySelector('[data-request-fields]');
    if (step.type === 'request' && fields) {
      step.request = {...step.request, url: card.querySelector('[data-request-url]').value.trim(), method: card.querySelector('[data-request-method]').value};
      collectRequestExtraction(card, step);
    }
    return step;
  });
  if (validate) validateWorkflow(rule); state.workflow = rule; syncWorkflowJSON(); refreshPreviewSteps(); return rule;
}
function captureWorkflow() { try { collectWorkflow(false); } catch (e) { notify(e.message, true); } }
function showRule(rule, id = null, version = null) {
  validateWorkflow(rule); state.spider = id; state.workflow = clone(rule); renderWorkflow();
  $('editor-title').textContent = rule.name || '新建工作流'; $('version').textContent = version ? `版本 ${version}` : '未保存'; $('delete').hidden = !id;
  $('preview-card').hidden = true; listSpiders().catch(() => {});
}
function readRule() { return collectWorkflow(); }
async function listSpiders() {
  const rows = await api('/api/spiders'); $('spiders').replaceChildren();
  if (!rows.length) $('spiders').append(node('p', '点击 ＋ 创建第一个爬虫', 'empty'));
  for (const r of rows) { const b = node('button', r.name, 'spider-button' + (r.id === state.spider ? ' active' : '')); b.append(node('small', r.interval_seconds && r.enabled ? `每 ${r.interval_seconds} 秒 · v${r.version}` : `手动执行 · v${r.version}`)); b.onclick = () => action(b, async () => { const full = await api(`/api/spiders/${r.id}`); showRule(full.rule, full.id, full.version); state.run = null; state.active = false; $('run-detail').hidden = true; await listRuns(); }); $('spiders').append(b); }
}
async function save() {
  const r = readRule(); const created = await api(state.spider ? `/api/spiders/${state.spider}` : '/api/spiders', state.spider ? 'PUT' : 'POST', r);
  const full = await api(`/api/spiders/${created.id}`); showRule(full.rule, full.id, full.version); notify('规则已保存'); await listRuns(); return created.id;
}
function renderTable(container, items) {
  container.replaceChildren(); if (!items.length) { container.append(node('div', '暂无采集结果', 'empty')); return; }
  const keys = [...new Set(items.flatMap(item => Object.keys(item)))]; const table = node('table'); const head = node('thead'); const hr = node('tr'); keys.forEach(k => hr.append(node('th', k))); head.append(hr); table.append(head); const body = node('tbody');
  for (const item of items) { const tr = node('tr'); keys.forEach(k => tr.append(node('td', item[k] === null || item[k] === undefined ? '—' : typeof item[k] === 'object' ? JSON.stringify(item[k]) : String(item[k])))); body.append(tr); }
  table.append(body); container.append(table);
}
async function listRuns() {
  const rows = await api('/api/runs' + (state.spider ? `?spider_id=${state.spider}` : '')); $('runs').replaceChildren();
  if (!rows.length) { const tr = node('tr'); const td = node('td', '暂无运行记录', 'empty'); td.colSpan = 6; tr.append(td); $('runs').append(tr); }
  for (const r of rows) { const tr = node('tr', undefined, 'run-row' + (r.id === state.run ? ' selected' : '')); tr.append(node('td', `#${r.id}`)); const td = node('td'); td.append(node('span', statuses[r.status] || r.status, `status ${r.status}`)); tr.append(td); tr.append(node('td', r.pages), node('td', r.item_count), node('td', new Date(r.created_at).toLocaleString()), node('td', `v${r.version}`)); tr.onclick = () => selectRun(r.id).catch(e => notify(e.message, true)); $('runs').append(tr); }
}
async function selectRun(id, append = false) {
  if (state.run !== id) { state.trace = null; clearStepTrace(); }
  state.run = id; const after = append && state.results.length ? state.results.at(-1).id : 0;
  const [run, items, events, artifacts, steps] = await Promise.all([api(`/api/runs/${id}`), api(`/api/runs/${id}/items?after=${after}`), api(`/api/runs/${id}/events`), api(`/api/runs/${id}/artifacts`), api(`/api/runs/${id}/steps`)]);
  if (state.run !== id) return;
  state.active = ['queued', 'running'].includes(run.status); state.results = append ? state.results.concat(items) : items;
  $('run-detail').hidden = false; $('run-title').textContent = `运行 #${id} · ${statuses[run.status] || run.status} · ${run.item_count} 条结果`;
  $('run-error').textContent = run.error || ''; $('stop').hidden = !state.active;
  renderTable($('result-items'), state.results.map(r => r.data)); $('more-results').hidden = items.length < 1000;
  $('events').textContent = events.slice().reverse().map(e => `${new Date(e.created_at).toLocaleTimeString()} [${e.level}] ${e.message}`).join('\n') || '等待执行…';
  renderSteps(steps);
  $('artifacts').replaceChildren(); for (const file of artifacts) { const b = node('button', `下载文件 #${file.id} · ${(file.bytes / 1024).toFixed(1)} KiB`); b.onclick = () => action(b, () => download(`/api/artifacts/${file.id}/file`, `file-${file.id}.bin`)); $('artifacts').append(b); }
}
function clearStepTrace() {
  state.traceRequest++; $('trace-detail').hidden = true;
  for (const id of ['trace-context', 'trace-output', 'trace-error']) $(id).textContent = '';
  $('trace-title').textContent = '选择步骤查看上下文与输出。';
}
function renderSteps(steps) {
  state.traceSteps = steps; $('step-traces').hidden = !steps.length; $('step-rows').replaceChildren();
  if (!steps.some(step => step.id === state.trace)) { state.trace = null; clearStepTrace(); }
  for (const step of steps) {
    const tr = node('tr', undefined, 'step-row' + (step.id === state.trace ? ' selected' : '')); const first = node('td'); const select = node('button', `#${step.id}`, 'subtle'); select.onclick = () => selectStep(step.id); first.append(select); tr.append(first);
    for (const value of [step.step_id, step.parent_id ?? '—', `${step.branch_key || '—'} / ${step.record_key || '—'}`, statuses[step.status] || step.status, step.attempts, step.phase ? '汇合' : '执行']) tr.append(node('td', String(value)));
    $('step-rows').append(tr);
  }
}
async function selectStep(id) {
  const run = state.run; state.trace = id; renderSteps(state.traceSteps); clearStepTrace();
  const request = state.traceRequest; $('trace-title').textContent = `正在加载实例 #${id}…`;
  try {
    const step = await api(`/api/runs/${run}/steps/${id}`);
    if (state.run !== run || state.trace !== id || state.traceRequest !== request) return;
    $('trace-title').textContent = `实例 #${step.id} · ${step.step_id}`;
    $('trace-context').textContent = JSON.stringify(step.context, null, 2); $('trace-output').textContent = JSON.stringify(step.output, null, 2); $('trace-error').textContent = step.error || '无'; $('trace-detail').hidden = false;
  } catch (e) {
    if (state.run !== run || state.trace !== id || state.traceRequest !== request) return;
    $('trace-title').textContent = `实例 #${id} 加载失败：${e.message}`; notify(e.message, true);
  }
}
async function download(path, name) { const blob = await api(path, 'GET', undefined, true); saveBlob(blob, name); }
function saveBlob(blob, name) { const url = URL.createObjectURL(blob); const a = node('a'); a.href = url; a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); }
$('new').onclick = () => { showRule(defaults); state.run = null; state.active = false; $('run-detail').hidden = true; listRuns().catch(e => notify(e.message, true)); };
$('save').onclick = () => action($('save'), save);
$('workflow-example').onclick = () => { state.workflow = workflowExample(); renderWorkflow(); notify('示例地址为 example.com；请按目标站点修改 URL 和选择器。'); };
$('add-step').onclick = () => { try { collectWorkflow(false); let count = state.workflow.steps.length + 1; while (state.workflow.steps.some(step => step.id === `step_${count}`)) count++; state.workflow.steps.push({id: `step_${count}`, type: 'request', ...stepConfiguration('request')}); renderWorkflow(); } catch (e) { notify(e.message, true); } };
$('apply-workflow-json').onclick = () => { try { const rule = JSON.parse($('workflow-json').value); validateWorkflow(rule); state.workflow = clone(rule); renderWorkflow(); notify('工作流 JSON 已应用，保存后生效'); } catch (e) { notify(e.message, true); } };
for (const id of ['wf-name', 'wf-entry', 'wf-max-steps', 'wf-interval', 'wf-enabled', 'wf-vars', 'wf-defaults']) $(id).onchange = captureWorkflow;
$('preview').onclick = () => action($('preview'), async () => {
  $('preview-card').hidden = true; $('preview-output').textContent = ''; $('raw-response').textContent = '';
  const rule = readRule(); if (!$('preview-step').value) throw new Error('请添加并选择一个 request 步骤');
  const payload = {rule, step_id: $('preview-step').value, context: jsonObject($('preview-context').value, '试采集上下文')};
  const out = await api('/api/preview', 'POST', payload); const items = out.items || []; const response = out.response || {};
  renderTable($('preview-items'), items); $('preview-meta').textContent = `HTTP ${response.status ?? '—'} · ${items.length} 行${response.bytes === undefined ? '' : ` · ${response.bytes} 字节`}`;
  $('raw-response').textContent = (response.body || '') + (response.truncated ? '\n… 响应预览已截断' : ''); $('preview-trace').hidden = false; $('preview-output').textContent = JSON.stringify(out, null, 2); $('preview-card').hidden = false; notify('试采集完成');
});
$('run').onclick = () => action($('run'), async () => { const id = await save(); const run = await api(`/api/spiders/${id}/run`, 'POST', {}); await selectRun(run.id); await listRuns(); notify(`运行 #${run.id} 已加入队列`); });
$('stop').onclick = () => action($('stop'), async () => { if (!state.run) return; await api(`/api/runs/${state.run}/stop`, 'POST', {}); await selectRun(state.run); notify('已发送停止请求'); });
$('delete').onclick = () => action($('delete'), async () => { if (!confirm('删除此爬虫？历史运行和结果仍会保留。')) return; await api(`/api/spiders/${state.spider}`, 'DELETE'); showRule(defaults); await listRuns(); notify('爬虫已删除'); });
$('refresh').onclick = () => action($('refresh'), async () => { await listRuns(); if (state.run) await selectRun(state.run); });
$('more-results').onclick = () => action($('more-results'), () => selectRun(state.run, true));
for (const format of ['json', 'csv']) $(`export-${format}`).onclick = () => action($(`export-${format}`), () => download(`/api/runs/${state.run}/export?format=${format}`, `spider-run-${state.run}.${format}`));
$('export-rule').onclick = () => { try { saveBlob(new Blob([JSON.stringify(readRule(), null, 2)], {type: 'application/json'}), 'spider-rule.json'); } catch (e) { notify(e.message, true); } };
$('import').onclick = () => $('import-file').click();
$('import-file').onchange = async () => { try { const file = $('import-file').files[0]; if (!file) return; if (file.size > 1048576) throw new Error('规则文件不能超过 1 MiB'); const rule = JSON.parse(await file.text()); validateWorkflow(rule); showRule(rule); notify('已导入规则，保存后生效'); } catch (e) { notify(e.message, true); } finally { $('import-file').value = ''; } };
$('token-toggle').onclick = () => { $('token-panel').hidden = !$('token-panel').hidden; };
$('token-save').onclick = () => { state.token = $('token').value; sessionStorage.setItem('spider-token', state.token); connect(); };
async function connect() { try { await api('/api/health'); $('connection').textContent = '服务已连接'; $('token-panel').hidden = true; await listSpiders(); await listRuns(); } catch (e) { $('connection').textContent = '连接失败'; notify(e.message, true); } }
$('token').value = state.token; showRule(defaults); connect();
let polling = false;
setInterval(async () => { if (polling || document.hidden) return; polling = true; try { await listRuns(); if (state.active && state.run) await selectRun(state.run); } catch (_) {} finally { polling = false; } }, 1500);
