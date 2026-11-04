'use strict';
const $ = id => document.getElementById(id);
const state = {spider: null, run: null, active: false, results: [], token: sessionStorage.getItem('spider-token') || ''};
const statuses = {queued: '排队中', running: '运行中', succeeded: '已完成', failed: '失败', cancelled: '已停止'};
const defaults = {name: '', url: '', method: 'GET', format: 'html', row_selector: '', next_selector: '', max_pages: 1, interval_seconds: 0, enabled: true, delay_ms: 300, retries: 2, timeout_seconds: 30, max_bytes: 4194304, headers: {}, body: '', fields: [{name: 'title', selector: 'h1', attribute: '', type: 'string', required: true, download: false}]};
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
function addField(field = {}) {
  const tr = node('tr');
  for (const key of ['name', 'selector', 'attribute']) { const td = node('td'); const el = input('text', field[key]); el.dataset.key = key; td.append(el); tr.append(td); }
  const td = node('td'); const sel = node('select'); sel.dataset.key = 'type';
  for (const [value, label] of [['string', '文本'], ['number', '数值'], ['integer', '整数'], ['boolean', '布尔']]) { const opt = node('option', label); opt.value = value; sel.append(opt); }
  sel.value = field.type || 'string'; td.append(sel); tr.append(td);
  for (const key of ['required', 'download']) { const td = node('td'); const el = input('checkbox', field[key]); el.dataset.key = key; el.setAttribute('aria-label', key); td.append(el); tr.append(td); }
  const last = node('td'); const remove = node('button', '×', 'remove-field'); remove.title = '删除字段'; remove.onclick = () => tr.remove(); last.append(remove); tr.append(last); $('fields').append(tr);
}
const mapping = {name:'name', url:'url', method:'method', format:'format', row_selector:'row-selector', next_selector:'next-selector', max_pages:'max-pages', interval_seconds:'interval', delay_ms:'delay', retries:'retries', timeout_seconds:'timeout'};
function showRule(rule, id = null, version = null) {
  state.spider = id; const r = {...defaults, ...rule};
  for (const [key, element] of Object.entries(mapping)) $(element).value = r[key];
  $('enabled').checked = r.enabled; $('max-size').value = r.max_bytes / 1048576;
  $('headers').value = JSON.stringify(r.headers, null, 2); $('body').value = r.body;
  $('fields').replaceChildren(); for (const field of r.fields) addField(field);
  $('editor-title').textContent = id ? r.name : '新建爬虫'; $('version').textContent = version ? `版本 ${version}` : '未保存'; $('delete').hidden = !id;
  $('preview-card').hidden = true; updateFormat(); listSpiders().catch(() => {});
}
function readRule() {
  const rule = {}; for (const [key, element] of Object.entries(mapping)) rule[key] = ['max_pages','interval_seconds','delay_ms','retries','timeout_seconds'].includes(key) ? Number($(element).value) : $(element).value.trim();
  rule.enabled = $('enabled').checked; rule.max_bytes = Math.round(Number($('max-size').value) * 1048576);
  try { rule.headers = JSON.parse($('headers').value || '{}'); } catch (_) { throw new Error('请求头必须是有效的 JSON 对象'); }
  rule.body = $('body').value;
  rule.fields = [...$('fields').children].map(tr => { const f = {}; tr.querySelectorAll('[data-key]').forEach(el => { f[el.dataset.key] = el.type === 'checkbox' ? el.checked : el.value.trim(); }); return f; });
  return rule;
}
function updateFormat() { const json = $('format').value === 'json'; $('field-hint').textContent = json ? '使用 JSON Pointer 路径，例如 /name、/items/0/title；列表路径指向数组时逐条提取。' : '选择器相对于列表中的每一行；属性留空时提取文本。'; $('row-selector').placeholder = json ? '例如 /items，留空为整个 JSON' : '例如 .product，留空为整页'; $('next-selector').placeholder = json ? '例如 /next_url' : '例如 a.next，留空不翻页'; }
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
  state.run = id; const after = append && state.results.length ? state.results.at(-1).id : 0;
  const [run, items, events, artifacts] = await Promise.all([api(`/api/runs/${id}`), api(`/api/runs/${id}/items?after=${after}`), api(`/api/runs/${id}/events`), api(`/api/runs/${id}/artifacts`)]);
  if (state.run !== id) return;
  state.active = ['queued', 'running'].includes(run.status); state.results = append ? state.results.concat(items) : items;
  $('run-detail').hidden = false; $('run-title').textContent = `运行 #${id} · ${statuses[run.status] || run.status} · ${run.item_count} 条结果`;
  $('run-error').textContent = run.error || ''; $('stop').hidden = !state.active;
  renderTable($('result-items'), state.results.map(r => r.data)); $('more-results').hidden = items.length < 1000;
  $('events').textContent = events.slice().reverse().map(e => `${new Date(e.created_at).toLocaleTimeString()} [${e.level}] ${e.message}`).join('\n') || '等待执行…';
  $('artifacts').replaceChildren(); for (const file of artifacts) { const b = node('button', `下载文件 #${file.id} · ${(file.bytes / 1024).toFixed(1)} KiB`); b.onclick = () => action(b, () => download(`/api/artifacts/${file.id}/file`, `file-${file.id}.bin`)); $('artifacts').append(b); }
}
async function download(path, name) { const blob = await api(path, 'GET', undefined, true); saveBlob(blob, name); }
function saveBlob(blob, name) { const url = URL.createObjectURL(blob); const a = node('a'); a.href = url; a.download = name; a.click(); setTimeout(() => URL.revokeObjectURL(url), 1000); }
$('new').onclick = () => { showRule(defaults); state.run = null; state.active = false; $('run-detail').hidden = true; listRuns().catch(e => notify(e.message, true)); };
$('format').onchange = updateFormat;
$('add-field').onclick = () => addField();
$('save').onclick = () => action($('save'), save);
$('preview').onclick = () => action($('preview'), async () => { const out = await api('/api/preview', 'POST', readRule()); renderTable($('preview-items'), out.items); $('preview-meta').textContent = `HTTP ${out.response.status} · ${out.items.length} 行 · ${out.response.bytes} 字节`; $('raw-response').textContent = out.response.body + (out.response.truncated ? '\n… 响应预览已截断' : ''); $('preview-card').hidden = false; notify('试采集完成'); });
$('run').onclick = () => action($('run'), async () => { const id = await save(); const run = await api(`/api/spiders/${id}/run`, 'POST', {}); await selectRun(run.id); await listRuns(); notify(`运行 #${run.id} 已加入队列`); });
$('stop').onclick = () => action($('stop'), async () => { if (!state.run) return; await api(`/api/runs/${state.run}/stop`, 'POST', {}); await selectRun(state.run); notify('已发送停止请求'); });
$('delete').onclick = () => action($('delete'), async () => { if (!confirm('删除此爬虫？历史运行和结果仍会保留。')) return; await api(`/api/spiders/${state.spider}`, 'DELETE'); showRule(defaults); await listRuns(); notify('爬虫已删除'); });
$('refresh').onclick = () => action($('refresh'), async () => { await listRuns(); if (state.run) await selectRun(state.run); });
$('more-results').onclick = () => action($('more-results'), () => selectRun(state.run, true));
for (const format of ['json', 'csv']) $(`export-${format}`).onclick = () => action($(`export-${format}`), () => download(`/api/runs/${state.run}/export?format=${format}`, `spider-run-${state.run}.${format}`));
$('export-rule').onclick = () => { try { saveBlob(new Blob([JSON.stringify(readRule(), null, 2)], {type: 'application/json'}), 'spider-rule.json'); } catch (e) { notify(e.message, true); } };
$('import').onclick = () => $('import-file').click();
$('import-file').onchange = async () => { try { const file = $('import-file').files[0]; if (!file) return; if (file.size > 1048576) throw new Error('规则文件不能超过 1 MiB'); const rule = JSON.parse(await file.text()); if (!Array.isArray(rule.fields)) throw new Error('规则缺少 fields 数组'); showRule(rule); notify('已导入规则，保存后生效'); } catch (e) { notify(e.message, true); } finally { $('import-file').value = ''; } };
$('token-toggle').onclick = () => { $('token-panel').hidden = !$('token-panel').hidden; };
$('token-save').onclick = () => { state.token = $('token').value; sessionStorage.setItem('spider-token', state.token); connect(); };
async function connect() { try { await api('/api/health'); $('connection').textContent = '服务已连接'; $('token-panel').hidden = true; await listSpiders(); await listRuns(); } catch (e) { $('connection').textContent = '连接失败'; notify(e.message, true); } }
$('token').value = state.token; showRule(defaults); connect();
let polling = false;
setInterval(async () => { if (polling || document.hidden) return; polling = true; try { await listRuns(); if (state.active && state.run) await selectRun(state.run); } catch (_) {} finally { polling = false; } }, 1500);
