'use strict';
// Exercise rendered dashboard behavior and request/selection races without a browser dependency.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
class Element {
  constructor(tag) { this.tagName = tag.toUpperCase(); this.children = []; this.value = ''; this.textContent = ''; this.hidden = false; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
}
const html = fs.readFileSync(path.join(__dirname, '../web/ui/index.html'), 'utf8');
const elements = new Map([...html.matchAll(/<(\w+)[^>]*\bid="([^"]+)"[^>]*>/g)].map(match => [match[2], new Element(match[1])]));
const requests = [];
let responses = {'/api/spiders': []};
let fetcher = async url => responses[url] || [];
const context = vm.createContext({
  document: {getElementById: id => { assert.ok(elements.has(id), `Missing element ${id}`); return elements.get(id); }, createElement: tag => new Element(tag), hidden: false},
  sessionStorage: {getItem: () => '', setItem: () => {}},
  fetch: async (url, options) => { requests.push({url, options}); return {ok: true, json: async () => fetcher(url)}; },
  setInterval: () => {}, console
});
const run = value => vm.runInContext(value, context);
const result = value => JSON.parse(run(`JSON.stringify(${value})`));
vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/ui/app.js'), 'utf8'), context);
(async () => {
  await run('refreshDashboard()');
  assert.equal(elements.get('total-count').textContent, '0');
  assert.equal(elements.get('empty-state').hidden, false);
  assert.equal(elements.get('history-card').hidden, true);
  const title = '<img src=x onerror=alert(1)>';
  responses = {
    '/api/spiders': [{id: 1, code_id: 'site-a', name: title, enabled: 1, interval_seconds: 60, status: 'failed', successes: 4, failures: 2}, {id: 2, code_id: 'site-b', name: 'B', enabled: 1, interval_seconds: 30, status: 'running', successes: 1, failures: 0}],
    '/api/runs?spider_id=1': [{id: 12, status: 'succeeded'}, {id: 11, status: 'failed'}],
    '/api/runs/12': {id: 12, status: 'succeeded', item_count: 3, started_at: 1000, finished_at: 2000},
    '/api/runs/12/events': [],
    '/api/runs/11': {id: 11, status: 'failed', error: '<b>failure</b>'},
    '/api/runs/11/events': []
  };
  await run('refreshDashboard()');
  assert.equal(elements.get('total-count').textContent, '2');
  assert.equal(elements.get('running-count').textContent, '1');
  assert.equal(elements.get('success-count').textContent, '5');
  assert.equal(elements.get('failure-count').textContent, '2');
  assert.equal(elements.get('spiders').children[0].children[0].children[0].textContent, title);
  assert.equal(elements.get('run-title').textContent, '运行 #12');
  assert.equal(elements.get('run-state').textContent, '成功');
  await run('selectRun(11)');
  assert.equal(elements.get('run-state').textContent, '失败');
  assert.equal(elements.get('run-error').textContent, '<b>failure</b>');
  // A slow older response must not replace the details of the later selection.
  const deferred = new Map();
  fetcher = url => new Promise(resolve => deferred.set(url, resolve));
  const older = run('selectRun(11)');
  const later = run('selectRun(12)');
  await new Promise(resolve => setImmediate(resolve));
  deferred.get('/api/runs/12')({id: 12, status: 'succeeded'});
  deferred.get('/api/runs/12/events')([]);
  await later;
  deferred.get('/api/runs/11')({id: 11, status: 'failed', error: 'stale'});
  deferred.get('/api/runs/11/events')([]);
  await older;
  assert.equal(elements.get('run-title').textContent, '运行 #12');
  assert.equal(elements.get('run-error').textContent, '');
  assert.equal(result('state.run'), 12);
  assert.ok(requests.length > 0);
  assert.ok(requests.every(request => request.options.method === 'GET'));
  assert.ok(requests.every(request => request.options.body === undefined));
  fetcher = async url => url === '/api/spiders' ? [] : [];
  await run('refreshDashboard()');
  assert.equal(elements.get('run-detail').hidden, true);
  assert.equal(elements.get('history-card').hidden, true);
  assert.equal(elements.get('empty-state').hidden, false);
  // An obsolete failure (including 401) must not undo a later successful refresh.
  let rejectOld;
  fetcher = () => new Promise((resolve, reject) => { rejectOld = reject; });
  const oldRefresh = run('refresh()');
  await new Promise(resolve => setImmediate(resolve));
  fetcher = async () => [];
  await run('refresh()');
  rejectOld(new Error('obsolete network failure'));
  await oldRefresh;
  assert.equal(elements.get('connection').textContent, '服务已连接');
  assert.equal(elements.get('notice').hidden, true);
  let releaseUnauthorized;
  context.fetch = async () => ({ok:false,status:401,json:() => new Promise(resolve => { releaseUnauthorized = resolve; })});
  const oldUnauthorized = run('refresh()');
  await new Promise(resolve => setImmediate(resolve));
  context.fetch = async () => ({ok:true,json:async () => []});
  elements.get('token-panel').hidden = true;
  await run('refresh()');
  releaseUnauthorized({error:'obsolete token failure'});
  await oldUnauthorized;
  assert.equal(elements.get('connection').textContent, '服务已连接');
  assert.equal(elements.get('token-panel').hidden, true);
  console.log('dashboard UI passed: status rendering, GET-only requests, stale selection protection');
})().catch(error => { console.error(error); process.exitCode = 1; });
