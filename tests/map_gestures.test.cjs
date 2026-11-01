const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {test} = require('node:test');
const vm = require('node:vm');

const source = readFileSync(join(__dirname, '../RID_Workbench/map_picker.h'), 'utf8').split('R"JS(')[1].split(')JS";')[0];

function harness() {
  const nodes = [], frames = [], grid = [], tileKeys = [], selections = [], imageUrls = [];
  const element = () => {
    const node = {style: {}, setAttribute() {}, append() {}, after() {}, addEventListener() {}};
    nodes.push(node);
    return node;
  };
  const listeners = new Map(), captures = new Set();
  const context = new Proxy({}, {get(target, key) {
    if (key === 'strokeRect') return (...args) => grid.push(args);
    return target[key] || (() => {});
  }});
  const rect = {left: 20, top: 40, width: 400, height: 300};
  const canvas = {
    ...element(), parentElement: element(), clientWidth: 400, clientHeight: 300,
    getBoundingClientRect: () => rect, getContext: () => context,
    addEventListener: (name, handler, options) => listeners.set(name, {handler, options}),
    setPointerCapture: id => captures.add(id), hasPointerCapture: id => captures.has(id),
    releasePointerCapture(id) {captures.delete(id); emit('lostpointercapture', {pointerId: id});}
  };
  const sandbox = {
    document: {createElement: element, createTextNode: text => ({textContent: text})}, devicePixelRatio: 2, URL,
    requestAnimationFrame: callback => frames.push(callback),
    ResizeObserver: class {observe() {}},
    Image: class {set src(url) {imageUrls.push(url);}},
    setTimeout() {return 0;}, clearTimeout() {},
  };
  sandbox.window = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(source, sandbox);
  sandbox.RidIpLocation.lookup = () => new Promise(() => {});
  sandbox.RidOfflineTiles.get = key => {tileKeys.push(key); return Promise.resolve(null);};
  const map = sandbox.createRidMap(canvas, (...args) => selections.push(args), {status: element(), radius: 0});
  function emit(name, props = {}) {
    const event = {button: 0, clientX: 220, clientY: 190, deltaMode: 0, prevented: false,
      preventDefault() {this.prevented = true;}, ...props};
    listeners.get(name).handler(event);
    return event;
  }
  function view() {
    grid.length = 0; tileKeys.length = 0;
    sandbox.RidOfflineTiles.listeners.forEach(listener => listener());
    map.draw();
    while (frames.length) frames.shift()();
    const link = nodes.find(node => node.textContent === '在高德查看当前地图中心');
    link.onclick();
    const [lon, lat] = new URL(link.href).searchParams.get('position').split(',').map(Number);
    const zoom = Number(tileKeys[0].split('/')[0]) + Math.log2(grid[0][2] / 256);
    return {lat, lon, zoom};
  }
  function coordinate(viewport, x, y) {
    const m = sandbox.RidMapMath, c = m.project(viewport.lat, viewport.lon, viewport.zoom);
    return m.unproject(c[0] + x - rect.left - rect.width / 2, c[1] + y - rect.top - rect.height / 2, viewport.zoom);
  }
  return {map, emit, view, coordinate, selections, listeners, tileKeys, imageUrls};
}

function near(actual, expected, tolerance = 1e-8) {
  assert.ok(Math.abs(actual - expected) < tolerance, `${actual} ≠ ${expected}`);
}

test('wheel zoom follows the cursor and uses integer tile addresses during smooth scaling', () => {
  const h = harness(), before = h.view(), anchor = h.coordinate(before, 320, 130);
  const event = h.emit('wheel', {clientX: 320, clientY: 130, deltaY: -100});
  const after = h.view(), actual = h.coordinate(after, 320, 130);
  near(after.zoom, before.zoom + 0.3);
  actual.forEach((value, i) => near(value, anchor[i]));
  assert.equal(event.prevented, true);
  assert.equal(h.listeners.get('wheel').options.passive, false);
  assert.ok(h.tileKeys.every(key => /^\d+\/\d+\/\d+$/.test(key)));
  assert.equal(h.selections.length, 0);
});

