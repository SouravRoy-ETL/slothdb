// SlothDB playground — VS Code-style IDE for SQL against local files.
// Boots slothdb.wasm, fetches demo CSV/Parquet, wires activity bar / editor / panel / status bar.

import createSlothDB from './slothdb.js';
import {
    EditorView, basicSetup, keymap, EditorState,
    indentWithTab, sql, oneDark,
} from './vendor/cm.js';

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
    $('#sb-rows').textContent = err ? 'error' : (rows != null ? `${rows} rows` : '—');
    $('#sb-time').textContent = ms != null ? `${ms.toFixed(1)} ms` : '—';
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
      sql: "-- Same deterministic 1,000-row dataset, two formats.\nSELECT 'csv' AS fmt, COUNT(*) AS n, SUM(revenue) AS rev FROM '/data/sales.csv'\nUNION ALL\nSELECT 'parquet', COUNT(*), SUM(revenue) FROM '/data/sales.parquet';" },
    { name: 'Two-column GROUP BY',
      sql: "SELECT product, year,\n       SUM(revenue) AS rev,\n       SUM(qty)     AS qty,\n       COUNT(*)     AS n\nFROM '/data/sales.parquet'\nGROUP BY product, year\nORDER BY year, product;" },
    { name: 'WHERE + GROUP BY',
      sql: "SELECT region, COUNT(*) AS n, SUM(revenue) AS rev\nFROM '/data/sales.csv'\nWHERE year >= 2023 AND qty > 100\nGROUP BY region\nORDER BY region;" },
    { name: 'Date functions (0.1.4)',
      sql: "SELECT DATE_TRUNC('quarter', CURRENT_DATE) AS quarter_start,\n       MONTHNAME(CURRENT_DATE) AS month_name,\n       DAYNAME(CURRENT_DATE)   AS day_name,\n       LAST_DAY(CURRENT_DATE)  AS last_day;" },
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
        empty.textContent = '(query failed — see Messages)';
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

    mod = await createSlothDB();
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
});
