// SlothDB playground - VS Code-style IDE for SQL against local files.
// Boots slothdb.wasm, fetches demo CSV/Parquet, wires activity bar / editor / panel / status bar.
//
// BUILD_VERSION - bump on every rebuild/push so browsers refetch the
// wasm / js / css / cm bundle instead of serving the cached prior version.
const BUILD_VERSION = '20260428-1';

import createSlothDB from './slothdb.js?v=20260428-1';
import {
    EditorView, basicSetup, keymap, EditorState,
    indentWithTab, sql, oneDark,
} from './vendor/cm.js?v=20260428-1';

const $ = (s, root = document) => root.querySelector(s);
const $$ = (s, root = document) => Array.from(root.querySelectorAll(s));

// ────────────────────────────────────────────────────────────
// State

let mod = null;
let editor = null;
let lastResult = null;
let sortState = { col: -1, dir: 1 };
const files = []; // user-added files: {path, size, kind}
const history = []; // {sql, ms, rowCount, error, at}

// ────────────────────────────────────────────────────────────
// Utilities

function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({
        '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;',
    })[c]);
}

function formatBytes(n) {
    if (n < 1024) return `${n} B`;
    if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
    return `${(n / 1024 / 1024).toFixed(1)} MB`;
}

function extOf(name) {
    const dot = name.lastIndexOf('.');
    return dot === -1 ? '' : name.slice(dot + 1).toLowerCase();
}

// ────────────────────────────────────────────────────────────
// Logging

function log(text, cls = '') {
    const now = new Date();
    const ts = now.toTimeString().slice(0, 8);
    const el = document.createElement('div');
    el.className = 'log-line' + (cls ? ' ' + cls : '');
    el.innerHTML = `<span class="ts">[${ts}]</span>${escapeHtml(text)}`;
    const box = $('#messages');
    box.appendChild(el);
    box.scrollTop = box.scrollHeight;
}

// ────────────────────────────────────────────────────────────
// Status bar

function setStatus(text, state = 'ok') {
    const dot = $('#sb-dot');
    const status = $('#sb-status');
    const bar = $('.statusbar');
    status.textContent = text;
    bar.classList.remove('boot', 'err');
    if (state === 'boot') bar.classList.add('boot');
    if (state === 'err') bar.classList.add('err');
    if (state === 'ok')  dot.style.background = '#102710';
    if (state === 'err') dot.style.background = '#fff';
}

function setRowStats(rows, ms, err) {
    $('#sb-rows').textContent = err ? 'error' : (rows != null ? `${rows} rows` : '-');
    $('#sb-time').textContent = ms != null ? `${ms.toFixed(1)} ms` : '-';
}

function refreshFileCount() {
    const n = 1 + files.length;
    $('#sb-files').textContent = `${n} file${n === 1 ? '' : 's'}`;
}

// ────────────────────────────────────────────────────────────
// File tree

function refreshFileList() {
    const entries = [
        { path: '/data/sales.csv',     size: DEMO_SIZES.csv, demo: true, kind: 'csv' },
        { path: '/data/sales.parquet', size: DEMO_SIZES.parquet, demo: true, kind: 'parquet' },
        ...files,
    ];
    $('#file-list').innerHTML = entries.map((f) => {
        const kind = f.kind || extOf(f.path) || 'file';
        const name = f.path.split('/').pop();
        return `<li data-path="${escapeHtml(f.path)}" title="Click to insert file reference">
            <span class="file-icon ${kind}">${kind.slice(0, 3)}</span>
            <span class="file-name">${escapeHtml(name)}</span>
            <span class="file-size">${formatBytes(f.size)}</span>
        </li>`;
    }).join('');
    refreshFileCount();
}

$('#file-list').addEventListener('click', (e) => {
    const li = e.target.closest('li');
    if (!li) return;
    const ref = `'${li.dataset.path}'`;
    editor.dispatch(editor.state.replaceSelection(ref));
    editor.focus();
});

