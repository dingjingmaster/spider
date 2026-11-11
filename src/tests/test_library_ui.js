'use strict';
// Exercise the local-library page without a browser: type columns, page size and pagination.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
class Element {
  constructor(tag) { this.tagName = tag.toUpperCase(); this.children = []; this.value = ''; this.textContent = ''; this.hidden = false; this.disabled = false; this.className = ''; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = children; }
}
const html = fs.readFileSync(path.join(__dirname, '../web/ui/library.html'), 'utf8');
const elements = new Map([...html.matchAll(/<(\w+)[^>]*\bid="([^"]+)"[^>]*>/g)].map(match => [match[2], new Element(match[1])]));
const requests = [];
const empty = {type: 'music', page: 1, page_size: 20, total: 0, pages: 0, items: []};
const music = {type: 'music', page: 1, page_size: 20, total: 3, pages: 1, items: [
  {id: 1, artist: 'A', title: 'Song A', album: 'Album', format: '24bit Flac', bytes: 10485760, seconds: 210, status: 'downloaded', decision: 'include', target_path: '/music/A/song.flac', item_identifier: 'i1', item_title: 'Source', item_url: 'https://example.com/i1'},
  {id: 2, artist: 'B', title: 'Song B', album: '', format: 'VBR MP3', bytes: 1048576, seconds: 60, status: 'pending', decision: 'include', target_path: '', item_identifier: 'i2', item_title: 'Source 2'},
  {id: 3, artist: 'C', title: 'Song C', album: '', format: 'VBR MP3', bytes: 0, seconds: 0, status: 'failed', decision: 'reject', target_path: '', item_identifier: 'i3', item_title: ''}
]};
const novel = {type: 'novel', page: 1, page_size: 20, total: 1, pages: 1, items: [
  {id: 4, author: 'Author', title: 'Book', series: 'Series', chapter_no: 3, word_count: 12000, format: 'HTML', status: 'pending', decision: 'reject', target_path: '/novels/book/3.html', item_identifier: 'b1', item_title: 'Book source', item_url: 'https://example.com/b1'}
]};
let responses = {};
const context = vm.createContext({
  document: {getElementById: id => { assert.ok(elements.has(id), `Missing element ${id}`); return elements.get(id); }, createElement: tag => new Element(tag)},
  sessionStorage: {getItem: () => '', setItem: () => {}},
  fetch: async url => { requests.push(url); return {ok: true, json: async () => responses[url] || empty}; },
  console
});
const run = value => vm.runInContext(value, context);
const settle = () => new Promise(resolve => setImmediate(resolve));
(async () => {
  responses['/api/library?type=music&page=1&page_size=20&status='] = music;
  vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/ui/library.js'), 'utf8'), context);
  for (let i = 0; i < 20 && elements.get('library-rows').children.length !== 3; i++) await settle();
  assert.equal(elements.get('library-rows').children.length, 3);
  const first = elements.get('library-rows').children[0];
  assert.equal(first.children[0].textContent, 'A');
  assert.equal(first.children[1].textContent, 'Song A');
  assert.equal(first.children[4].textContent, '10.0 MiB');
  assert.equal(first.children[5].textContent, '3:30');
  assert.equal(first.children[6].children[0].textContent, '已下载');
  assert.equal(first.children[7].children[0].textContent, '/music/A/song.flac');
  assert.equal(first.children[8].children[0].textContent, 'Source');
  assert.match(elements.get('page-info').textContent, /第 1 \/ 1 页，共 3 条/);

  responses['/api/library?type=novel&page=1&page_size=20&status='] = novel;
  run('state.type="novel";state.page=1;renderTabs()');
  await run('load()');
  const row = elements.get('library-rows').children[0];
  assert.equal(row.children[0].textContent, 'Author');
  assert.equal(row.children[1].textContent, 'Book');
  assert.equal(row.children[2].textContent, 'Series');
  assert.equal(row.children[3].textContent, '3');
  assert.equal(row.children[4].textContent.replace(/\D/g, ''), '12000');
  assert.equal(row.children[6].children[0].textContent, '待下载');
  assert.match(elements.get('page-info').textContent, /共 1 条/);

  responses['/api/library?type=music&page=1&page_size=50&status='] = {...music, page_size: 50};
  run('state.type="music";state.page=1;state.pageSize=50;renderTabs()');
  await run('load()');
  assert.ok(requests.some(url => url === '/api/library?type=music&page=1&page_size=50&status='));
  assert.match(elements.get('page-info').textContent, /共 3 条/);
  responses['/api/library?type=music&page=1&page_size=50&status=downloaded'] = {...music, page_size: 50};
  run('state.status="downloaded";state.page=1;state.pageSize=50');
  await run('load()');
  assert.ok(requests.some(url => url.includes('status=downloaded')));
  console.log('library UI passed: music/novel columns, page size and pagination');
})().catch(error => { console.error(error); process.exitCode = 1; });
