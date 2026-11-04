// h1u.js: Halo 1 Unlimited's additions to the dedicated server's page. The page itself (app.js, index.html,
// app.css) is OpenCE's; these live in their own files so OpenCE's updates to it merge cleanly. app.js calls two
// hooks (window.h1uServerUi.pickers and .rotation) with a few of its own functions; nothing else of it is touched.
//
// - The rotation's map list: a folder map linked to a Custom Edition map (/api/maps' "link") and that map are one
//   entry, in yellow, under the folder map's title (its info.txt's first line); choosing it adds the Custom Edition
//   map, which the server hosts linked. Other copies keep their own entries, with where they come from.
// - Filter buttons (All, Linked, Folder, Custom, Halo's) and a search box above the map list.
// - The gametype list: the gametypes the chosen map has first (a folder map's info.txt "Gametypes:" line; Halo's
//   maps have them all), the others marked "(no flags)" but still there to choose.
// - Edit on each rotation slot: the slot in the add row, ADD becomes UPDATE (with CANCEL); the slot is replaced in
//   place.

'use strict';

// ---------- logic (node tests: tools/server_ui_tests)

const H1U_ENGINES = ['slayer', 'ctf', 'king', 'oddball', 'race'];

// a "Gametypes:" line's engines (as hmp_browse.c reads it on the Xbox): a Set, or null when the map doesn't say
function h1uGametypesIn(text) {
  if (typeof text !== 'string' || !text.trim()) return null;
  const found = new Set();
  const words = text.toLowerCase().split(/[^a-z0-9]+/).filter(Boolean);
  for (const word of words) {
    if (word === 'ctf' || word === 'capture') found.add('ctf');
    else if (word === 'king' || word === 'hill' || word === 'koth') found.add('king');
    else if (word === 'oddball' || word === 'ball') found.add('oddball');
    else if (word === 'race') found.add('race');
    else if (word === 'slayer') found.add('slayer');
  }
  return found.size ? found : null;
}

// /api/maps' maps as the list shows them: {token, label, kind: linked|folder|custom|stock, gametypes: Set|null,
// search}, by label
function h1uMapItems(maps, mapToken, titleOf) {
  const list = Array.isArray(maps) ? maps : [];
  const customTokens = new Set(list.filter((map) => map.kind === 'custom').map((map) => mapToken(map).toLowerCase()));
  const linked = new Set();
  const items = [];
  const titleCount = {};
  for (const map of list) {
    if (map.kind === 'folder') titleCount[titleOf(map).toLowerCase()] = (titleCount[titleOf(map).toLowerCase()] || 0) + 1;
  }
  for (const map of list) {
    if (map.kind !== 'folder') continue;
    const link = typeof map.link === 'string' ? map.link : '';
    const gametypes = h1uGametypesIn(map.gametypes);
    const title = titleOf(map);
    const where = titleCount[title.toLowerCase()] > 1 ? ` ${map.name.replace(/^\+/, '')}` : '';
    if (link && customTokens.has(link.toLowerCase())) {
      linked.add(link.toLowerCase());
      items.push({ token: link, label: `${title} (linked${where ? ':' + where : ''})`, kind: 'linked', gametypes,
        search: `${title} ${map.name} ${link}`.toLowerCase() });
    } else {
      items.push({ token: mapToken(map), label: `${title} (folder map${where ? ':' + where : ''})`, kind: 'folder', gametypes,
        search: `${title} ${map.name}`.toLowerCase() });
    }
  }
  for (const map of list) {
    if (map.kind === 'folder') continue;
    const token = mapToken(map);
    if (map.kind === 'custom') {
      if (linked.has(token.toLowerCase())) continue;
      items.push({ token, label: `${titleOf(map)} (custom)`, kind: 'custom', gametypes: null,
        search: `${map.name} ${titleOf(map)}`.toLowerCase() });
    } else {
      items.push({ token, label: `${titleOf(map)} (Halo's)`, kind: 'stock', gametypes: new Set(H1U_ENGINES),
        search: `${titleOf(map)} ${map.name}`.toLowerCase() });
    }
  }
  return items.sort((a, b) => a.label.localeCompare(b.label));
}

function h1uFilterItems(items, filter, search) {
  const text = String(search || '').trim().toLowerCase();
  return items.filter((item) => (filter === 'all' || item.kind === filter) && (!text || item.search.includes(text)));
}

// ---------- the page