async function handleFiles(flist) {
    const list = Array.from(flist || []);
    for (const f of list) {
        const buf = new Uint8Array(await f.arrayBuffer());
        const path = `/data/${f.name}`;
        try { mod.FS.mkdir('/data'); } catch (_) {}
        mod.FS.writeFile(path, buf);
        files.push({ path, size: buf.byteLength, kind: extOf(f.name) });
        log(`Loaded ${path} (${formatBytes(buf.byteLength)}).`, 'ok');
    }
    refreshFileList();
    $('#file-input').value = '';
}

$('#file-input').addEventListener('change', (e) => handleFiles(e.target.files));

// ────────────────────────────────────────────────────────────
// Floating tooltip - rendered at <body> so it isn't clipped by any
// ancestor overflow (e.g. the sidebar's overflow-y: auto). Any element
// with `data-tooltip="..."` gets it for free.

(function wireTooltips() {
    const bubble = document.createElement('div');
    bubble.className = 'tt-bubble';
    bubble.setAttribute('role', 'tooltip');
    document.body.appendChild(bubble);

    function show(target) {
        const text = target.getAttribute('data-tooltip');
        if (!text) return;
        bubble.textContent = text;
        const r = target.getBoundingClientRect();
        const GAP = 10;
        // Measure after text set.
        bubble.style.visibility = 'hidden';
        bubble.classList.remove('below');
        bubble.classList.add('show');
        const bw = bubble.offsetWidth;
        const bh = bubble.offsetHeight;
        bubble.style.visibility = '';

        // Horizontal: center on the target, clamp to viewport.
        const centerX = r.left + r.width / 2;
        const maxLeft = window.innerWidth - bw - 8;
        const left = Math.max(8, Math.min(centerX - bw / 2, maxLeft));

        // Vertical: prefer above; flip below if we'd crowd the title bar
        // (first 40 px of viewport). Accounts for multi-line tooltips whose
        // height puts them into the chrome even when their top is positive.
        const aboveTop = r.top - bh - GAP;
        const below = aboveTop < 40;
        const top = below ? r.bottom + GAP : aboveTop;

        bubble.style.left = `${left}px`;
        bubble.style.top = `${top}px`;
        if (below) bubble.classList.add('below');
        // Anchor the arrow back at the target's center regardless of clamp.
        bubble.style.setProperty('--arrow-x', `${centerX - left}px`);
    }
    function hide() {
        bubble.classList.remove('show');
    }

    document.addEventListener('mouseenter', (e) => {
        const t = e.target.closest?.('[data-tooltip]');
        if (t) show(t);
    }, true);
    document.addEventListener('mouseleave', (e) => {
        if (e.target.closest?.('[data-tooltip]')) hide();
    }, true);
    document.addEventListener('focusin', (e) => {
        if (e.target.closest?.('[data-tooltip]')) show(e.target.closest('[data-tooltip]'));
    });
    document.addEventListener('focusout', hide);
})();

// ────────────────────────────────────────────────────────────
// Activity bar / view switching

$$('.act').forEach((btn) => {
    btn.addEventListener('click', () => {
        const view = btn.dataset.view;
        $$('.act').forEach((b) => b.classList.toggle('active', b === btn));
        $$('.view').forEach((v) => { v.hidden = v.dataset.view !== view; });
    });
});

// Tree group collapse / expand
$$('.tree-head').forEach((head) => {
    head.addEventListener('click', (e) => {
        if (e.target.closest('.tree-action')) return;
        head.parentElement.classList.toggle('collapsed');
    });
});

// ────────────────────────────────────────────────────────────
// Snippets (sidebar + larger view)