test('pinch can zoom and pan around its midpoint without selecting on finger release', () => {
  const h = harness(), before = h.view(), anchor = h.coordinate(before, 220, 190);
  h.emit('pointerdown', {pointerId: 1, clientX: 170});
  h.emit('pointerdown', {pointerId: 2, clientX: 270});
  h.emit('pointermove', {pointerId: 1, clientX: 130, clientY: 210});
  h.emit('pointermove', {pointerId: 2, clientX: 330, clientY: 210});
  const after = h.view(), actual = h.coordinate(after, 230, 210);
  near(after.zoom, before.zoom + 1);
  actual.forEach((value, i) => near(value, anchor[i]));
  h.emit('pointerup', {pointerId: 1});
  h.emit('pointerup', {pointerId: 2});
  assert.equal(h.selections.length, 0);
  h.emit('pointerdown', {pointerId: 3});
  h.emit('pointerup', {pointerId: 3});
  assert.equal(h.selections.length, 1);
});

test('mouse drag, secondary mouse buttons, capture loss and cancellation preserve selection', () => {
  const h = harness(), before = h.view();
  h.emit('pointerdown', {pointerId: 1});
  h.emit('pointermove', {pointerId: 1, clientX: 260});
  h.emit('pointerup', {pointerId: 1, clientX: 260});
  assert.notEqual(h.view().lon, before.lon);
  h.emit('pointerdown', {pointerId: 2, button: 2});
  h.emit('pointerup', {pointerId: 2, button: 2});
  for (const name of ['pointercancel', 'lostpointercapture']) {
    h.emit('pointerdown', {pointerId: 3}); h.emit(name, {pointerId: 3}); h.emit('pointerup', {pointerId: 3});
  }
  assert.equal(h.selections.length, 0);
});

test('zoom respects bounds, wheel delta modes, and disabled state clears an active gesture', () => {
  const h = harness();
  h.map.zoom(100); near(h.view().zoom, 17);
  h.emit('wheel', {deltaY: -100}); near(h.view().zoom, 17);
  h.map.zoom(-100); near(h.view().zoom, 3);
  h.emit('wheel', {deltaY: 100}); near(h.view().zoom, 3);
  h.emit('wheel', {deltaY: -1, deltaMode: 1}); near(h.view().zoom, 3.048);
  h.emit('wheel', {deltaY: -1, deltaMode: 2}); near(h.view().zoom, 3.408);
  h.emit('pointerdown', {pointerId: 1});
  const before = h.view(); h.map.setDisabled(true);
  h.emit('wheel', {deltaY: -100}); h.map.zoom(2);
  h.emit('pointermove', {pointerId: 1, clientX: 280}); h.emit('pointerup', {pointerId: 1});
  assert.deepEqual(h.view(), before);
  h.map.setDisabled(false); h.emit('pointerup', {pointerId: 1});
  assert.equal(h.selections.length, 0);
});

test('online tiles keep valid XYZ URLs when a wheel gesture stops at a fractional zoom', () => {
  const h = harness();
  h.emit('wheel', {deltaY: -100}); h.map.online(true); h.view();
  assert.ok(h.imageUrls.length > 0);
  assert.ok(h.imageUrls.every(url => /^https:\/\/tile\.openstreetmap\.org\/\d+\/\d+\/\d+\.png$/.test(url)));
});

test('adding a third finger and continuing with one finger do not jump or select', () => {
  const h = harness();
  h.emit('pointerdown', {pointerId: 1, clientX: 170});
  h.emit('pointerdown', {pointerId: 2, clientX: 270});
  h.emit('pointermove', {pointerId: 2, clientX: 370});
  const before = h.view();
  h.emit('pointerdown', {pointerId: 3, clientX: 220});
  h.emit('pointerup', {pointerId: 3});
  h.emit('pointerup', {pointerId: 2});
  assert.deepEqual(h.view(), before);
  h.emit('pointermove', {pointerId: 1, clientX: 190});
  const after = h.view();
  assert.notEqual(after.lon, before.lon); near(after.zoom, before.zoom);
  h.emit('pointerup', {pointerId: 1});
  assert.equal(h.selections.length, 0);
});
