// Halo 1 Unlimited's additions to the page (port/assets/server_ui/h1u.js): the rotation's map list and gametypes

'use strict';

const test = require('node:test');
const assert = require('node:assert');
const { h1uGametypesIn, h1uMapItems, h1uFilterItems } = require('../../port/assets/server_ui/h1u.js');
const { mapToken, titleOf } = require('../../port/assets/server_ui/app.js');

const maps = [
  { name: '+CustomEd/Anxiety', kind: 'folder', title: 'Anxiety', gametypes: 'Slayer, CTF', link: 'custom_maps\\anxiety' },
  { name: 'anxiety', kind: 'custom', title: 'anxiety' },
  { name: '+Lone/Thing', kind: 'folder', title: 'Thing', link: 'custom_maps\\missing' },
  { name: '+PackA/Bigass', kind: 'folder', title: 'Bigass Day' },
  { name: '+PackB/Bigass', kind: 'folder', title: 'Bigass Day', gametypes: 'King of the Hill' },
  { name: 'beavercreek', kind: 'xbox', title: 'Battle Creek' },
];

test('a "Gametypes:" line', () => {
  assert.deepStrictEqual([...h1uGametypesIn('Slayer, CTF, King of the Hill')].sort(), ['ctf', 'king', 'slayer']);
  assert.strictEqual(h1uGametypesIn(''), null);
  assert.strictEqual(h1uGametypesIn(undefined), null);
});

test('a linked pair is one entry, under the folder map\'s title, adding the Custom Edition map', () => {
  const items = h1uMapItems(maps, mapToken, titleOf);
  const linked = items.filter((item) => item.kind === 'linked');
  assert.strictEqual(linked.length, 1);
  assert.strictEqual(linked[0].token, 'custom_maps\\anxiety');
  assert.strictEqual(linked[0].label, 'Anxiety (linked)');
  assert.ok(!items.some((item) => item.kind === 'custom' && item.token === 'custom_maps\\anxiety'));
  // (a link to a Custom Edition map the server hasn't: the folder map alone)
  assert.ok(items.some((item) => item.kind === 'folder' && item.token === '+Lone/Thing'));
});

test('copies of one title say where they are; filters and search', () => {
  const items = h1uMapItems(maps, mapToken, titleOf);
  assert.deepStrictEqual(items.filter((item) => item.label.startsWith('Bigass')).map((item) => item.label),
    ['Bigass Day (folder map: PackA/Bigass)', 'Bigass Day (folder map: PackB/Bigass)']);
  assert.strictEqual(h1uFilterItems(items, 'stock', '').length, 1);
  assert.strictEqual(h1uFilterItems(items, 'all', 'bigass').length, 2);
  assert.ok(h1uFilterItems(items, 'stock', '')[0].gametypes.has('race'));
});