const SNIPPETS = [
    { name: 'Count CSV rows',
      sql: "SELECT COUNT(*) AS rows FROM '/data/sales.csv';" },
    { name: 'Count Parquet rows',
      sql: "SELECT COUNT(*) AS rows FROM '/data/sales.parquet';" },
    { name: 'GROUP BY region (CSV)',
      sql: "SELECT region, SUM(revenue) AS total\nFROM '/data/sales.csv'\nGROUP BY region\nORDER BY region;" },
    { name: 'GROUP BY region (Parquet)',
      sql: "SELECT region, SUM(revenue) AS total\nFROM '/data/sales.parquet'\nGROUP BY region\nORDER BY region;" },
    { name: 'CSV vs Parquet (same data)',
      sql: "-- Same deterministic 1,000-row dataset, two formats.\n-- Row 1 = CSV, Row 2 = Parquet. Identical values prove format parity.\nSELECT COUNT(*) AS n_rows, SUM(revenue) AS sum_revenue FROM '/data/sales.csv'\nUNION ALL\nSELECT COUNT(*),            SUM(revenue)              FROM '/data/sales.parquet';" },
    { name: 'Two-column GROUP BY',
      sql: "SELECT product, year,\n       SUM(revenue) AS rev,\n       SUM(qty)     AS qty,\n       COUNT(*)     AS n\nFROM '/data/sales.parquet'\nGROUP BY product, year\nORDER BY year, product;" },
    { name: 'WHERE + GROUP BY',
      sql: "SELECT region, COUNT(*) AS n, SUM(revenue) AS rev\nFROM '/data/sales.csv'\nWHERE year >= 2023 AND qty > 100\nGROUP BY region\nORDER BY region;" },
    { name: 'Date functions (0.1.4)',
      sql: "SELECT DATE_TRUNC('quarter', CURRENT_DATE) AS q,\n       MONTHNAME(CURRENT_DATE) AS month,\n       DAYNAME(CURRENT_DATE)   AS day,\n       LAST_DAY(CURRENT_DATE)  AS eom;" },
];

function renderSnippetsView() {
    $('#snip-full').innerHTML = SNIPPETS.map((s, i) =>
        `<li data-idx="${i}"><span class="si">▸</span>${escapeHtml(s.name)}</li>`
    ).join('');
}

$('#snip-full').addEventListener('click', (e) => {
    const li = e.target.closest('li[data-idx]');
    if (!li) return;
    const s = SNIPPETS[+li.dataset.idx];
    setEditorValue(s.sql);
    runQuery();
});

document.addEventListener('click', (e) => {
    const li = e.target.closest('.snippet');
    if (!li || !li.dataset.sql) return;
    setEditorValue(li.dataset.sql);
    runQuery();
});

// ────────────────────────────────────────────────────────────
// Query execution & result rendering

function isNumericColumn(rows, col) {
    let seen = 0;
    for (const r of rows) {
        const v = r[col];
        if (v === null) continue;
        seen++;
        if (isNaN(Number(v))) return false;
        if (seen > 20) break;
    }
    return seen > 0;
}

function renderResult(res) {
    lastResult = res;
    const meta = $('#panel-meta');
    const table = $('#result-table');
    const empty = $('#result-empty');
    const dlBtn = $('#download-csv');

    if (res.error) {
        meta.innerHTML = `<span class="err">${escapeHtml(res.error)}</span>`;
        table.innerHTML = '';
        empty.style.display = '';
        empty.textContent = '(query failed - see Messages)';
        dlBtn.disabled = true;
        setRowStats(null, res.ms, true);
        switchPanel('messages');
        return;
    }

    const { columns, rows, ms, rowCount } = res;
    meta.innerHTML = `
        <span class="ok">${rowCount} row${rowCount === 1 ? '' : 's'}</span>
        <span>${ms.toFixed(1)} ms</span>
        <span>${columns.length} col${columns.length === 1 ? '' : 's'}</span>`;
    setRowStats(rowCount, ms);

    if (!rows.length) {
        table.innerHTML = '';
        empty.style.display = '';
        empty.textContent = 'Query returned 0 rows.';
        dlBtn.disabled = true;
        switchPanel('results');
        return;
    }
    empty.style.display = 'none';
    dlBtn.disabled = false;

    const numericCols = columns.map((_, c) => isNumericColumn(rows, c));

    let display = rows;
    if (sortState.col >= 0 && sortState.col < columns.length) {
        display = rows.slice();
        const c = sortState.col, d = sortState.dir, num = numericCols[c];
        display.sort((a, b) => {
            const av = a[c], bv = b[c];
            if (av === null && bv === null) return 0;
            if (av === null) return 1;
            if (bv === null) return -1;
            if (num) return d * (Number(av) - Number(bv));
            return d * String(av).localeCompare(String(bv));
        });
    }

    const thead = '<tr><th class="row-idx">#</th>' + columns.map((c, i) => {
        const arrow = sortState.col === i
            ? `<span class="sort-arrow">${sortState.dir === 1 ? '▲' : '▼'}</span>` : '';
        return `<th data-col="${i}">${escapeHtml(c)}${arrow}</th>`;
    }).join('') + '</tr>';

    const MAX = 1000;
    const slice = display.slice(0, MAX);
    const tbody = slice.map((r, ri) =>
        '<tr>' +
        `<td class="row-idx">${ri + 1}</td>` +
        r.map((v, ci) => {
            if (v === null) return '<td class="null">NULL</td>';
            return `<td class="${numericCols[ci] ? 'num' : ''}">${escapeHtml(v)}</td>`;
        }).join('') +
        '</tr>'
    ).join('');
    const more = display.length > MAX
        ? `<tr><td class="null" colspan="${columns.length + 1}">(${display.length - MAX} more rows truncated)</td></tr>`
        : '';
    table.innerHTML = `<table class="grid"><thead>${thead}</thead><tbody>${tbody}${more}</tbody></table>`;

    $$('thead th[data-col]', table).forEach((th) => {
        th.addEventListener('click', () => {
            const c = +th.dataset.col;
            if (sortState.col === c) sortState.dir = -sortState.dir;
            else sortState = { col: c, dir: 1 };
            renderResult(lastResult);
        });
    });

    switchPanel('results');
}

