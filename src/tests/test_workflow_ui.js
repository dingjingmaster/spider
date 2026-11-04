'use strict';
// Browser logic regression checks with a small DOM substitute; no browser/Node production dependency.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
class Element {
  constructor(tag = 'div') { this.tagName = tag.toUpperCase(); this.children = []; this.dataset = {}; this.value = ''; this.type = ''; this.hidden = false; this.checked = false; this.textContent = ''; }
  append(...children) { for (const child of children) { child.parent = this; this.children.push(child); } }
  replaceChildren(...children) { this.children = []; this.append(...children); }
  remove() { this.parent.children = this.parent.children.filter(child => child !== this); }
  setAttribute(key, value) { this[key] = value; }
  get options() { return this.children; }
  querySelectorAll(selector) {
    const key = selector.slice(6, -1).replace(/-([a-z])/g, (_, char) => char.toUpperCase());
    return this.children.flatMap(child => [...(Object.hasOwn(child.dataset, key) ? [child] : []), ...child.querySelectorAll(selector)]);
  }
  querySelector(selector) { return this.querySelectorAll(selector)[0] || null; }
  click() { if (this.onclick) return this.onclick(); }
}
const html = fs.readFileSync(path.join(__dirname, '../web/ui/index.html'), 'utf8');
assert.doesNotMatch(html, /id="(?:editor-mode|simple-editor|add-field|max-pages)"/);
const elements = new Map([...html.matchAll(/<(\w+)[^>]*\bid="([^"]+)"[^>]*>/g)].map(match => [match[2], new Element(match[1])]));
const context = vm.createContext({
  document: {getElementById: id => { assert.ok(elements.has(id), `Missing HTML element ${id}`); return elements.get(id); }, createElement: tag => new Element(tag), hidden: false},
  sessionStorage: {getItem: () => '', setItem: () => {}},
  fetch: async () => ({ok: true, json: async () => []}), setInterval: () => {}, setTimeout: () => {},
  URL: {createObjectURL: () => 'blob:test', revokeObjectURL: () => {}}, Blob, confirm: () => true, console
});
vm.runInContext(fs.readFileSync(path.join(__dirname, '../web/ui/app.js'), 'utf8').replace('showRule(defaults); connect();', 'showRule(defaults);'), context);
const run = source => vm.runInContext(source, context);
const result = source => JSON.parse(run(`JSON.stringify(${source})`));
const initial = result('readRule()');
assert.equal(initial.schema_version, 2);
assert.deepEqual(initial.steps.map(step => step.type), ['request', 'foreach', 'emit']);
assert.equal(initial.steps[1].items, '{{steps.request.items}}');
assert.equal(initial.steps[2].fields.title, '{{input.title}}');
assert.throws(() => run(`showRule({name:'legacy',url:'https://example.com',fields:[{name:'title'}]})`), /schema_version/);
// Login and pure HTTP steps may omit extraction; UI must preserve that omission.
const pureRequest = {schema_version:2,name:'Login only',entry:'login',steps:[{id:'login',type:'request',request:{url:'https://example.com/login',method:'POST',json:{user:'demo'},headers:{'X-Login':'yes'},headers_origin:'https://example.com',body_origin:'https://example.com'},retry_post:false}]};
context.pureRequest = pureRequest;
run('showRule(pureRequest)');
assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), false);
assert.deepEqual(result('readRule()').steps[0].request, pureRequest.steps[0].request);
const loginCard = elements.get('workflow-steps').children[0];
const extractEnabled = loginCard.querySelector('[data-request-extract-enabled]');
assert.equal(extractEnabled.checked, false);
assert.equal(loginCard.querySelector('[data-request-fields]').disabled, true);
loginCard.querySelector('[data-request-url]').value = 'https://example.com/auth'; loginCard.querySelector('[data-request-url]').onchange();
assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), false);
extractEnabled.checked = true; extractEnabled.onchange();
assert.equal(result('readRule()').steps[0].extract.fields[0].name, 'title');
extractEnabled.checked = false; extractEnabled.onchange();
assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), false);
assert.equal(result('readRule()').steps[0].request.headers['X-Login'], 'yes');
// Deleting extract through either JSON editor also preserves the request-only step.
extractEnabled.checked = true; extractEnabled.onchange();
const loginConfig = loginCard.querySelector('[data-step-config]');
const loginData = JSON.parse(loginConfig.value); delete loginData.extract; loginConfig.value = JSON.stringify(loginData); loginConfig.onchange();
assert.equal(extractEnabled.checked, false);
assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), false);
extractEnabled.checked = true; extractEnabled.onchange();
assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), true);
elements.get('workflow-json').value = JSON.stringify(pureRequest); elements.get('apply-workflow-json').onclick();
assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), false);
const example = result('workflowExample()');
assert.equal(example.schema_version, 2);
assert.deepEqual(example.steps.map(step => step.type), ['request', 'foreach', 'request', 'emit']);
assert.equal(example.steps[2].next, 'save');
run(`showRule({...workflowExample(), custom_property: {keep: true}, fields: [{name: 'export_only'}]}, 7, 3)`);
let rule = result('readRule()');
assert.equal(rule.custom_property.keep, true);
assert.equal(rule.fields[0].name, 'export_only');
assert.equal(rule.steps[2].request.url, '{{input.url}}');
assert.equal(elements.get('workflow-editor').hidden, false);
assert.equal(elements.get('version').textContent, '版本 3');
// Configuration, ID, next and type fields all contribute to the saved workflow.
const first = elements.get('workflow-steps').children[0];
first.querySelector('[data-step-config]').value = JSON.stringify({request: {url: 'https://example.com/new'}, extract: {format: 'json', fields: [{name: 'url', selector: '/url'}]}, retry_post: true});
first.querySelector('[data-step-config]').onchange();
assert.equal(result('readRule()').steps[0].retry_post, true);
// Request controls update their step config and preserve unrelated attributes.
const requestURL = first.querySelector('[data-request-url]');
assert.equal(requestURL.value, 'https://example.com/new');
requestURL.value = 'https://example.com/from-form'; requestURL.onchange();
assert.equal(result('readRule()').steps[0].request.url, 'https://example.com/from-form');
assert.equal(result('readRule()').steps[0].retry_post, true);
const requestFormat = first.querySelector('[data-request-format]');
requestFormat.value = 'json'; requestFormat.onchange();
assert.equal(result('readRule()').steps[0].extract.format, 'json');
const extractFields = first.querySelector('[data-request-fields]');
extractFields.value = JSON.stringify([{name:'headline',selector:'/headline',type:'string'}]); extractFields.onchange();
assert.equal(result('readRule()').steps[0].extract.fields[0].name, 'headline');
const beforeInvalidFields = result('readRule()'); extractFields.value = '{}'; extractFields.onchange();
assert.throws(() => result('readRule()'), /JSON 数组/);
extractFields.value = JSON.stringify(beforeInvalidFields.steps[0].extract.fields); extractFields.onchange();
assert.deepEqual(result('readRule()'), beforeInvalidFields);
// Full JSON applies without flattening unknown root/step properties.
elements.get('workflow-json').value = JSON.stringify({...example, opaque: [1, 2]}); elements.get('apply-workflow-json').onclick();
assert.deepEqual(result('readRule()').opaque, [1, 2]);
assert.throws(() => run(`validateWorkflow({...workflowExample(), entry:'missing'})`), /入口/);
assert.throws(() => run(`validateWorkflow({...workflowExample(), steps:[{id:'a',type:'emit',next:'missing'}],entry:'a'})`), /不存在/);
// Traces and hostile response text are plain text, with stable selection on refresh.
const hostile = '<img src=x onerror=alert(1)>';
context.hostile = hostile;
run(`state.run = 5; renderSteps([{id:11,step_id:'list',parent_id:null,branch_key:'root',record_key:'one',status:'done',attempts:1,phase:0,error:hostile},{id:12,step_id:'detail',status:'done',attempts:1,phase:0}])`);
assert.equal(elements.get('trace-detail').hidden, true);
assert.equal(elements.get('trace-output').textContent, '');
(async () => {
  run(`api = async route => { globalThis.detailRoute = route; return {id:11,step_id:'list',context:{input:hostile},output:{response:{body:hostile}},error:hostile}; };`);
  await run('selectStep(11)');
  assert.equal(run('detailRoute'), '/api/runs/5/steps/11');
  assert.equal(elements.get('trace-error').textContent, hostile);
  assert.match(elements.get('trace-output').textContent, /<img src=x/);
  assert.equal(elements.get('trace-output').children.length, 0);
  // Later selections and run changes must not display a stale in-flight response.
  run(`globalThis.pendingDetails = []; api = route => new Promise(resolve => pendingDetails.push({route,resolve})); globalThis.oldDetail = selectStep(11); globalThis.newDetail = selectStep(12);`);
  assert.equal(elements.get('trace-output').textContent, '');
  assert.equal(elements.get('trace-detail').hidden, true);
  run(`pendingDetails[1].resolve({id:12,step_id:'detail',context:{},output:{title:'fresh'},error:''});`);
  await run('newDetail');
  run(`pendingDetails[0].resolve({id:11,step_id:'list',context:{},output:{title:'stale'},error:''});`);
  await run('oldDetail');
  assert.match(elements.get('trace-output').textContent, /fresh/);
  assert.doesNotMatch(elements.get('trace-output').textContent, /stale/);
  run(`globalThis.changedRunDetail = selectStep(11); state.run = 6; clearStepTrace(); pendingDetails[2].resolve({id:11,step_id:'list',context:{},output:{title:'wrong run'},error:''});`);
  await run('changedRunDetail');
  assert.equal(elements.get('trace-output').textContent, '');
  assert.equal(elements.get('trace-detail').hidden, true);
  run(`renderSteps([])`); assert.equal(elements.get('step-traces').hidden, true);
  run(`api = async (route, method, payload) => { globalThis.lastPayload = payload; return {items:[{title:'ok'}],response:{status:200,body:hostile,truncated:false}}; };`);
  elements.get('preview-step').value = 'list'; elements.get('preview-context').value = JSON.stringify({run: {site: 'example'}, input: {}, steps: {}});
  await elements.get('preview').onclick();
  assert.equal(result('lastPayload').rule.schema_version, 2);
  assert.equal(result('lastPayload').step_id, 'list');
  assert.equal(result('lastPayload').context.run.site, 'example');
  assert.equal(elements.get('raw-response').textContent, hostile);
  assert.equal(elements.get('preview-card').hidden, false);
  elements.get('preview-context').value = 'invalid';
  await elements.get('preview').onclick();
  assert.equal(elements.get('preview-card').hidden, true);
  assert.equal(elements.get('preview-output').textContent, '');
  // Old simple-rule files are rejected without replacing the current workflow.
  const beforeLegacyImport = result('readRule()');
  elements.get('import-file').files = [{size:100,text:async () => JSON.stringify({name:'legacy',url:'https://example.com',fields:[{name:'title'}]})}];
  await elements.get('import-file').onchange();
  assert.deepEqual(result('readRule()'), beforeLegacyImport);
  assert.match(elements.get('notice').textContent, /schema_version/);
  // Import a v2 workflow without legacy root.fields; save/export retain all workflow structure.
  context.imported = {...example, extra_setting: {preserved: true}};
  elements.get('import-file').files = [{size: 100, text: async () => JSON.stringify(context.imported)}];
  await elements.get('import-file').onchange();
  assert.equal(result('readRule()').schema_version, 2);
  assert.equal(result('readRule()').extra_setting.preserved, true);
  run(`saveBlob = (blob, name) => { globalThis.exported = blob; globalThis.exportName = name; };`);
  elements.get('export-rule').onclick();
  assert.equal(JSON.parse(await run('exported.text()')).extra_setting.preserved, true);
  run(`api = async (route, method, payload) => {
    if (route === '/api/spiders' && method === 'POST') { globalThis.saved = payload; return {id:9}; }
    if (route === '/api/spiders/9') return {id:9,version:1,rule:globalThis.saved};
    return [];
  };`);
  await run('save()');
  assert.equal(result('saved').steps.length, 4);
  assert.equal(result('saved').extra_setting.preserved, true);
  assert.equal(result('readRule()').schema_version, 2);
  run('showRule(pureRequest)');
  await run('save()');
  assert.equal(Object.hasOwn(result('saved').steps[0], 'extract'), false);
  assert.deepEqual(result('saved').steps[0].request, pureRequest.steps[0].request);
  assert.equal(Object.hasOwn(result('readRule()').steps[0], 'extract'), false);
  elements.get('new').onclick();
  assert.deepEqual(result('readRule()').steps.map(step => step.type), ['request', 'foreach', 'emit']);
  assert.equal(result('readRule()').schema_version, 2);
  assert.equal(run('state.spider'), null);
  console.log('workflow UI checks passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
