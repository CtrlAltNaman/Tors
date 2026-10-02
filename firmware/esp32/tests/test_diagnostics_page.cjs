const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');

// Exercise the actual flash-resident page's script, with browser I/O substituted.
const source = fs.readFileSync(path.join(__dirname, '../main/device_diagnostics.c'), 'utf8');
const literal = source.slice(source.indexOf('static const char page[] ='), source.indexOf('\nvoid device_diagnostics_publish'));
const html = [...literal.matchAll(/"(?:[^"\\]|\\.)*"/g)].map(m => JSON.parse(m[0])).join('');
const script = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = {connection: {textContent: ''}, logs: {textContent: ''}};
const timers = [];
let requests = 0;
let fail = false;
const sandbox = {
    document: {getElementById: id => elements[id]},
    AbortController,
    setTimeout: (fn, ms) => { const timer = {fn, ms}; timers.push(timer); return timer; },
    clearTimeout: timer => { timer.cleared = true; },
    fetch: async (url, options) => {
        requests++;
        assert.equal(url, '/logs');
        assert.equal(options.cache, 'no-store');
        assert(options.signal instanceof AbortSignal);
        if (fail) throw Error('disconnected');
        return {ok: true, text: async () => 'RAM used=1B\nMIC frames=250\n<script>untrusted</script>'};
    },
};

(async () => {
    vm.runInNewContext(script, sandbox);
    await new Promise(resolve => setImmediate(resolve));
    assert.equal(requests, 1);
    assert(elements.logs.textContent.includes('MIC frames=250'));
    assert(elements.logs.textContent.includes('<script>untrusted</script>')); // Remains text, not markup.
    assert(elements.connection.textContent.startsWith('Connected'));
    assert(timers.some(t => t.ms === 4000 && t.cleared));
    const retry = timers.find(t => t.ms === 5000);
    assert(retry);
    fail = true;
    await retry.fn();
    assert.equal(requests, 2);
    assert(elements.connection.textContent.includes('stale'));
    assert(elements.logs.textContent.includes('MIC frames=250')); // Preserve last data but mark stale.
    assert.equal(timers.filter(t => t.ms === 5000).length, 2);
    console.log('diagnostics page: polling, safe text, refresh scheduling and disconnect display passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
