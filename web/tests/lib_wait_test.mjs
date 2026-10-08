// Node test of lib.mjs' waitState against a fake page (no browser). M1 close-out I-1: the recoverable 'area-error' state (C-N1) was not
// in the stop list, so a failed area load waited out the whole 60-120 s timeout. Run: cd web && timeout 60 node --test tests/lib_wait_test.mjs
import { test } from 'node:test';
import assert from 'node:assert/strict';
import { waitState } from './lib.mjs';

// A fake Playwright page whose window.__lomn is `lomn`. waitForFunction evaluates the predicate ONCE: a state outside the stop list is a
// timeout (what the real page does after `timeout` ms), reported at once so a missing state fails the test in microseconds.
const fakePage = (lomn) => ({
  waitForFunction: async (fn, arg, opts) => {
    globalThis.window = { __lomn: lomn };
    if (!fn(arg)) throw new Error(`Timeout ${opts.timeout}ms exceeded`);
  },
  evaluate: async (fn) => { globalThis.window = { __lomn: lomn }; return fn(); },
});

test('I-1 waitState: a page in area-error ends a wait for area-ready at once, with the page\'s message', async () => {
  const page = fakePage({ state: 'area-error', message: 'Could not load levt/test: area: blk: bad magic. Pick an area to try again.' });
  await assert.rejects(waitState(page, ['area-ready'], 120000), (e) => /area-error/.test(e.message) && /Could not load levt\/test: area: blk: bad magic/.test(e.message) && !/Timeout/.test(e.message));
});
test('I-1 waitState: a caller that lists area-error gets the state back (the C-N1 section); error is still returned, not thrown', async () => {
  const r = await waitState(fakePage({ state: 'area-error', message: 'm' }), ['area-error', 'area-ready']);
  assert.equal(r.state, 'area-error');
  const e = await waitState(fakePage({ state: 'error', message: 'm', error: 'boom' }), ['area-ready']);
  assert.equal(e.state, 'error');
  assert.equal(e.error, 'boom');
});
test('I-1 waitState: area-ready and the other waited states are unchanged; an unrelated state still times out', async () => {
  assert.equal((await waitState(fakePage({ state: 'area-ready', message: 'levt/test' }), ['area-ready'])).state, 'area-ready');
  assert.equal((await waitState(fakePage({ state: 'need-files', message: '' }), ['need-files'])).state, 'need-files');
  await assert.rejects(waitState(fakePage({ state: 'loading-area', message: '' }), ['area-ready'], 5), /Timeout/);
});