function switchPanel(name) {
    $$('.ptab').forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
    $$('.tab-panel').forEach((p) => p.classList.toggle('active', p.id === `${name}-panel`));
}
$$('.ptab').forEach((btn) => btn.addEventListener('click', () => switchPanel(btn.dataset.tab)));

// Run button click handler - the editor's own Mod-Enter keymap only fires
// when CodeMirror has focus, so the button needs its own binding.
$('#run').addEventListener('click', () => runQuery());

// Global Ctrl/Cmd+Enter - works even when focus is outside the editor
// (e.g. a snippet button was just clicked, or the user is on the results pane).
document.addEventListener('keydown', (e) => {
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') {
        e.preventDefault();
        runQuery();
    }
});

function runQuery() {
    if (!mod) return;
    const sqlText = editor.state.doc.toString().trim();
    if (!sqlText) return;
    setStatus('running…', 'ok');
    $('#run').disabled = true;
    setTimeout(() => {
        try {
            const stmts = sqlText.split(/;\s*(?:\r?\n|$)/).map((s) => s.trim()).filter(Boolean);
            let res;
            for (const s of stmts) {
                const json = mod.runQuery(s);
                res = JSON.parse(json);
                if (res.error) break;
            }
            sortState = { col: -1, dir: 1 };
            renderResult(res);
            pushHistory(sqlText, res);
            setStatus(res.error ? 'error' : 'ready', res.error ? 'err' : 'ok');
            log(res.error ? `Error: ${res.error}` : `Query OK, ${res.rowCount} row${res.rowCount === 1 ? '' : 's'} in ${res.ms.toFixed(1)} ms.`,
                res.error ? 'err' : 'ok');
        } catch (e) {
            renderResult({ error: String(e), ms: 0 });
            setStatus('error', 'err');
        } finally {
            $('#run').disabled = false;
        }
    }, 10);
}

// ────────────────────────────────────────────────────────────
// History

function pushHistory(sqlText, res) {
    const entry = {
        sql: sqlText,
        ms: res.ms,
        rowCount: res.rowCount ?? 0,
        error: res.error || null,
        at: new Date(),
    };
    history.unshift(entry);
    if (history.length > 50) history.length = 50;
    renderHistory();
}

function renderHistory() {
    if (!history.length) {
        $('#history-list').innerHTML = '<li style="color:var(--fg-fade);font-style:italic;cursor:default;">(empty)</li>';
        return;
    }
    $('#history-list').innerHTML = history.map((h, i) => {
        const firstLine = h.sql.split('\n').find((l) => l.trim() && !l.trim().startsWith('--')) || h.sql;
        const when = h.at.toTimeString().slice(0, 5);
        const meta = h.error
            ? `<span style="color:var(--error);">✕ ${when}</span>`
            : `<span style="color:var(--accent);">✓ ${h.rowCount} rows · ${h.ms.toFixed(1)}ms · ${when}</span>`;
        return `<li data-idx="${i}" title="${escapeHtml(h.sql)}">
            <div class="hist-sql">${escapeHtml(firstLine.trim())}</div>
            <div class="hist-meta">${meta}</div>
        </li>`;
    }).join('');
}