function h1uServerUi(root) {
  const doc = root.document;
  const $ = (id) => doc.getElementById(id);
  const state = { filter: 'all', search: '', items: [], editing: null, api: null };
  const FILTERS = [['all', 'All'], ['linked', 'Linked'], ['folder', 'Folder'], ['custom', 'Custom'], ['stock', "Halo's"]];

  function fillMaps() {
    const api = state.api;
    const select = $('add-map');
    if (!api || !select) return;
    const keep = select.value;
    const shown = h1uFilterItems(state.items, state.filter, state.search);
    select.replaceChildren(...shown.map((item) => {
      const node = api.option(item.token, item.label, item.token === keep);
      if (item.kind === 'linked') node.className = 'h1u-linked';
      return node;
    }));
    if (!shown.length) select.appendChild(api.option('', 'No maps match', true, true));
    markGametypes();
  }

  function chosenItem() {
    const select = $('add-map');
    return select ? state.items.find((item) => item.token === select.value) : null;
  }

  // (the chosen map's gametypes first in each group, the others marked)
  function markGametypes() {
    const api = state.api;
    const select = $('add-gametype');
    const mapSelect = $('add-map');
    if (!api || !select) return;
    const item = chosenItem();
    if (mapSelect) mapSelect.classList.toggle('h1u-linked', !!item && item.kind === 'linked');
    const files = (api.page.gametypes && api.page.gametypes.files) || [];
    const builtin = (api.page.gametypes && api.page.gametypes.builtin) || [];
    for (const group of select.querySelectorAll('optgroup')) {
      const options = Array.from(group.querySelectorAll('option'));
      for (const node of options) {
        if (!node.dataset.h1uText) node.dataset.h1uText = node.textContent;
        const file = files.find((entry) => entry.name === node.value);
        const engine = api.engineOf(file ? file.base : node.value, builtin);
        const supported = !item || !item.gametypes || !engine || item.gametypes.has(engine);
        node.dataset.h1uSupported = supported ? '1' : '0';
        node.textContent = node.dataset.h1uText + (supported ? '' : ' (no flags)');
        node.classList.toggle('h1u-unsupported', !supported);
      }
      options.sort((a, b) => Number(b.dataset.h1uSupported) - Number(a.dataset.h1uSupported));
      for (const node of options) group.appendChild(node);
    }
  }

  function ensureBar() {
    const api = state.api;
    const form = $('rotation-add');
    if (!api || !form || $('h1u-map-bar')) return;
    const buttons = FILTERS.map(([key, text]) => api.h('button', { type: 'button', class: 'small' + (key === state.filter ? ' h1u-on' : ''),
      text, data: { filter: key }, listen: { click: () => {
        state.filter = key;
        for (const node of doc.querySelectorAll('#h1u-map-bar button')) node.classList.toggle('h1u-on', node.dataset.filter === key);
        fillMaps();
      } } }));
    const search = api.h('input', { type: 'search', placeholder: 'Search maps', id: 'h1u-map-search',
      listen: { input: (event) => { state.search = event.target.value; fillMaps(); } } });
    form.parentNode.insertBefore(api.h('div', { id: 'h1u-map-bar', class: 'h1u-map-bar' }, [...buttons, search]), form);
    $('add-map').addEventListener('change', markGametypes);
    // (an edit's UPDATE: the slot replaced, before app.js's submit, which would add one)
    doc.addEventListener('submit', (event) => {
      if (event.target !== form || state.editing === null) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      try {
        const entries = state.api.page.rotation.entries.slice();
        entries[state.editing] = state.api.makeEntry($('add-map').value, $('add-gametype').value, $('add-rules').value);
        stopEditing();
        state.api.editRotation(entries);
        $('add-rules').value = '';
      } catch (error) {
        state.api.toast(error.message, true);
      }
    }, true);
  }

  function addButton() {
    return $('rotation-add') && $('rotation-add').querySelector('button[type="submit"], button:not([type])');
  }

  function stopEditing() {
    state.editing = null;
    const button = addButton();
    if (button) button.textContent = 'Add';
    const cancel = $('h1u-edit-cancel');
    if (cancel) cancel.remove();
    for (const row of doc.querySelectorAll('#rotation li.h1u-editing')) row.classList.remove('h1u-editing');
  }

  function startEditing(index) {
    const api = state.api;
    const entry = api.page.rotation.entries[index];
    if (!entry) return;
    state.editing = index;
    // (the slot's map in the list: shown whatever the filter and search, so it can be chosen)
    state.filter = 'all';
    state.search = '';
    for (const node of doc.querySelectorAll('#h1u-map-bar button')) node.classList.toggle('h1u-on', node.dataset.filter === 'all');
    if ($('h1u-map-search')) $('h1u-map-search').value = '';
    fillMaps();
    const mapSelect = $('add-map');
    const wanted = String(entry.map || '').toLowerCase();
    const match = Array.from(mapSelect.options).find((node) => node.value.toLowerCase() === wanted ||
      node.value.toLowerCase() === 'custom_maps\\' + wanted);
    if (match) mapSelect.value = match.value;
    else mapSelect.appendChild(api.option(entry.map, `${entry.map} (as written)`, true));
    markGametypes();
    if (entry.gametype) $('add-gametype').value = entry.gametype;
    const rules = String(entry.text || '').split('+').slice(1).join('+');
    $('add-rules').value = rules;
    const button = addButton();
    if (button) {
      button.textContent = 'Update';
      if (!$('h1u-edit-cancel')) {
        button.parentNode.insertBefore(api.h('button', { type: 'button', id: 'h1u-edit-cancel', class: 'small', text: 'Cancel',
          listen: { click: () => { stopEditing(); $('add-rules').value = ''; } } }), button.nextSibling);
      }
    }
    const rows = doc.querySelectorAll('#rotation li');
    for (let k = 0; k < rows.length; k++) rows[k].classList.toggle('h1u-editing', k === index);
    $('rotation-add').scrollIntoView({ block: 'nearest' });
  }

  return {
    // app.js's renderPickers: the map list and the gametypes
    pickers(api) {
      state.api = api;
      state.items = h1uMapItems(api.page.maps, api.mapToken, api.titleOf);
      ensureBar();
      fillMaps();
    },
    // app.js's renderRotation: an Edit button on each slot (admins)
    rotation(api) {
      state.api = api;
      if (!api.admin) return;
      const rows = doc.querySelectorAll('#rotation li');
      rows.forEach((row, index) => {
        const actions = row.querySelector('.actions');
        if (!actions || actions.querySelector('.h1u-edit')) return;
        actions.insertBefore(api.h('button', { type: 'button', class: 'small h1u-edit', text: 'Edit',
          listen: { click: () => startEditing(index) } }), actions.firstChild);
        row.classList.toggle('h1u-editing', index === state.editing);
      });
      if (state.editing !== null && state.editing >= api.page.rotation.entries.length) stopEditing();
    },
  };
}

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { h1uGametypesIn, h1uMapItems, h1uFilterItems };
} else {
  window.h1uServerUi = h1uServerUi(window);
}
