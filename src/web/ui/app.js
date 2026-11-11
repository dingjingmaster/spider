'use strict';
const $ = id => document.getElementById(id);
const state = {spiders: [], runs: [], spider: null, run: null, refreshSequence: 0, refreshAttempt: 0, historySequence: 0, runSequence: 0, followLatest: true};
const statuses = {idle: '未执行', queued: '排队中', running: '运行中', succeeded: '成功', failed: '失败', partial: '部分成功', cancelled: '已取消'};
function node(tag, text, cls) { const value = document.createElement(tag); if (text !== undefined) value.textContent = text; if (cls) value.className = cls; return value; }
function time(value) { if (!value) return '—'; const date = new Date(value); return Number.isNaN(date.getTime()) ? '—' : date.toLocaleString(); }
function duration(run) { if (!run.started_at) return '—'; const end = run.finished_at || Date.now(); return `${Math.max(0, (end - run.started_at) / 1000).toFixed(1)} 秒`; }
function statusText(row) { return row.execution_active && !['running', 'queued'].includes(row.status) ? '正在退出' : statuses[row.status] || row.status || '未执行'; }
function badge(row) { return node('span', statusText(row), `status ${Object.hasOwn(statuses, row.status) ? row.status : 'idle'}`); }
function notify(error) { $('notice').textContent = error.message || String(error); $('notice').hidden = false; }
async function api(path) {
  const attempt = state.refreshAttempt;
  const response = await fetch(path, {method: 'GET'});
  if (!response.ok) {
    let message = `查询失败 (${response.status})`;
    try { message = (await response.json()).error || message; } catch (_) {}
    throw new Error(message);
  }
  return response.json();
}
async function post(path) {
  const response = await fetch(path, {method: 'POST', headers: {'X-Spider-Request': '1'}});
  if (!response.ok) {
    let message = `操作失败 (${response.status})`;
    try { message = (await response.json()).error || message; } catch (_) {}
    throw new Error(message);
  }
  return response.json();
}
async function runSpider(row) {
  if (!confirm(`立即运行「${row.name}」？`)) return;
  try {
    const result = await post(`/api/spiders/${row.id}/run`);
    $('notice').textContent = `已提交运行 #${result.id}`;
    $('notice').hidden = false;
    state.spider = row.id;
    state.followLatest = true;
    state.refreshSequence++;
    await refreshDashboard();
  } catch (error) { notify(error); }
}
function renderSpiders() {
  $('spiders').replaceChildren(); $('empty-state').hidden = state.spiders.length > 0;
  $('total-count').textContent = String(state.spiders.length);
  $('running-count').textContent = String(state.spiders.filter(row => row.status === 'running' || row.execution_active).length);
  $('success-count').textContent = String(state.spiders.reduce((sum, row) => sum + (row.successes || 0), 0));
  $('failure-count').textContent = String(state.spiders.reduce((sum, row) => sum + (row.failures || 0), 0));
  for (const row of state.spiders) {
    const tr = node('tr', undefined, `selectable${row.id === state.spider ? ' selected' : ''}`);
    const name = node('td'); const choose = node('button', row.name, 'link-button');
    choose.onclick = () => selectSpider(row.id).catch(notify); name.append(choose, node('small', row.code_id, 'code-id'));
    const plan = !row.enabled ? '未启用' : row.interval_seconds > 0 ? `每 ${row.interval_seconds} 秒` : '未设置定时';
    const status = node('td'); status.append(badge(row));
    const action = node('td');
    const running = row.status === 'running' || row.execution_active;
    const run = node('button', running ? '运行中' : '运行', 'run-button');
    run.disabled = running;
    run.onclick = () => runSpider(row);
    action.append(run);
    tr.append(name, node('td', plan), status, node('td', `${row.successes || 0} / ${row.failures || 0}`), node('td', time(row.started_at)), node('td', row.enabled && row.interval_seconds > 0 ? time(row.next_run) : '—'), action);
    $('spiders').append(tr);
  }
}
function renderHistory() {
  $('runs').replaceChildren(); $('history-card').hidden = state.spider === null;
  const spider = state.spiders.find(row => row.id === state.spider);
  $('history-title').textContent = spider ? `${spider.name} · 运行历史` : '运行历史';
  if (!state.runs.length) { const tr = node('tr'); const cell = node('td', '尚无执行记录', 'empty'); cell.colSpan = 6; tr.append(cell); $('runs').append(tr); }
  for (const row of state.runs) {
    const tr = node('tr', undefined, `selectable${row.id === state.run ? ' selected' : ''}`);
    const id = node('td'); const choose = node('button', `#${row.id}`, 'link-button'); choose.onclick = () => { state.followLatest = false; selectRun(row.id).catch(notify); }; id.append(choose);
    const status = node('td'); status.append(badge(row));
    tr.append(id, status, node('td', time(row.started_at)), node('td', time(row.finished_at)), node('td', duration(row)), node('td', String(row.item_count || 0)));
    $('runs').append(tr);
  }
}
function renderRun(run, events) {
  $('run-detail').hidden = false; $('run-title').textContent = `运行 #${run.id}`;
  $('run-state').textContent = statusText(run); $('run-state').className = `status ${Object.hasOwn(statuses, run.status) ? run.status : 'idle'}`;
  $('run-start').textContent = time(run.started_at); $('run-finish').textContent = time(run.finished_at);
  $('run-duration').textContent = duration(run); $('run-items').textContent = String(run.item_count || 0);
  $('run-error').textContent = run.error || '';
  const progress = run.progress;
  $('run-progress').textContent = run.execution_active && run.status === 'cancelled' ? '正在退出，等待当前操作结束。' : progress ? `${progress.phase || '执行中'} · 已传输 ${(progress.received / 1024).toFixed(1)} KiB${progress.total > 0 ? ` / ${(progress.total / 1024).toFixed(1)} KiB` : ''}` : '';
  $('events').textContent = events.slice(0, 10).reverse().map(event => `${time(event.created_at)} [${event.level}] ${event.message}`).join('\n') || '暂无日志';
}
async function selectRun(id) {
  const spider = state.spider; state.run = id; const sequence = ++state.runSequence; renderHistory();
  const [run, events] = await Promise.all([api(`/api/runs/${id}`), api(`/api/runs/${id}/events`)]);
  if (state.spider !== spider || state.run !== id || state.runSequence !== sequence) return;
  renderRun(run, events);
}
async function loadHistory() {
  const spider = state.spider; const sequence = ++state.historySequence;
  if (spider === null) { state.runs = []; state.run = null; state.runSequence++; $('history-card').hidden = true; $('run-detail').hidden = true; return; }
  const rows = await api(`/api/runs?spider_id=${spider}`);
  if (state.spider !== spider || state.historySequence !== sequence) return;
  state.runs = rows;
  if (state.followLatest && rows.length) state.run = rows[0].id;
  if (!rows.some(row => row.id === state.run)) state.run = rows[0]?.id ?? null;
  renderHistory();
  if (state.run !== null) await selectRun(state.run); else { state.runSequence++; $('run-detail').hidden = true; }
}
async function selectSpider(id) {
  state.spider = id; state.run = null; state.followLatest = true; state.runSequence++; $('run-detail').hidden = true; renderSpiders(); await loadHistory();
}
async function refreshDashboard() {
  const sequence = ++state.refreshSequence;
  const rows = await api('/api/spiders');
  if (state.refreshSequence !== sequence) return;
  state.spiders = rows;
  if (!rows.some(row => row.id === state.spider)) { state.spider = rows[0]?.id ?? null; state.run = null; state.runSequence++; }
  renderSpiders(); await loadHistory();
  if (state.refreshSequence !== sequence) return;
  $('connection').textContent = '服务已连接'; $('updated-at').textContent = `更新于 ${new Date().toLocaleTimeString()}`; $('notice').hidden = true;
}
async function refresh() {
  const attempt = ++state.refreshAttempt;
  try { await refreshDashboard(); }
  catch (error) {
    if (state.refreshAttempt !== attempt) return;
    $('connection').textContent = '连接失败';
    notify(error);
  }
}
$('refresh').onclick = async () => { $('refresh').disabled = true; try { await refresh(); } finally { $('refresh').disabled = false; } };
refresh();
let polling = false;
setInterval(async () => { if (polling || document.hidden) return; polling = true; try { await refresh(); } finally { polling = false; } }, 1500);
