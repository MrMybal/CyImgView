// Component fixtures only: no browser navigation, script evaluation or resource fetching.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { Window } from 'happy-dom';
import { mountViewer } from 'cydoc/viewer';
import imageModule from 'cydoc/modules/image';
import galleryModule from 'cydoc/modules/gallery';
const data = JSON.parse(await readFile(new URL('../Documentation/DocData/manifest.json', import.meta.url), 'utf8'));
const settle = () => new Promise(resolve => setImmediate(resolve));
function fixture(t) {
  const window = new Window({ settings: { disableJavaScriptFileLoading: true, enableJavaScriptEvaluation: false,
    enableImageFileLoading: false, enableCSSFileLoading: false } });
  const bindings = { window, document: window.document, localStorage: window.localStorage, location: window.location,
    navigator: window.navigator, ResizeObserver: window.ResizeObserver, IntersectionObserver: window.IntersectionObserver,
    matchMedia: window.matchMedia.bind(window), requestAnimationFrame: window.requestAnimationFrame.bind(window),
    cancelAnimationFrame: window.cancelAnimationFrame.bind(window) };
  const previous = new Map();
  for (const [key, value] of Object.entries(bindings)) {
    previous.set(key, Object.getOwnPropertyDescriptor(globalThis, key));
    Object.defineProperty(globalThis, key, { configurable: true, writable: true, value });
  }
  const host = window.document.createElement('div'); window.document.body.append(host);
  const abort = new window.AbortController();
  t.after(async () => {
    abort.abort(); await window.happyDOM.abort(); window.close();
    for (const [key, descriptor] of previous) {
      if (descriptor) Object.defineProperty(globalThis, key, descriptor); else delete globalThis[key];
    }
  });
  return { window, host, context: { locale: 'fr', storageKey: 'cyimgview-fixture', signal: abort.signal, assetUrl: src => `DocData/${src}` } };
}
function media(window, pageId, id) {
  const page = data.pages.find(page => page.id === pageId && page.locale === 'fr');
  const content = window.document.createElement('div'); content.innerHTML = page.html;
  return JSON.parse(content.querySelector(`[data-cydoc-id="${id}"]`).dataset.config);
}
test('the real manual keeps its route, translation, theme and reading position', t => {
  const { window, host } = fixture(t);
  const route = { hash: '#/fr/viewer', href: '#/fr/viewer' };
  Object.defineProperty(globalThis, 'location', { configurable: true, writable: true, value: route });
  let top = 0;
  Object.defineProperty(window, 'scrollY', { configurable: true, get: () => top });
  window.scrollTo = options => { top = options.top; };
  const viewer = mountViewer(host, data, { homePage: 'overview', storageId: 'cyimgview-manual', initialTheme: 'nord', assetBase: 'DocData' });
  const input = hash => { route.hash = hash; window.dispatchEvent(new window.Event('hashchange')); };
  assert.equal(host.dataset.theme, 'nord');
  assert.match(host.querySelector('h1').textContent, /Voir et parcourir/);
  assert.ok(host.querySelector('.skip-link'));
  assert.equal(host.querySelector('.brand-logo').getAttribute('src'), 'DocData/assets/app-logo.png');
  assert.equal(host.querySelectorAll('.locale-control option').length, 2);
  assert.equal(host.querySelectorAll('.local-note,.sidebar-foot,.mit-tag').length, 0);
  const editBranch = host.querySelector('[data-page=edit]').closest('.nav-group');
  assert.ok(editBranch.querySelector('[data-page=layers]'));
  const layerLink = host.querySelector('[data-page=layers]');
  layerLink.focus();
  assert.equal(host.querySelector('.reader-tooltip').hidden, false);
  assert.match(host.querySelector('.reader-tooltip-description').textContent, /masques/);
  window.document.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'k', ctrlKey: true, bubbles: true }));
  assert.equal(host.querySelector('.search-dialog').open, true);
  const search = host.querySelector('.search-form input');
  search.value = 'masques'; search.dispatchEvent(new window.Event('input', { bubbles: true }));
  assert.ok(host.querySelector('.search-result[href*="/layers"]'));
  search.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
  assert.equal(host.querySelector('.search-dialog').open, false);
  const headerSearch = host.querySelector('[data-header-search]');
  headerSearch.value = 'dégradé'; headerSearch.dispatchEvent(new window.Event('input', { bubbles: true }));
  assert.equal(host.querySelector('.search-dialog').classList.contains('search-popover'), true);
  assert.ok(host.querySelector('.search-result[href*="/edit"]'));
  headerSearch.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
  const nav = host.querySelector('.side-nav'); nav.scrollTop = 220;
  top = 730;
  input('#/fr/layers'); assert.equal(top, 0);
  assert.match(host.querySelector('h1').textContent, /calques/i);
  assert.equal(host.querySelector('.side-nav').scrollTop, 220);
  input('#/fr/viewer'); assert.equal(top, 730);
  host.querySelector('#channels').scrollIntoView = () => { top = 1300; };
  input('#/fr/viewer/channels'); assert.equal(top, 1300);
  const locale = host.querySelector('.locale-control select'); locale.value = 'en';
  locale.dispatchEvent(new window.Event('change', { bubbles: true }));
  assert.equal(route.hash, '#/en/viewer/channels');
  window.dispatchEvent(new window.Event('hashchange'));
  assert.ok(host.querySelector('#channels'));
  assert.match(host.querySelector('h1').textContent, /View and browse/);
  host.querySelector('[data-set-theme=contrast]').click();
  assert.equal(window.localStorage.getItem('cydoc:theme'), 'contrast');
  input('#/de/layers/masks');
  assert.match(host.querySelector('h1').textContent, /Layers/);
  assert.ok(host.querySelector('#masks'));
  viewer.destroy(); assert.equal(host.innerHTML, '');
});
test('annotated screenshots zoom and expand inside the window with keyboard access', async t => {
  const { window, host, context } = fixture(t);
  const config = media(window, 'overview', 'viewer-overview');
  const cleanup = imageModule.mount(host, config, context);
  assert.ok(host.querySelectorAll('[data-image-note]').length >= 2);
  assert.match(host.querySelector('img').getAttribute('src'), /^DocData\/assets\//);
  let fullscreenCalls = 0; host.requestFullscreen = async () => { fullscreenCalls++; };
  const stage = host.querySelector('.media-stage');
  const original = host.querySelector('output').textContent;
  stage.dispatchEvent(new window.KeyboardEvent('keydown', { key: '+', bubbles: true }));
  assert.notEqual(host.querySelector('output').textContent, original);
  const expand = host.querySelector('[data-action=expand]');
  expand.click(); await settle();
  assert.equal(host.classList.contains('media-expanded'), true);
  assert.equal(fullscreenCalls, 0);
  assert.equal(expand.getAttribute('aria-expanded'), 'true');
  window.document.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Escape', bubbles: true }));
  assert.equal(host.classList.contains('media-expanded'), false);
  cleanup?.();
});
test('the actual colour comparison responds to pointer and keyboard input', t => {
  const { window, host, context } = fixture(t);
  const cleanup = imageModule.mount(host, media(window, 'black-white', 'color-comparison'), context);
  const stage = host.querySelector('.media-stage');
  stage.getBoundingClientRect = () => ({ left: 100, top: 0, width: 400, height: 240 });
  const grab = host.querySelector('.compare-grab'); grab.setPointerCapture = () => {};
  grab.dispatchEvent(new window.PointerEvent('pointerdown', { pointerId: 1, clientX: 300, bubbles: true }));
  grab.dispatchEvent(new window.PointerEvent('pointermove', { pointerId: 1, clientX: 420, bubbles: true }));
  assert.equal(grab.getAttribute('aria-valuenow'), '80');
  grab.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'ArrowLeft', bubbles: true }));
  assert.equal(host.querySelector('input[type=range]').value, '79');
  grab.dispatchEvent(new window.KeyboardEvent('keydown', { key: 'Home', bubbles: true }));
  assert.equal(grab.getAttribute('aria-valuenow'), '0');
  cleanup?.();
});
test('the six-workspace gallery loads only its selected image reader', async t => {
  const { window, host, context } = fixture(t);
  const loads = [], disposed = [];
  context.loadModule = async kind => {
    loads.push(kind); assert.equal(kind, 'image');
    return { mount(element, config, child) {
      const cleanup = imageModule.mount(element, config, child);
      return () => { disposed.push(config.src); cleanup?.(); };
    } };
  };
  const cleanup = galleryModule.mount(host, media(window, 'workspaces', 'workspace-gallery'), context);
  await settle();
  assert.deepEqual(loads, ['image']);
  assert.equal(host.querySelector('output').textContent, '1 / 6');
  host.querySelector('[data-gallery=next]').click(); await settle();
  assert.deepEqual(loads, ['image', 'image']);
  assert.equal(disposed.length, 1);
  assert.equal(host.querySelector('output').textContent, '2 / 6');
  assert.equal(host.querySelectorAll('[role=tab][aria-selected=true]').length, 1);
  cleanup(); assert.equal(disposed.length, 2);
});