$('#history-list').addEventListener('click', (e) => {
    const li = e.target.closest('li[data-idx]');
    if (!li) return;
    setEditorValue(history[+li.dataset.idx].sql);
});
$('#clear-history').addEventListener('click', () => {
    history.length = 0;
    renderHistory();
});

// ────────────────────────────────────────────────────────────
// CSV export

$('#download-csv').addEventListener('click', () => {
    if (!lastResult || lastResult.error) return;
    const esc = (v) => {
        if (v === null) return '';
        const s = String(v);
        return /[,"\n]/.test(s) ? '"' + s.replace(/"/g, '""') + '"' : s;
    };
    const lines = [lastResult.columns.map(esc).join(',')];
    for (const r of lastResult.rows) lines.push(r.map(esc).join(','));
    const blob = new Blob([lines.join('\n')], { type: 'text/csv' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = 'slothdb-result.csv';
    a.click();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
});

// ────────────────────────────────────────────────────────────
// Resizable split

(function () {
    const split = $('#split');
    const gutter = $('#gutter');
    let dragging = false;
    gutter.addEventListener('mousedown', () => {
        dragging = true;
        gutter.classList.add('active');
        document.body.style.cursor = 'row-resize';
        document.body.style.userSelect = 'none';
    });
    document.addEventListener('mousemove', (e) => {
        if (!dragging) return;
        const rect = split.getBoundingClientRect();
        const y = e.clientY - rect.top;
        const top = Math.max(100, Math.min(rect.height - 100, y));
        split.style.gridTemplateRows = `${top}px 4px 1fr`;
    });
    document.addEventListener('mouseup', () => {
        if (!dragging) return;
        dragging = false;
        gutter.classList.remove('active');
        document.body.style.cursor = '';
        document.body.style.userSelect = '';
    });
})();

// ────────────────────────────────────────────────────────────
// Editor

const INITIAL_SQL = `-- SlothDB playground
-- Files in /data/ are already loaded. Try clicking a snippet on the left,
-- or Ctrl+Enter to run this query.

SELECT region, SUM(revenue) AS total
FROM '/data/sales.parquet'
GROUP BY region
ORDER BY region;
`;

function setEditorValue(text) {
    editor.dispatch({
        changes: { from: 0, to: editor.state.doc.length, insert: text },
    });
}

function mountEditor() {
    const runKey = keymap.of([{
        key: 'Mod-Enter',
        preventDefault: true,
        run: () => { runQuery(); return true; },
    }]);
    const state = EditorState.create({
        doc: INITIAL_SQL,
        extensions: [basicSetup, sql(), oneDark, runKey, keymap.of([indentWithTab])],
    });
    editor = new EditorView({ state, parent: $('#editor') });
}

// ────────────────────────────────────────────────────────────
// Demo data (fetched at boot)

const DEMO_SIZES = { csv: 0, parquet: 0 };

async function loadDemoData() {
    const loadOne = async (url, memPath, kind) => {
        const r = await fetch(url);
        if (!r.ok) throw new Error(`demo fetch ${url}: ${r.status}`);
        const buf = new Uint8Array(await r.arrayBuffer());
        mod.FS.writeFile(memPath, buf);
        DEMO_SIZES[kind] = buf.byteLength;
        return buf.byteLength;
    };
    try { mod.FS.mkdir('/data'); } catch (_) {}
    const [a, b] = await Promise.all([
        loadOne('data/sales.csv',     '/data/sales.csv',     'csv'),
        loadOne('data/sales.parquet', '/data/sales.parquet', 'parquet'),
    ]);
    log(`Demo data loaded: /data/sales.csv (${formatBytes(a)}), /data/sales.parquet (${formatBytes(b)}).`, 'ok');
}

// ────────────────────────────────────────────────────────────
// Boot

async function boot() {
    mountEditor();
    renderSnippetsView();
    renderHistory();
    setStatus('booting wasm…', 'boot');
    log('Booting SlothDB…');

    // Cache-bust slothdb.wasm via locateFile - without this the JS gets
    // the new hash via `?v=` but the wasm stays stuck on the old copy.
    mod = await createSlothDB({
        locateFile: (path) => path + '?v=' + BUILD_VERSION,
    });
    mod.openDatabase();

    await loadDemoData();

    $('#sb-version').textContent = `SlothDB v${mod.version()}`;
    $('#run').disabled = false;
    refreshFileList();
    setStatus('ready', 'ok');
    log(`SlothDB v${mod.version()} ready.`, 'ok');

    // Auto-run initial query so there's something visible on first paint.
    runQuery();
}

boot().catch((e) => {
    setStatus('boot failed', 'err');
    log(`Boot failed: ${e.message || e}`, 'err');
    console.error(e);
}).then(() => {
    // First-visit guided tour - runs once per browser, dismissable.
    setTimeout(() => maybeStartTour(), 600);
});

// ────────────────────────────────────────────────────────────
// Guided tour (first-visit onboarding)

const TOUR_KEY = 'slothdb-tour-v1';

const TOUR_STEPS = [
    {
        selector: '#editor',
        title: 'SQL editor',
        body: 'Write SQL here. A demo query is already loaded - press <kbd>Ctrl</kbd>+<kbd>Enter</kbd> or click Run to execute.',
        placement: 'right',
    },
    {
        selector: '.tree-group[data-group="data"]',
        title: 'Your data files',
        body: 'The demo CSV and Parquet are preloaded at <code>/data/</code>. Click any file to insert its path into the editor.',
        placement: 'right',
    },
    {
        selector: '.tree-action[data-tooltip]',
        title: 'Add your own files',
        body: 'Upload CSV, Parquet, JSON, Arrow, SQLite, or Excel. Files stay in your browser - nothing is uploaded to a server.',
        placement: 'bottom',
    },
    {
        selector: '.tree-group[data-group="snippets-mini"]',
        title: 'Snippet library',
        body: 'Ready-made queries - COUNT, GROUP BY, format comparisons, date functions. Click any to load and run.',
        placement: 'right',
    },
    {
        selector: '#run',
        title: 'Run your query',
        body: 'Click Run or press <kbd>Ctrl</kbd>+<kbd>Enter</kbd>. Results appear in the grid below. Column headers sort; the Export button saves as CSV.',
        placement: 'bottom',
    },
    {
        // Switch the sidebar to the .ask view so the user actually sees the
        // panel contents (SVG + docs link) while the spotlight rings the
        // activity-bar button. Without onEnter the explorer pane stays
        // active and the user only sees an icon light up.
        selector: '.act[data-view="ask"]',
        title: 'Natural language in the shell (.ask)',
        body: 'The CLI shell has a natural-language sub-REPL. At the <code>slothdb&gt;</code> prompt type <code>.ask</code> (no argument) to enter it. Every English question becomes SQL, shown to you, gated by <code>[Y/n]</code> before running. Rules-first; builds compiled with <code>-DSLOTHDB_ASK_MODEL=ON</code> fall back to a local Qwen model. Click the icon in this tour to see the demo, or open <a href="https://github.com/SouravRoy-ETL/slothdb/blob/main/docs/ASK.md" target="_blank" rel="noopener">docs/ASK.md</a>.',
        placement: 'right',
        onEnter: () => {
            // Click the .ask activity-bar button so the sidebar view
            // changes to the .ask panel while the tour highlights it.
            const btn = document.querySelector('.act[data-view="ask"]');
            if (btn) btn.click();
        },
    },
];

function maybeStartTour() {
    if (localStorage.getItem(TOUR_KEY)) return;
    startTour();
}

// Expose a way to replay the tour from the UI (About view button).
window.slothdbReplayTour = () => {
    localStorage.removeItem(TOUR_KEY);
    startTour();
};

function startTour() {
    // Bail if any target is missing (defensive - e.g. responsive hidden).
    for (const s of TOUR_STEPS) {
        if (!document.querySelector(s.selector)) return;
    }

    const overlay = document.createElement('div');
    overlay.className = 'tour-overlay';
    const spot = document.createElement('div');
    spot.className = 'tour-spotlight';
    const card = document.createElement('div');
    card.className = 'tour-card';
    document.body.append(overlay, spot, card);

    let idx = 0;

    function render() {
        const step = TOUR_STEPS[idx];
        // Let the step mutate the UI (e.g. switch sidebar view) before we
        // measure the target so the spotlight lands on the right position.
        if (typeof step.onEnter === 'function') step.onEnter();
        const target = document.querySelector(step.selector);
        const r = target.getBoundingClientRect();

        // Spotlight around the target.
        const pad = 6;
        spot.style.top    = `${r.top - pad}px`;
        spot.style.left   = `${r.left - pad}px`;
        spot.style.width  = `${r.width + pad * 2}px`;
        spot.style.height = `${r.height + pad * 2}px`;

        // Card contents.
        const isLast = idx === TOUR_STEPS.length - 1;
        card.innerHTML = `
            <div class="tour-step">${idx + 1} of ${TOUR_STEPS.length}</div>
            <h4>${step.title}</h4>
            <p>${step.body}</p>
            <div class="tour-actions">
                <button class="tour-skip">Skip tour</button>
                <div class="tour-nav">
                    ${idx > 0 ? '<button class="tour-prev">Back</button>' : ''}
                    <button class="tour-next">${isLast ? 'Got it' : 'Next ->'}</button>
                </div>
            </div>
        `;

        // Position card near the spotlight.
        positionCard(step.placement, r);

        card.querySelector('.tour-skip').onclick = finish;
        card.querySelector('.tour-next').onclick = () => {
            if (isLast) finish();
            else { idx++; render(); }
        };
        const prev = card.querySelector('.tour-prev');
        if (prev) prev.onclick = () => { idx--; render(); };
    }

    function positionCard(placement, targetRect) {
        // Make card visible to measure, then position.
        card.style.visibility = 'hidden';
        card.classList.add('show');
        const cw = card.offsetWidth;
        const ch = card.offsetHeight;
        card.style.visibility = '';
        const GAP = 14;

        let top, left;
        const centerX = targetRect.left + targetRect.width / 2;
        const centerY = targetRect.top  + targetRect.height / 2;

        if (placement === 'right' && targetRect.right + GAP + cw < window.innerWidth - 8) {
            left = targetRect.right + GAP;
            top  = Math.max(12, Math.min(centerY - ch / 2, window.innerHeight - ch - 12));
        } else if (placement === 'bottom') {
            top  = targetRect.bottom + GAP;
            left = Math.max(12, Math.min(centerX - cw / 2, window.innerWidth - cw - 12));
            // Flip if would overflow bottom.
            if (top + ch > window.innerHeight - 12) {
                top = Math.max(12, targetRect.top - GAP - ch);
            }
        } else {
            // Default: try right; fall back to below the target.
            if (targetRect.right + GAP + cw < window.innerWidth - 8) {
                left = targetRect.right + GAP;
                top  = Math.max(12, Math.min(centerY - ch / 2, window.innerHeight - ch - 12));
            } else {
                top  = targetRect.bottom + GAP;
                left = Math.max(12, Math.min(centerX - cw / 2, window.innerWidth - cw - 12));
            }
        }

        card.style.top  = `${top}px`;
        card.style.left = `${left}px`;
    }

    function finish() {
        overlay.remove();
        spot.remove();
        card.remove();
        localStorage.setItem(TOUR_KEY, '1');
        window.removeEventListener('resize', render);
        document.removeEventListener('keydown', onKey);
    }

    function onKey(e) {
        if (e.key === 'Escape') { finish(); }
        else if (e.key === 'Enter' || e.key === 'ArrowRight') {
            if (idx < TOUR_STEPS.length - 1) { idx++; render(); } else finish();
        }
        else if (e.key === 'ArrowLeft') {
            if (idx > 0) { idx--; render(); }
        }
    }

    window.addEventListener('resize', render);
    document.addEventListener('keydown', onKey);
    render();
}
