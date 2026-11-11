'use strict';
const $ = id => document.getElementById(id);
const state = {type: 'music', page: 1, pageSize: 20, total: 0, pages: 0, token: sessionStorage.getItem('spider-token') || '', loading: false};
const columns = {
  music: [['artist', '歌手'], ['title', '曲名'], ['album', '专辑'], ['format', '格式'], ['bytes', '大小'], ['seconds', '时长'], ['status', '状态'], ['target_path', '文件'], ['item_title', '来源']],
  novel: [['author', '作者'], ['title', '书名'], ['series', '系列'], ['chapter_no', '章节'], ['word_count', '字数'], ['format', '格式'], ['status', '状态'], ['target_path', '文件'], ['item_title', '来源']]
};
const statusLabels = {pending: '待下载', downloading: '下载中', downloaded: '已下载', skipped: '已存在', failed: '失败', ignored: '已忽略'};
const statusClasses = {pending: 'queued', downloading: 'running', downloaded: 'succeeded', skipped: 'succeeded', failed: 'failed', ignored: 'cancelled'};
function node(tag, text, cls) { const value = document.createElement(tag); if (text !== undefined) value.textContent = text; if (cls) value.className = cls; return value; }
function text(value) { return value === undefined || value === null || value === '' ? '—' : String(value); }
function number(value) { return value === undefined || value === null || value === '' ? '—' : Number(value).toLocaleString(); }
function humanBytes(value) {
  const size = Number(value);
  if (!size) return '—';
  const units = ['B', 'KiB', 'MiB', 'GiB', 'TiB'];
  let index = 0, amount = size;
  while (amount >= 1024 && index < units.length - 1) { amount /= 1024; index++; }
  return `${amount.toFixed(index ? 1 : 0)} ${units[index]}`;
}
function humanSeconds(value) {
  const total = Math.round(Number(value));
  if (!total) return '—';
  const hours = Math.floor(total / 3600), minutes = Math.floor((total % 3600) / 60), seconds = total % 60;
  return hours ? `${hours}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}` : `${minutes}:${String(seconds).padStart(2, '0')}`;
}
function statusBadge(value) { return node('span', statusLabels[value] || text(value), `status ${statusClasses[value] || ''}`); }
function sourceLink(row) {
  const label = text(row.item_title || row.item_identifier);
  if (!row.item_url) return node('span', label);
  const link = node('a', label, 'data-link'); link.href = row.item_url; link.target = '_blank'; link.rel = 'noreferrer'; return link;
}
function cell(key, row) {
  const td = node('td');
  if (key === 'status') { td.append(statusBadge(row.status)); return td; }
  if (key === 'bytes') { td.textContent = humanBytes(row.bytes); return td; }
  if (key === 'seconds') { td.textContent = humanSeconds(row.seconds); return td; }
  if (key === 'word_count' || key === 'track_no' || key === 'chapter_no') { td.textContent = number(row[key]); return td; }
  if (key === 'target_path') { const span = node('span', row.target_path || '—', 'data-path'); span.title = row.target_path || ''; td.append(span); return td; }
  if (key === 'item_title') { td.append(sourceLink(row)); return td; }
  td.textContent = text(row[key]);
  return td;
}
function renderTabs() {
  $('type-music').className = state.type === 'music' ? 'active' : '';
  $('type-novel').className = state.type === 'novel' ? 'active' : '';
}
function renderHead() {
  const tr = node('tr');
  for (const [, label] of columns[state.type]) tr.append(node('th', label));
  $('library-head').replaceChildren(tr);
}
function renderRows(items) {
  $('library-rows').replaceChildren();
  if (!items.length) {
    const tr = node('tr'), emptyCell = node('td', '暂无数据', 'empty');
    emptyCell.colSpan = columns[state.type].length; tr.append(emptyCell); $('library-rows').append(tr); return;
  }
  for (const row of items) {
    const tr = node('tr');
    for (const [key] of columns[state.type]) tr.append(cell(key, row));
    $('library-rows').append(tr);
  }
}
function renderPagination() {
  const pages = Math.max(1, state.pages || 0);
  $('page-info').textContent = state.total ? `第 ${state.page} / ${pages} 页，共 ${state.total} 条` : '暂无数据';
  $('prev').disabled = state.page <= 1;
  $('next').disabled = !state.total || state.page >= pages;
}
async function load() {
  if (state.loading) return;
  state.loading = true; $('refresh').disabled = true; $('connection').textContent = '正在加载';
  try {
    const headers = state.token ? {Authorization: `Bearer ${state.token}`} : {};
    const response = await fetch(`/api/library?type=${encodeURIComponent(state.type)}&page=${state.page}&page_size=${state.pageSize}`, {method: 'GET', headers});
    if (!response.ok) {
      let message = `查询失败 (${response.status})`;
      try { message = (await response.json()).error || message; } catch (_) {}
      if (response.status === 401) $('token-panel').hidden = false;
      throw new Error(message);
    }
    const data = await response.json();
    state.page = data.page; state.pages = data.pages; state.total = data.total;
    renderHead(); renderRows(data.items || []); renderPagination();
    $('library-summary').textContent = `共 ${data.total} 条`;
    $('toolbar-summary').textContent = `第 ${data.page} / ${Math.max(1, data.pages)} 页`;
    $('connection').textContent = '服务已连接'; $('notice').hidden = true;
  } catch (error) {
    $('connection').textContent = '连接失败';
    $('notice').textContent = error.message || String(error); $('notice').hidden = false;
  } finally {
    state.loading = false; $('refresh').disabled = false;
  }
}
function setType(type) { if (state.type === type) return; state.type = type; state.page = 1; renderTabs(); load(); }
$('type-music').onclick = () => setType('music');
$('type-novel').onclick = () => setType('novel');
$('page-size').onchange = () => { state.pageSize = Number($('page-size').value); state.page = 1; load(); };
$('prev').onclick = () => { if (state.page > 1) { state.page--; load(); } };
$('next').onclick = () => { if (state.page < Math.max(1, state.pages)) { state.page++; load(); } };
$('refresh').onclick = load;
$('token-toggle').onclick = () => { $('token-panel').hidden = !$('token-panel').hidden; };
$('token-save').onclick = () => { state.token = $('token').value; sessionStorage.setItem('spider-token', state.token); state.page = 1; load(); };
$('token').value = state.token;
renderTabs();
load();
